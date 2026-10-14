#pragma once
#include <windows.h>
#include <map>

namespace sharedvd {
// UI-thread polling only. Never activates a window or moves the cursor.
class DialogFollower {
 public:
  void reset();
  void tick(HMONITOR target, bool enabled, DWORD only_process = 0);
 private:
  struct Seen { DWORD pid{}; HWND owner{}; RECT initial{}; ULONGLONG due{}; };
  std::map<HWND, Seen> seen_;
  HMONITOR target_{};
  ULONGLONG next_{};
  bool enabled_{};
};
}
