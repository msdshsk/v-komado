#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <functional>
#include <string>
#include <vector>

namespace sharedvd {
struct WindowChoice {
  HWND window{};
  DWORD process{};
  HMONITOR monitor{};
  std::wstring title;
  std::wstring application;
};

// Owned by the viewer UI thread; no capture thread, hooks or input confinement.
class WindowPicker {
 public:
  ~WindowPicker();
  void toggle(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
              std::function<void(const std::wstring&)> log);
  void move_active(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
                   std::function<void(const std::wstring&)> log);
  void close(bool restore = false);
  bool open() const { return window_ != nullptr; }
 private:
  void begin(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
             std::function<void(const std::wstring&)> log, HWND active = nullptr);
  struct Pending { HWND window; DWORD process; RECT destination; bool maximize; };
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  LRESULT message(UINT, WPARAM, LPARAM);
  void refresh();
  void switch_direction(bool returning);
  void layout();
  void thumbnails();
  void clear_thumbnails();
  void paint(HDC output = nullptr);
  void invalidate_card(int slot);
  void select(int index);
  void move_selected();
  void check_move();
  void finish(bool success, const std::wstring& error = {});
  bool valid(const WindowChoice&) const;
  int px(int value) const;
  HWND window_{}, owner_{}, previous_foreground_{};
  HMONITOR virtual_{}, primary_{}, destination_{};
  DWORD only_process_{};
  POINT original_cursor_{};
  bool returning_{}, busy_{}, direct_{};
  UINT dpi_ = 96;
  int page_{}, selected_{}, pressed_ = -1, hover_ = -1;
  int counts_[2]{};
  ULONGLONG deadline_{};
  std::wstring status_;
  std::vector<WindowChoice> choices_;
  std::vector<Pending> pending_;
  std::vector<HTHUMBNAIL> thumbnails_;
  RECT cards_[6]{}, previews_[6]{};
  HFONT font_{}, heading_{};
  std::function<void(const std::wstring&)> log_;
};
}
