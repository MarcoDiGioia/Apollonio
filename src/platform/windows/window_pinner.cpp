/**
 * @file src/platform/windows/window_pinner.cpp
 * @brief Keeps the windows of a streamed app on the session's virtual display.
 *
 * Windows places new windows on the primary display, so apps launched for a stream on a
 * virtual display often open on a physical monitor instead. While a session is active, a
 * background thread periodically looks for windows that belong to the app and moves the ones
 * that are not on the stream display. Other windows are never touched.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_map>

// platform includes
#include <windows.h>
#include <tlhelp32.h>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "window_pinner.h"

using namespace std::literals;

namespace window_pinner {
  namespace {
    constexpr auto SWEEP_INTERVAL = 100ms;
    constexpr auto DESKTOP_SYNC_INTERVAL = 1s;
    constexpr int MAX_MOVES_PER_WINDOW = 20;
    constexpr int MAX_ANCESTOR_DEPTH = 32;

    // Whether a pinned window has been seen this session, and when one was last seen
    std::atomic<bool> pinned_seen {false};
    std::atomic<std::chrono::steady_clock::rep> pinned_last_seen {0};

    // Helper processes of launchers that own their own long-lived windows (main client,
    // friends list, overlay). They are only pinned through a window title match.
    const std::vector<std::wstring> IGNORED_DESCENDANTS {
      L"steamwebhelper.exe",
      L"steamservice.exe",
      L"steamerrorreporter.exe",
      L"gameoverlayui.exe",
      L"gameoverlayui64.exe",
    };

    std::wstring to_lower(std::wstring str) {
      std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) {
        return (wchar_t) std::towlower(c);
      });
      return str;
    }

    bool contains(const std::vector<std::wstring> &list, const std::wstring &value) {
      return std::find(list.begin(), list.end(), value) != list.end();
    }

    ULONGLONG to_ull(const FILETIME &ft) {
      return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    /**
     * @return The creation time of the process, or 0 if it cannot be queried.
     */
    ULONGLONG process_creation_time(DWORD pid) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) {
        return 0;
      }

      FILETIME creation, exit, kernel, user;
      ULONGLONG result = 0;
      if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        result = to_ull(creation);
      }
      CloseHandle(process);

      return result;
    }

    struct process_entry_t {
      DWORD parent_pid;
      std::wstring exe;
    };

    using process_table_t = std::unordered_map<DWORD, process_entry_t>;

    process_table_t snapshot_processes() {
      process_table_t table;

      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) {
        return table;
      }

      PROCESSENTRY32W entry {};
      entry.dwSize = sizeof(entry);
      if (Process32FirstW(snapshot, &entry)) {
        do {
          table[entry.th32ProcessID] = {entry.th32ParentProcessID, to_lower(entry.szExeFile)};
        } while (Process32NextW(snapshot, &entry));
      }
      CloseHandle(snapshot);

      return table;
    }

    struct cached_match_t {
      ULONGLONG creation_time;
      bool pinned;
    };

    struct window_state_t {
      int moves = 0;
    };

    class pinner_t {
    public:
      explicit pinner_t(options_t options):
          _options(std::move(options)) {
        for (auto &exe : _options.parent_processes) {
          exe = to_lower(exe);
        }
        for (auto &title : _options.window_titles) {
          title = to_lower(title);
        }

        _session_start = _options.session_start ? _options.session_start : now();
        _own_pid = GetCurrentProcessId();
      }

      void run(std::atomic<bool> &stop, std::mutex &mutex, std::condition_variable &cv) {
        auto last_desktop_sync = std::chrono::steady_clock::time_point {};

        std::unique_lock lock(mutex);
        while (!stop) {
          lock.unlock();

          // Follow the input desktop, as the UAC prompt and lock screen switch it.
          auto now = std::chrono::steady_clock::now();
          if (now - last_desktop_sync >= DESKTOP_SYNC_INTERVAL) {
            platf::syncThreadDesktop();
            last_desktop_sync = now;
          }

          sweep();

          lock.lock();
          cv.wait_for(lock, SWEEP_INTERVAL, [&stop]() {
            return stop.load();
          });
        }
      }

    private:
      options_t _options;
      ULONGLONG _session_start;
      DWORD _own_pid;

      HMONITOR _target = nullptr;
      MONITORINFO _target_info {};

      std::unordered_map<DWORD, cached_match_t> _process_cache;
      std::unordered_map<HWND, window_state_t> _windows;
      process_table_t _processes;
      bool _processes_valid = false;
      bool _found_pinned = false;

      static BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
        auto self = reinterpret_cast<pinner_t *>(param);

        MONITORINFOEXW info {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, reinterpret_cast<LPMONITORINFO>(&info)) && _wcsicmp(info.szDevice, self->_options.display_name.c_str()) == 0) {
          self->_target = monitor;
          self->_target_info.cbSize = sizeof(MONITORINFO);
          self->_target_info.rcMonitor = info.rcMonitor;
          self->_target_info.rcWork = info.rcWork;
          return FALSE;
        }

        return TRUE;
      }

      static BOOL CALLBACK visit_window(HWND hwnd, LPARAM param) {
        reinterpret_cast<pinner_t *>(param)->visit(hwnd);
        return TRUE;
      }

      void sweep() {
        // The display may still be initializing, or its position may have changed.
        _target = nullptr;
        EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(this));
        if (!_target) {
          return;
        }

        _processes_valid = false;
        _found_pinned = false;
        EnumWindows(visit_window, reinterpret_cast<LPARAM>(this));

        if (_found_pinned) {
          pinned_last_seen = std::chrono::steady_clock::now().time_since_epoch().count();
          pinned_seen = true;
        }

        // Forget windows that no longer exist
        for (auto it = _windows.begin(); it != _windows.end();) {
          it = IsWindow(it->first) ? std::next(it) : _windows.erase(it);
        }
      }

      void visit(HWND hwnd) {
        if (!IsWindowVisible(hwnd)) {
          return;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == 0 || pid == _own_pid) {
          return;
        }

        if (!title_matches(hwnd) && !process_matches(pid)) {
          return;
        }

        // Minimized and already pinned windows still count as open app windows
        _found_pinned = true;

        if (IsIconic(hwnd) || MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == _target) {
          return;
        }

        auto &state = _windows[hwnd];
        if (state.moves >= MAX_MOVES_PER_WINDOW) {
          return;
        }

        if (++state.moves == MAX_MOVES_PER_WINDOW) {
          BOOST_LOG(warning) << "Window of PID ["sv << pid << "] keeps leaving the stream display, no longer pinning it"sv;
        }

        move_to_target(hwnd, pid);
      }

      bool title_matches(HWND hwnd) const {
        if (_options.window_titles.empty()) {
          return false;
        }

        wchar_t buffer[512];
        int length = GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer));
        if (length <= 0) {
          return false;
        }

        auto title = to_lower(std::wstring(buffer, length));
        return std::any_of(_options.window_titles.begin(), _options.window_titles.end(), [&title](const std::wstring &pattern) {
          return title.find(pattern) != std::wstring::npos;
        });
      }

      bool process_matches(DWORD pid) {
        auto creation_time = process_creation_time(pid);

        // Process IDs are reused, so the cache is keyed on the creation time as well
        auto cached = _process_cache.find(pid);
        if (cached != _process_cache.end() && cached->second.creation_time == creation_time) {
          return cached->second.pinned;
        }

        bool pinned = in_job(pid) || (creation_time > _session_start && descends_from_parent(pid, creation_time));
        _process_cache[pid] = {creation_time, pinned};

        return pinned;
      }

      bool in_job(DWORD pid) const {
        if (!_options.job) {
          return false;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
          return false;
        }

        BOOL result = FALSE;
        IsProcessInJob(process, _options.job, &result);
        CloseHandle(process);

        return result;
      }

      bool descends_from_parent(DWORD pid, ULONGLONG creation_time) {
        if (_options.parent_processes.empty()) {
          return false;
        }

        if (!_processes_valid) {
          _processes = snapshot_processes();
          _processes_valid = true;
        }

        auto self = _processes.find(pid);
        if (self == _processes.end() || contains(IGNORED_DESCENDANTS, self->second.exe)) {
          return false;
        }

        DWORD current = pid;
        ULONGLONG current_creation = creation_time;
        for (int depth = 0; depth < MAX_ANCESTOR_DEPTH; ++depth) {
          auto entry = _processes.find(current);
          if (entry == _processes.end()) {
            return false;
          }

          auto parent = _processes.find(entry->second.parent_pid);
          if (parent == _processes.end() || parent->first == current) {
            return false;
          }

          // A parent created after its child is a reused PID, not the real parent
          auto parent_creation = process_creation_time(parent->first);
          if (parent_creation != 0 && current_creation != 0 && parent_creation > current_creation) {
            return false;
          }

          if (contains(_options.parent_processes, parent->second.exe)) {
            return true;
          }

          // Launchers often exit after starting their child (e.g. RetroBat), which breaks the
          // chain to the configured parent. A child of an already pinned process is pinned too.
          auto cached = _process_cache.find(parent->first);
          if (cached != _process_cache.end() && cached->second.pinned && cached->second.creation_time == parent_creation) {
            return true;
          }

          current = parent->first;
          current_creation = parent_creation;
        }

        return false;
      }

      void move_to_target(HWND hwnd, DWORD pid) {
        HMONITOR source = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO source_info {};
        source_info.cbSize = sizeof(source_info);
        if (!GetMonitorInfoW(source, &source_info)) {
          return;
        }

        RECT window;
        if (!GetWindowRect(hwnd, &window)) {
          return;
        }

        const auto &dst_monitor = _target_info.rcMonitor;
        const auto &dst_work = _target_info.rcWork;
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;

        // Borderless fullscreen windows cover the whole monitor: make them cover the stream display
        if (EqualRect(&window, &source_info.rcMonitor)) {
          SetWindowPos(hwnd, nullptr, dst_monitor.left, dst_monitor.top, dst_monitor.right - dst_monitor.left, dst_monitor.bottom - dst_monitor.top, flags);
          log_move(hwnd, pid);
          return;
        }

        bool maximized = IsZoomed(hwnd);
        RECT normal = window;
        if (maximized) {
          WINDOWPLACEMENT placement {};
          placement.length = sizeof(placement);
          if (GetWindowPlacement(hwnd, &placement)) {
            normal = placement.rcNormalPosition;
          }
          ShowWindowAsync(hwnd, SW_RESTORE);
        }

        // Keep the window size and its offset from the work area, clamped to the stream display
        LONG dst_width = dst_work.right - dst_work.left;
        LONG dst_height = dst_work.bottom - dst_work.top;
        LONG width = std::min<LONG>(normal.right - normal.left, dst_width);
        LONG height = std::min<LONG>(normal.bottom - normal.top, dst_height);
        LONG x = dst_work.left + std::clamp<LONG>(window.left - source_info.rcWork.left, 0, dst_width - width);
        LONG y = dst_work.top + std::clamp<LONG>(window.top - source_info.rcWork.top, 0, dst_height - height);

        SetWindowPos(hwnd, nullptr, x, y, width, height, flags);

        if (maximized) {
          ShowWindowAsync(hwnd, SW_MAXIMIZE);
        }

        log_move(hwnd, pid);
      }

      void log_move(HWND hwnd, DWORD pid) const {
        wchar_t buffer[256];
        int length = GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer));
        BOOST_LOG(info) << "Moved window ["sv << platf::to_utf8(std::wstring_view(buffer, std::max(length, 0)))
                        << "] of PID ["sv << pid << "] to stream display "sv << platf::to_utf8(_options.display_name);
      }
    };

    std::mutex control_mutex;
    std::thread worker;
    std::atomic<bool> stop_requested {false};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    void stop_locked() {
      if (!worker.joinable()) {
        return;
      }

      {
        std::lock_guard lock(wait_mutex);
        stop_requested = true;
      }
      wait_cv.notify_all();
      worker.join();
      stop_requested = false;
      pinned_seen = false;

      BOOST_LOG(info) << "Stopped pinning windows to the stream display"sv;
    }
  }  // namespace

  ULONGLONG now() {
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    return to_ull(time);
  }

  void start(options_t options) {
    std::lock_guard lock(control_mutex);
    stop_locked();

    if (options.display_name.empty()) {
      return;
    }

    BOOST_LOG(info) << "Pinning app windows to stream display "sv << platf::to_utf8(options.display_name);
    pinned_seen = false;

    worker = std::thread([options = std::move(options)]() mutable {
      pinner_t pinner(std::move(options));
      pinner.run(stop_requested, wait_mutex, wait_cv);
    });
  }

  void stop() {
    std::lock_guard lock(control_mutex);
    stop_locked();
  }

  bool windows_closed(std::chrono::milliseconds grace) {
    if (!pinned_seen) {
      return false;
    }

    auto last_seen = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(pinned_last_seen.load()));
    return std::chrono::steady_clock::now() - last_seen > grace;
  }

  std::vector<std::wstring> split_list(const std::string &list) {
    std::vector<std::wstring> result;

    size_t begin = 0;
    while (begin <= list.size()) {
      size_t end = list.find(',', begin);
      if (end == std::string::npos) {
        end = list.size();
      }

      auto item = list.substr(begin, end - begin);
      auto first = item.find_first_not_of(" \t");
      auto last = item.find_last_not_of(" \t");
      if (first != std::string::npos) {
        result.emplace_back(platf::from_utf8(item.substr(first, last - first + 1)));
      }

      begin = end + 1;
    }

    return result;
  }
}  // namespace window_pinner
