/**
 * @file tools/shell_probe.cpp
 * @brief Phase 0 validation tool for the Apollonio "console shell".
 *
 * This is a standalone diagnostic executable, it is NOT part of the streaming pipeline.
 * It verifies on a real host the assumptions the window supervisor will rely on:
 *   - which monitor is the Apollo virtual display;
 *   - how Steam Big Picture / Steam games show up (exe, window class, title, RunningAppID);
 *   - whether a user-session process can reliably own the foreground (SetForegroundWindow);
 *   - whether a black "backdrop" window + Z-order management hides everything unmanaged;
 *   - how fast we can return to Big Picture when a game closes;
 *   - which signals (cursor visibility, loaded input modules) predict when a mouse is needed.
 *
 * Usage: shell_probe <command> [options]   (run "shell_probe help")
 * Every line printed is also written to a log file next to the executable.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

  // ---------------------------------------------------------------------------
  // Logging
  // ---------------------------------------------------------------------------

  FILE *g_log = nullptr;

  std::string to_utf8(const std::wstring &w) {
    if (w.empty()) {
      return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
  }

  void logf(const wchar_t *fmt, ...) {
    wchar_t body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[4200];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

    auto utf8 = to_utf8(line);
    fwrite(utf8.data(), 1, utf8.size(), stdout);
    fflush(stdout);
    if (g_log) {
      fwrite(utf8.data(), 1, utf8.size(), g_log);
      fflush(g_log);
    }
  }

  // ---------------------------------------------------------------------------
  // Small helpers
  // ---------------------------------------------------------------------------

  std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t) std::towlower(c); });
    return s;
  }

  bool icontains(const std::wstring &haystack, const std::wstring &needle) {
    return lower(haystack).find(lower(needle)) != std::wstring::npos;
  }

  std::wstring basename(const std::wstring &path) {
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
  }

  std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
  }

  ULONGLONG now_ms() {
    return GetTickCount64();
  }

  ULONGLONG filetime_now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  }

  std::wstring process_path(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
      return L"<access denied>";
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD size = _countof(buf);
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
      out.assign(buf, size);
    }
    CloseHandle(h);
    return out;
  }

  // Process creation time as FILETIME ticks (0 if unavailable)
  ULONGLONG process_start(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
      return 0;
    }
    FILETIME c, e, k, u;
    ULONGLONG r = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
      r = ((ULONGLONG) c.dwHighDateTime << 32) | c.dwLowDateTime;
    }
    CloseHandle(h);
    return r;
  }

  // Lower-cased base names of all modules loaded in a process (empty on access denied)
  std::vector<std::wstring> process_modules(DWORD pid, bool &denied) {
    std::vector<std::wstring> out;
    denied = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
      denied = true;
      return out;
    }
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (EnumProcessModulesEx(h, mods.data(), (DWORD) (mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
      size_t count = std::min<size_t>(needed / sizeof(HMODULE), mods.size());
      for (size_t i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH];
        if (GetModuleBaseNameW(h, mods[i], name, MAX_PATH)) {
          out.push_back(lower(name));
        }
      }
    } else {
      denied = true;
    }
    CloseHandle(h);
    return out;
  }

  // ---------------------------------------------------------------------------
  // Window inspection & classification
  // ---------------------------------------------------------------------------

  enum class Role {
    Ignore,  ///< not a user-facing top-level window (tool windows, cloaked, shell...)
    Base,  ///< Steam Big Picture
    Game,  ///< window belonging to the game Steam reports as running
    Hidden,  ///< any other top-level window (launchers, explorer, popups...)
    Backdrop,  ///< our own black backdrop
  };

  const wchar_t *role_name(Role r) {
    switch (r) {
      case Role::Ignore:
        return L"IGNORE";
      case Role::Base:
        return L"BASE";
      case Role::Game:
        return L"GAME";
      case Role::Hidden:
        return L"HIDDEN";
      case Role::Backdrop:
        return L"BACKDROP";
    }
    return L"?";
  }

  struct WinInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    DWORD tid = 0;
    std::wstring path, exe, cls, title;
    RECT rc {};
    LONG style = 0, exstyle = 0;
    bool visible = false, cloaked = false, iconic = false, owned = false;
  };

  WinInfo inspect(HWND hwnd) {
    WinInfo w;
    w.hwnd = hwnd;
    if (!hwnd || !IsWindow(hwnd)) {
      return w;
    }
    w.tid = GetWindowThreadProcessId(hwnd, &w.pid);
    w.path = process_path(w.pid);
    w.exe = lower(basename(w.path));
    wchar_t buf[512];
    if (GetClassNameW(hwnd, buf, _countof(buf))) {
      w.cls = buf;
    }
    int len = GetWindowTextW(hwnd, buf, _countof(buf));
    w.title.assign(buf, std::max(len, 0));
    GetWindowRect(hwnd, &w.rc);
    w.style = GetWindowLongW(hwnd, GWL_STYLE);
    w.exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    w.visible = IsWindowVisible(hwnd);
    w.iconic = IsIconic(hwnd);
    w.owned = GetWindow(hwnd, GW_OWNER) != nullptr;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
      w.cloaked = cloaked != 0;
    }
    return w;
  }

  std::wstring describe(const WinInfo &w) {
    wchar_t buf[1024];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"hwnd=%p pid=%lu exe=%s class=\"%s\" title=\"%s\" rect=[%ld,%ld %ldx%ld] style=%08lx ex=%08lx%s%s%s%s", (void *) w.hwnd, w.pid, w.exe.c_str(), w.cls.c_str(), w.title.c_str(), w.rc.left, w.rc.top, w.rc.right - w.rc.left, w.rc.bottom - w.rc.top, (unsigned long) w.style, (unsigned long) w.exstyle, w.visible ? L"" : L" [invisible]", w.cloaked ? L" [cloaked]" : L"", w.iconic ? L" [minimized]" : L"", w.owned ? L" [owned]" : L"");
    return buf;
  }

  // ---------------------------------------------------------------------------
  // Steam state
  // ---------------------------------------------------------------------------

  struct SteamState {
    DWORD running_appid = 0;
    DWORD steam_pid = 0;
    std::wstring app_name;
  };

  DWORD reg_dword(HKEY root, const std::wstring &key, const wchar_t *value) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) {
      return 0;
    }
    return v;
  }

  std::wstring reg_string(HKEY root, const std::wstring &key, const wchar_t *value) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
      return {};
    }
    return buf;
  }

  SteamState read_steam() {
    SteamState s;
    s.running_appid = reg_dword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID");
    s.steam_pid = reg_dword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid");
    if (s.running_appid) {
      s.app_name = reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\Apps\\" + std::to_wstring(s.running_appid), L"Name");
    }
    return s;
  }

  // ---------------------------------------------------------------------------
  // Global probe state
  // ---------------------------------------------------------------------------

  struct Options {
    std::wstring bp_title = L"Big Picture";  ///< substring of the Big Picture window title
    std::wstring game_exe;  ///< optional: treat this exe as GAME (non-Steam tests)
    int monitor = -1;  ///< -1 = auto (virtual display if found, else primary)
    int seconds = 300;  ///< auto-exit timeout, 0 = never
    bool fit = false;  ///< backdrop: force managed windows borderless to the monitor
    bool park_cursor = false;  ///< backdrop: park the cursor when mode is CONTROLLER
    bool all = false;  ///< list: show ignored windows too
  } g_opt;

  struct MonitorInfo {
    HMONITOR handle;
    RECT rc;
    bool primary;
    std::wstring device, adapter, name;
  };

  struct State {
    SteamState steam;
    ULONGLONG appid_changed_ft = 0;  ///< FILETIME of the last RunningAppID change
    HWND backdrop = nullptr;
    RECT target_rc {};
    HWND managed = nullptr;  ///< last managed (BASE/GAME) window that had focus
    ULONGLONG managed_lost_at = 0;  ///< tick when the managed window disappeared
    HWND last_fg = nullptr;
    std::wstring last_mode;
    std::map<HWND, WinInfo> known;  ///< last info of windows we logged (for DESTROY events)
    std::map<HWND, std::vector<ULONGLONG>> steals;  ///< foreground steals per unmanaged window
    std::map<DWORD, std::pair<ULONGLONG, std::vector<std::wstring>>> module_cache;
    bool enforce = false;  ///< backdrop mode active
  } g;

  std::vector<MonitorInfo> enum_monitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
      auto &v = *reinterpret_cast<std::vector<MonitorInfo> *>(lp);
      MONITORINFOEXW mi {};
      mi.cbSize = sizeof(mi);
      if (!GetMonitorInfoW(hm, &mi)) {
        return TRUE;
      }
      MonitorInfo m {hm, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.szDevice, L"", L""};
      DISPLAY_DEVICEW dd {};
      dd.cb = sizeof(dd);
      for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (m.device == dd.DeviceName) {
          m.adapter = dd.DeviceString;
          break;
        }
        dd.cb = sizeof(dd);
      }
      DISPLAY_DEVICEW md {};
      md.cb = sizeof(md);
      if (EnumDisplayDevicesW(mi.szDevice, 0, &md, 0)) {
        m.name = md.DeviceString;
      }
      v.push_back(m);
      return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
  }

  bool is_shell_class(const std::wstring &cls) {
    static const std::set<std::wstring> shell = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow"};
    return shell.count(cls) > 0;
  }

  bool is_steam_exe(const std::wstring &exe) {
    return exe == L"steam.exe" || exe == L"steamwebhelper.exe" || exe == L"gameoverlayui.exe" || exe == L"gameoverlayui64.exe";
  }

  Role classify(const WinInfo &w) {
    if (!w.hwnd) {
      return Role::Ignore;
    }
    if (w.hwnd == g.backdrop) {
      return Role::Backdrop;
    }
    if (!w.visible || w.cloaked || (w.exstyle & WS_EX_TOOLWINDOW) || is_shell_class(w.cls) || GetAncestor(w.hwnd, GA_ROOT) != w.hwnd) {
      return Role::Ignore;
    }
    if (w.rc.right - w.rc.left <= 1 || w.rc.bottom - w.rc.top <= 1) {
      return Role::Ignore;
    }
    if (is_steam_exe(w.exe) && icontains(w.title, g_opt.bp_title)) {
      return Role::Base;
    }
    if (!g_opt.game_exe.empty() && w.exe == lower(g_opt.game_exe)) {
      return Role::Game;
    }
    if (g.steam.running_appid && !is_steam_exe(w.exe) && w.exe != L"explorer.exe") {
      // Steam games live under steamapps\common; non-Steam shortcuts are recognised by start time
      if (icontains(w.path, L"\\steamapps\\common\\")) {
        return Role::Game;
      }
      ULONGLONG started = process_start(w.pid);
      if (started && g.appid_changed_ft && started + 10'000'000ULL >= g.appid_changed_ft) {  // 1s tolerance
        return Role::Game;
      }
    }
    return Role::Hidden;
  }

  bool is_managed(Role r) {
    return r == Role::Base || r == Role::Game;
  }

  // Find the best window for a role (largest visible, unowned)
  HWND find_window(Role wanted) {
    struct Ctx {
      Role wanted;
      HWND best = nullptr;
      LONG best_area = 0;
    } ctx {wanted};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
      auto &c = *reinterpret_cast<Ctx *>(lp);
      auto w = inspect(h);
      if (w.owned || classify(w) != c.wanted) {
        return TRUE;
      }
      LONG area = (w.rc.right - w.rc.left) * (w.rc.bottom - w.rc.top);
      if (area > c.best_area) {
        c.best_area = area;
        c.best = h;
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
  }

  // ---------------------------------------------------------------------------
  // Cursor / input mode heuristics
  // ---------------------------------------------------------------------------

  const std::vector<std::wstring> &cached_modules(DWORD pid, bool &denied) {
    auto &entry = g.module_cache[pid];
    denied = false;
    if (entry.first == 0 || now_ms() - entry.first > 5000) {
      entry.second = process_modules(pid, denied);
      entry.first = now_ms();
      if (denied) {
        entry.second = {L"<denied>"};
      }
    }
    denied = !entry.second.empty() && entry.second[0] == L"<denied>";
    return entry.second;
  }

  std::wstring pad_modules(const std::vector<std::wstring> &mods) {
    static const wchar_t *pads[] = {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll", L"xinputuap.dll", L"gameinput.dll", L"sdl2.dll", L"sdl3.dll", L"dinput8.dll", L"windows.gaming.input.dll", L"steam_api64.dll", L"steam_api.dll"};
    std::wstring out;
    for (auto p : pads) {
      if (std::find(mods.begin(), mods.end(), p) != mods.end()) {
        out += (out.empty() ? L"" : L",") + std::wstring(p);
      }
    }
    return out.empty() ? L"none" : out;
  }

  bool has_real_pad_api(const std::wstring &pads) {
    return pads.find(L"xinput") != std::wstring::npos || pads.find(L"gameinput") != std::wstring::npos || pads.find(L"sdl") != std::wstring::npos || pads.find(L"windows.gaming.input") != std::wstring::npos;
  }

  // Returns the suggested mode and fills a human-readable reason
  std::wstring suggest_mode(const WinInfo &fg, Role role, std::wstring &reason) {
    CURSORINFO ci {};
    ci.cbSize = sizeof(ci);
    GetCursorInfo(&ci);
    bool showing = (ci.flags & CURSOR_SHOWING) != 0;
    bool over_fg = PtInRect(&fg.rc, ci.ptScreenPos);
    bool denied = false;
    auto pads = role == Role::Game ? pad_modules(cached_modules(fg.pid, denied)) : std::wstring(L"n/a");
    if (denied) {
      pads = L"<access denied: run elevated>";
    }

    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"role=%s cursor_showing=%d suppressed=%d hcursor=%p over_fg=%d pad_modules=%s", role_name(role), showing, (ci.flags & CURSOR_SUPPRESSED) != 0, (void *) ci.hCursor, over_fg, pads.c_str());
    reason = buf;

    if (role == Role::Base) {
      return L"CONTROLLER";
    }
    if (role == Role::Hidden) {
      return L"POINTER";
    }
    if (role == Role::Game) {
      if (showing && over_fg && !has_real_pad_api(pads)) {
        return L"POINTER";
      }
      if (showing && over_fg) {
        return L"CONTROLLER?";  // ambiguous: pad API loaded but cursor visible (menus, launchers in-process)
      }
      return L"CONTROLLER";
    }
    return L"-";
  }

  // ---------------------------------------------------------------------------
  // Foreground control
  // ---------------------------------------------------------------------------

  // Tries progressively more aggressive techniques, logs which one worked
  bool force_foreground(HWND target, const wchar_t *why) {
    if (!target || !IsWindow(target)) {
      return false;
    }
    ULONGLONG t0 = now_ms();
    if (IsIconic(target)) {
      ShowWindow(target, SW_RESTORE);
    }
    auto ok = [&]() {
      return GetForegroundWindow() == target;
    };

    const wchar_t *method = nullptr;
    if (SetForegroundWindow(target) && ok()) {
      method = L"SetForegroundWindow";
    }
    if (!method) {
      HWND fg = GetForegroundWindow();
      DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
      DWORD me = GetCurrentThreadId();
      bool attached = fg_tid && fg_tid != me && AttachThreadInput(me, fg_tid, TRUE);
      BringWindowToTop(target);
      SetForegroundWindow(target);
      if (attached) {
        AttachThreadInput(me, fg_tid, FALSE);
      }
      if (ok()) {
        method = L"AttachThreadInput";
      }
    }
    if (!method) {
      INPUT in[2] {};
      in[0].type = in[1].type = INPUT_KEYBOARD;
      in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
      in[1].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(2, in, sizeof(INPUT));
      SetForegroundWindow(target);
      if (ok()) {
        method = L"AltKeyTrick";
      }
    }
    if (!method) {
      SwitchToThisWindow(target, TRUE);
      Sleep(50);
      if (ok()) {
        method = L"SwitchToThisWindow";
      }
    }
    logf(L"FOCUS %s -> %s via %s in %llums (%s)", method ? L"OK" : L"FAILED", describe(inspect(target)).c_str(), method ? method : L"<none>", now_ms() - t0, why);
    return method != nullptr;
  }

  void place_backdrop_below(HWND managed) {
    if (!g.backdrop) {
      return;
    }
    if (!SetWindowPos(g.backdrop, managed, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
      logf(L"ZORDER backdrop-below-managed FAILED err=%lu", GetLastError());
    }
  }

  void push_below_backdrop(const WinInfo &w) {
    if (!g.backdrop) {
      return;
    }
    if (w.exstyle & WS_EX_TOPMOST) {
      SetWindowPos(w.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (!SetWindowPos(w.hwnd, g.backdrop, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
      logf(L"ZORDER push-below-backdrop FAILED err=%lu for %s", GetLastError(), describe(w).c_str());
    }
  }

  void fit_to_target(const WinInfo &w) {
    RECT &t = g.target_rc;
    if (w.rc.left == t.left && w.rc.top == t.top && w.rc.right == t.right && w.rc.bottom == t.bottom) {
      return;
    }
    LONG style = w.style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowLongW(w.hwnd, GWL_STYLE, style);
    BOOL r = SetWindowPos(w.hwnd, nullptr, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    logf(L"FIT %s -> [%ld,%ld %ldx%ld] %s", w.exe.c_str(), t.left, t.top, t.right - t.left, t.bottom - t.top, r ? L"ok" : L"FAILED");
  }

  void return_to_base(const wchar_t *why) {
    HWND game = find_window(Role::Game);
    HWND target = game ? game : find_window(Role::Base);
    if (!target) {
      logf(L"RETURN no BASE/GAME window found (is Big Picture running? title hint=\"%s\")", g_opt.bp_title.c_str());
      return;
    }
    if (force_foreground(target, why)) {
      place_backdrop_below(target);
      g.managed = target;
      if (g.managed_lost_at) {
        logf(L"METRIC return-latency=%llums (managed window lost -> %s focused)", now_ms() - g.managed_lost_at, game ? L"GAME" : L"BASE");
        g.managed_lost_at = 0;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Event handling (shared by watch and backdrop)
  // ---------------------------------------------------------------------------

  void on_foreground(HWND hwnd) {
    auto w = inspect(hwnd);
    Role r = classify(w);
    g.known[hwnd] = w;
    logf(L"FOREGROUND %-8s %s", role_name(r), describe(w).c_str());
    g.last_fg = hwnd;

    if (!g.enforce) {
      return;
    }
    if (is_managed(r)) {
      g.managed = hwnd;
      g.managed_lost_at = 0;
      place_backdrop_below(hwnd);
      if (g_opt.fit) {
        fit_to_target(w);
      }
      return;
    }
    if (r == Role::Hidden) {
      auto &list = g.steals[hwnd];
      ULONGLONG now = now_ms();
      list.erase(std::remove_if(list.begin(), list.end(), [&](ULONGLONG t) { return now - t > 10000; }), list.end());
      list.push_back(now);
      if (list.size() > 5) {
        logf(L"PERSISTENT_UNMANAGED %s stole focus %zu times in 10s: leaving it alone (needs user input?)", w.exe.c_str(), list.size());
        return;
      }
      push_below_backdrop(w);
    }
    // HIDDEN, IGNORE (desktop/taskbar) or our backdrop got focus: give it back to the managed app
    if (g.managed && IsWindow(g.managed)) {
      force_foreground(g.managed, L"restore after unmanaged foreground");
    } else {
      return_to_base(L"no managed window");
    }
  }

  void CALLBACK win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD) {
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF || !hwnd) {
      return;
    }
    switch (event) {
      case EVENT_SYSTEM_FOREGROUND:
        on_foreground(hwnd);
        break;
      case EVENT_OBJECT_SHOW:
        {
          if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
            return;
          }
          auto w = inspect(hwnd);
          Role r = classify(w);
          if (r == Role::Ignore || r == Role::Backdrop) {
            return;
          }
          g.known[hwnd] = w;
          logf(L"SHOW       %-8s %s", role_name(r), describe(w).c_str());
          if (g.enforce && r == Role::Hidden && GetForegroundWindow() != hwnd) {
            push_below_backdrop(w);
          }
          break;
        }
      case EVENT_OBJECT_DESTROY:
        {
          auto it = g.known.find(hwnd);
          if (it == g.known.end()) {
            return;
          }
          logf(L"DESTROY    %s", describe(it->second).c_str());
          g.known.erase(it);
          g.steals.erase(hwnd);
          if (hwnd == g.managed) {
            g.managed = nullptr;
            g.managed_lost_at = now_ms();
            if (g.enforce) {
              return_to_base(L"managed window destroyed");
            }
          }
          break;
        }
      case EVENT_SYSTEM_MINIMIZESTART:
        if (hwnd == g.managed) {
          logf(L"MINIMIZE   managed window minimized: %s", describe(inspect(hwnd)).c_str());
        }
        break;
      default:
        break;
    }
  }

  void poll_tick() {
    // Steam state
    auto s = read_steam();
    if (s.running_appid != g.steam.running_appid || s.steam_pid != g.steam.steam_pid) {
      g.appid_changed_ft = filetime_now();
      logf(L"STEAM      RunningAppID %lu -> %lu name=\"%s\" steam_pid=%lu", g.steam.running_appid, s.running_appid, s.app_name.c_str(), s.steam_pid);
      g.steam = s;
      g.module_cache.clear();
    }
    // Cursor / input mode suggestion
    HWND fg = GetForegroundWindow();
    auto w = inspect(fg);
    Role r = classify(w);
    std::wstring reason;
    auto mode = suggest_mode(w, r, reason);
    if (mode != g.last_mode) {
      logf(L"MODE       %s -> %s  [%s] fg=%s", g.last_mode.empty() ? L"-" : g.last_mode.c_str(), mode.c_str(), reason.c_str(), w.exe.c_str());
      g.last_mode = mode;
      if (g.enforce && g_opt.park_cursor && mode.rfind(L"CONTROLLER", 0) == 0) {
        SetCursorPos(g.target_rc.right - 1, g.target_rc.bottom - 1);
        logf(L"CURSOR     parked at bottom-right of target monitor");
      }
    }
    // Managed window vanished without a DESTROY we saw (e.g. hidden instead of destroyed)
    if (g.enforce && g.managed && (!IsWindow(g.managed) || !IsWindowVisible(g.managed))) {
      g.managed = nullptr;
      g.managed_lost_at = now_ms();
      return_to_base(L"managed window gone/hidden");
    }
  }

  // ---------------------------------------------------------------------------
  // Commands
  // ---------------------------------------------------------------------------

  void log_context() {
    wchar_t user[256];
    DWORD n = _countof(user);
    if (!GetUserNameW(user, &n)) {
      wcscpy_s(user, L"?");
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    TOKEN_ELEVATION elev {};
    DWORD sz = 0;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
      GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &sz);
      CloseHandle(tok);
    }
    wchar_t desk[128] = L"?";
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desk, sizeof(desk), nullptr);
    logf(L"CONTEXT    user=%s session=%lu elevated=%d desktop=%s", user, session, (int) elev.TokenIsElevated, desk);
    g.steam = read_steam();
    logf(L"STEAM      RunningAppID=%lu name=\"%s\" steam_pid=%lu", g.steam.running_appid, g.steam.app_name.c_str(), g.steam.steam_pid);
  }

  int cmd_monitors() {
    auto mons = enum_monitors();
    for (size_t i = 0; i < mons.size(); ++i) {
      auto &m = mons[i];
      logf(L"MONITOR #%zu %s%s rect=[%ld,%ld %ldx%ld] adapter=\"%s\" monitor=\"%s\"", i, m.device.c_str(), m.primary ? L" (primary)" : L"", m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, m.adapter.c_str(), m.name.c_str());
    }
    return 0;
  }

  int cmd_list() {
    EnumWindows([](HWND h, LPARAM) -> BOOL {
      auto w = inspect(h);
      Role r = classify(w);
      if (r != Role::Ignore || (g_opt.all && w.visible)) {
        logf(L"WINDOW %-8s %s", role_name(r), describe(w).c_str());
      }
      return TRUE;
    }, 0);
    return 0;
  }

  int cmd_focus(const std::wstring &needle) {
    struct Ctx {
      std::wstring needle;
      HWND found = nullptr;
    } ctx {lower(needle)};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
      auto &c = *reinterpret_cast<Ctx *>(lp);
      auto w = inspect(h);
      if (classify(w) != Role::Ignore && (w.exe.find(c.needle) != std::wstring::npos || lower(w.title).find(c.needle) != std::wstring::npos)) {
        c.found = h;
        return FALSE;
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found) {
      logf(L"No window matching \"%s\"", needle.c_str());
      return 1;
    }
    return force_foreground(ctx.found, L"focus command") ? 0 : 2;
  }

  LRESULT CALLBACK backdrop_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) {
      return MA_NOACTIVATE;
    }
    if (msg == WM_SETCURSOR) {
      SetCursor(nullptr);
      return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  bool create_backdrop() {
    auto mons = enum_monitors();
    if (mons.empty()) {
      logf(L"No monitors found");
      return false;
    }
    int idx = g_opt.monitor;
    if (idx < 0) {
      for (size_t i = 0; i < mons.size() && idx < 0; ++i) {
        if (icontains(mons[i].adapter, L"virtual") || icontains(mons[i].name, L"virtual")) {
          idx = (int) i;
        }
      }
      for (size_t i = 0; i < mons.size() && idx < 0; ++i) {
        if (mons[i].primary) {
          idx = (int) i;
        }
      }
    }
    if (idx < 0 || idx >= (int) mons.size()) {
      logf(L"Invalid monitor index %d (run 'shell_probe monitors')", idx);
      return false;
    }
    g.target_rc = mons[idx].rc;
    logf(L"BACKDROP   target monitor #%d %s adapter=\"%s\"", idx, mons[idx].device.c_str(), mons[idx].adapter.c_str());

    WNDCLASSW wc {};
    wc.lpfnWndProc = backdrop_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ApollonioShellBackdrop";
    RegisterClassW(&wc);
    RECT &r = g.target_rc;
    g.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Apollonio Backdrop", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.backdrop) {
      logf(L"CreateWindowEx failed err=%lu", GetLastError());
      return false;
    }
    ShowWindow(g.backdrop, SW_SHOWNOACTIVATE);
    return true;
  }

  DWORD g_main_tid = 0;

  BOOL WINAPI ctrl_handler(DWORD) {
    PostThreadMessageW(g_main_tid, WM_QUIT, 0, 0);
    return TRUE;
  }

  int run_loop(bool enforce) {
    g.enforce = enforce;
    if (enforce && !create_backdrop()) {
      return 1;
    }
    g_main_tid = GetCurrentThreadId();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK hooks[] = {
      SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, win_event, 0, 0, flags),
      SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, win_event, 0, 0, flags),
      SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW, nullptr, win_event, 0, 0, flags),
    };
    for (auto h : hooks) {
      if (!h) {
        logf(L"SetWinEventHook failed err=%lu", GetLastError());
      }
    }

    logf(L"RUNNING    %s mode, exit with Ctrl+C or Ctrl+Alt+Q%s", enforce ? L"BACKDROP (enforcing)" : L"WATCH (passive)", g_opt.seconds ? (L", auto-exit after " + std::to_wstring(g_opt.seconds) + L"s").c_str() : L"");

    on_foreground(GetForegroundWindow());
    if (enforce) {
      return_to_base(L"startup");
    }

    SetTimer(nullptr, 1, 500, nullptr);
    ULONGLONG start = now_ms();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == WM_HOTKEY) {
        break;
      }
      if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
        poll_tick();
        if (g_opt.seconds && now_ms() - start > (ULONGLONG) g_opt.seconds * 1000) {
          logf(L"TIMEOUT    reached");
          break;
        }
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    for (auto h : hooks) {
      if (h) {
        UnhookWinEvent(h);
      }
    }
    if (g.backdrop) {
      DestroyWindow(g.backdrop);
    }
    logf(L"EXIT");
    return 0;
  }

  void usage() {
    logf(L"shell_probe - Apollonio console shell, phase 0 probe");
    logf(L"  monitors                       list monitors (find the virtual display)");
    logf(L"  list [--all]                   snapshot of top-level windows with their role");
    logf(L"  watch                          passive: log foreground/show/destroy, Steam, input-mode hints");
    logf(L"  backdrop                       ACTIVE: black backdrop + keep BASE/GAME in front, hide the rest");
    logf(L"  focus <exe-or-title>           try to bring one window to the foreground");
    logf(L"Options: --monitor N  --seconds S (0=never, default 300)  --bp-title \"text\"");
    logf(L"         --game-exe name.exe  --fit (force borderless to monitor)  --park-cursor");
  }

}  // namespace

int main() {
  SetConsoleOutputCP(CP_UTF8);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args(argv + 1, argv + argc);
  LocalFree(argv);

  std::wstring cmd = args.empty() ? L"help" : args[0];
  std::wstring positional;
  for (size_t i = 1; i < args.size(); ++i) {
    auto &a = args[i];
    auto next = [&]() -> std::wstring {
      return i + 1 < args.size() ? args[++i] : L"";
    };
    if (a == L"--monitor") {
      g_opt.monitor = _wtoi(next().c_str());
    } else if (a == L"--seconds") {
      g_opt.seconds = _wtoi(next().c_str());
    } else if (a == L"--bp-title") {
      g_opt.bp_title = next();
    } else if (a == L"--game-exe") {
      g_opt.game_exe = next();
    } else if (a == L"--fit") {
      g_opt.fit = true;
    } else if (a == L"--park-cursor") {
      g_opt.park_cursor = true;
    } else if (a == L"--all") {
      g_opt.all = true;
    } else {
      positional = a;
    }
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[MAX_PATH];
  _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s\\shell_probe_%s_%04u%02u%02u_%02u%02u%02u.log", exe_dir().c_str(), cmd.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  _wfopen_s(&g_log, name, L"wb");

  if (cmd == L"help" || cmd == L"--help" || cmd == L"-h") {
    usage();
    return 0;
  }
  log_context();
  if (g_log) {
    logf(L"LOGFILE    %s", name);
  }

  int rc = 1;
  if (cmd == L"monitors") {
    rc = cmd_monitors();
  } else if (cmd == L"list") {
    rc = cmd_list();
  } else if (cmd == L"watch") {
    rc = run_loop(false);
  } else if (cmd == L"backdrop") {
    rc = run_loop(true);
  } else if (cmd == L"focus") {
    rc = cmd_focus(positional);
  } else {
    usage();
  }
  if (g_log) {
    fclose(g_log);
  }
  return rc;
}
