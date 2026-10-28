/**
 * @file src/platform/windows/window_pinner.h
 * @brief Declarations for keeping the windows of a streamed app on the session's virtual display.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// platform includes
#include <windows.h>

namespace window_pinner {
  struct options_t {
    /**
     * GDI device name of the display the windows are pinned to (e.g. `\\.\DISPLAY5`).
     */
    std::wstring display_name;

    /**
     * Job object of the app process group. Every window owned by a process in the job is pinned.
     * May be `nullptr` when the app has no command (e.g. detached-only apps).
     */
    HANDLE job = nullptr;

    /**
     * Executable names (e.g. `steam.exe`). A process started after the session began and
     * descending from one of these is pinned, so games and launchers started by an already
     * running launcher follow the stream.
     */
    std::vector<std::wstring> parent_processes;

    /**
     * Window title substrings (e.g. `Big Picture`). Matching windows are pinned regardless
     * of the process that owns them.
     */
    std::vector<std::wstring> window_titles;

    /**
     * Session start as a `FILETIME` value; only processes created after it are matched by
     * `parent_processes`. 0 uses the time `start()` is called.
     */
    ULONGLONG session_start = 0;
  };

  /**
   * @return The current time as a `FILETIME` value, for `options_t::session_start`.
   */
  ULONGLONG now();

  /**
   * @brief Start pinning windows to the given display, replacing any previous session.
   * @param options What to pin and where.
   */
  void start(options_t options);

  /**
   * @brief Stop pinning windows. Windows are left where they are.
   */
  void stop();

  /**
   * @brief Split a comma-separated list into trimmed, non-empty entries.
   * @param list The comma-separated list.
   * @return The entries, converted to UTF-16.
   */
  std::vector<std::wstring> split_list(const std::string &list);
}  // namespace window_pinner
