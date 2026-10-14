#include "dialog_follower.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwchar>
#include <vector>

namespace sharedvd {
namespace {
bool visible(HWND window) {
  if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
  DWORD cloaked{};
  return !(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked);
}
HWND virtual_owner(HWND window, DWORD pid, HMONITOR target) {
  // Use the nearest visible owner. A popup whose immediate owner is on the
  // primary display should not follow a more distant ancestor on the VDD.
  HWND owner = GetWindow(window, GW_OWNER);
  for (int depth = 0; owner && depth < 32; ++depth) {
    DWORD process{}; GetWindowThreadProcessId(owner, &process);
    if (process != pid) return nullptr;
    if (visible(owner)) return MonitorFromWindow(owner, MONITOR_DEFAULTTONULL) == target ? owner : nullptr;
    owner = GetWindow(owner, GW_OWNER);
  }
  return nullptr;
}
bool dialog_candidate(HWND window) {
  wchar_t cls[128]{}; GetClassNameW(window, cls, 128);
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((style & WS_CHILD) || (ex & (WS_EX_NOACTIVATE | WS_EX_APPWINDOW)) || IsZoomed(window)) return false;
  // Standard dialogs and captioned owned windows, excluding tooltips/menus.
  return wcscmp(cls, L"#32770") == 0 || (style & WS_CAPTION) == WS_CAPTION;
}
}
void DialogFollower::reset() { seen_.clear(); enabled_ = false; target_ = nullptr; next_ = 0; }
void DialogFollower::tick(HMONITOR target, bool enabled, DWORD only_process) {
  if (!enabled) { reset(); return; }
  MONITORINFO target_info{sizeof(target_info)};
  if (!GetMonitorInfoW(target, &target_info) || (target_info.dwFlags & MONITORINFOF_PRIMARY)) { reset(); return; }
  const ULONGLONG now = GetTickCount64();
  const bool baseline = !enabled_ || target_ != target;
  if (!baseline && now < next_) return;
  if (baseline) seen_.clear();
  enabled_ = true; target_ = target; next_ = now + 250;
  std::vector<HWND> windows;
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    if (visible(window)) reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&windows));
  std::map<HWND, Seen> current;
  for (const HWND window : windows) {
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() || (only_process && pid != only_process)) continue;
    RECT rect{}; if (!GetWindowRect(window, &rect)) continue;
    const HWND owner = GetWindow(window, GW_OWNER);
    auto old = seen_.find(window);
    Seen value{pid, owner, rect, baseline ? 0 : now + 250};
    if (old != seen_.end() && old->second.pid == pid && old->second.owner == owner) value = old->second;
    if (value.due && now >= value.due) {
      value.due = 0; // One attempt per visibility lifetime; never fight a manual move.
      HWND parent = virtual_owner(window, pid, target);
      const HWND foreground = GetForegroundWindow();
      GUITHREADINFO gui{sizeof(gui)};
      const bool moving = GetGUIThreadInfo(0, &gui) && (gui.flags & GUI_INMOVESIZE);
      if (parent && !moving && EqualRect(&rect, &value.initial) && dialog_candidate(window) &&
          (foreground == window || foreground == parent) &&
          MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != target) {
        RECT parent_rect{};
        if (GetWindowRect(parent, &parent_rect)) {
          const RECT work = target_info.rcWork;
          const LONG width = std::clamp(rect.right - rect.left, 1L, work.right - work.left);
          const LONG height = std::clamp(rect.bottom - rect.top, 1L, work.bottom - work.top);
          const LONG x = std::clamp(parent_rect.left + (parent_rect.right - parent_rect.left - width) / 2, work.left, work.right - width);
          const LONG y = std::clamp(parent_rect.top + (parent_rect.bottom - parent_rect.top - height) / 2, work.top, work.bottom - height);
          SetWindowPos(window, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        }
      }
    }
    current.emplace(window, value);
  }
  seen_.swap(current);
}
}
