#include "window_picker.h"
#include <windowsx.h>
#include <algorithm>
#include <cwchar>

namespace sharedvd {
namespace {
constexpr int kPageSize = 6;
constexpr UINT_PTR kMoveTimer = 71;
bool visible_app(HWND window) {
  if (!IsWindowVisible(window) || window == GetShellWindow() || window == GetDesktopWindow()) return false;
  wchar_t name[128]{}; GetClassNameW(window, name, 128);
  for (auto cls : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                   L"LGWLViewerWindow", L"SharedVDControlWindow", L"SharedVDWindowPicker",
                   L"SharedVDHotkeyEditor", L"SharedVDHoverChrome", L"SharedVDCursorRing", L"#32768", L"tooltips_class32"})
    if (wcscmp(name, cls) == 0) return false;
  DWORD cloaked{};
  return !(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked);
}
std::wstring application_name(DWORD process) {
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
  wchar_t path[32768]{}; DWORD size = 32768;
  const bool read = handle && QueryFullProcessImageNameW(handle, 0, path, &size);
  if (handle) CloseHandle(handle);
  if (!read) return L"アプリ";
  const wchar_t* basename = wcsrchr(path, L'\\');
  return basename ? basename + 1 : path;
}
void fill(HDC dc, RECT rect, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void label(HDC dc, const std::wstring& text, RECT rect, COLORREF color, UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
  SetTextColor(dc, color); DrawTextW(dc, text.c_str(), -1, &rect, flags | DT_NOPREFIX);
}
}

WindowPicker::~WindowPicker() { close(); }
int WindowPicker::px(int value) const { return MulDiv(value, dpi_, 96); }

void WindowPicker::toggle(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
                          std::function<void(const std::wstring&)> log) {
  if (window_) { if (!busy_) close(true); return; }
  begin(owner, virtual_monitor, only_process, std::move(log));
}
void WindowPicker::move_active(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
                               std::function<void(const std::wstring&)> log) {
  if (window_) return;
  const HWND active = GetForegroundWindow();
  DWORD process{}; GetWindowThreadProcessId(active, &process);
  if (!active || active == owner || process == GetCurrentProcessId() ||
      (only_process && process != only_process) || !visible_app(active) ||
      (GetWindowLongPtrW(active, GWL_STYLE) & WS_CHILD)) return;
  begin(owner, virtual_monitor, only_process, std::move(log), active);
}
void WindowPicker::begin(HWND owner, HMONITOR virtual_monitor, DWORD only_process,
                         std::function<void(const std::wstring&)> log, HWND active) {
  owner_ = owner; virtual_ = virtual_monitor; only_process_ = only_process; log_ = std::move(log);
  direct_ = active != nullptr;
  primary_ = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO main{sizeof(main)}, target{sizeof(target)};
  if (primary_ == virtual_ || !GetMonitorInfoW(primary_, &main) || !GetMonitorInfoW(virtual_, &target)) return;
  GetCursorPos(&original_cursor_); previous_foreground_ = GetForegroundWindow();
  returning_ = (direct_ ? MonitorFromWindow(active, MONITOR_DEFAULTTONULL) : MonitorFromPoint(original_cursor_, MONITOR_DEFAULTTONULL)) == virtual_;
  destination_ = returning_ ? primary_ : virtual_;
  page_ = selected_ = 0; busy_ = false; status_.clear(); pending_.clear();
  WNDCLASSEXW cls{sizeof(cls)}; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"SharedVDWindowPicker"; cls.lpfnWndProc = procedure;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101));
  RegisterClassExW(&cls);
  dpi_ = GetDpiForWindow(owner_);
  // Put the selection UI on the physical display even when the cursor is remote.
  const int width = std::min<LONG>(px(920), main.rcWork.right - main.rcWork.left);
  const int height = std::min<LONG>(px(640), main.rcWork.bottom - main.rcWork.top);
  const int x = main.rcWork.left + (main.rcWork.right - main.rcWork.left - width) / 2;
  const int y = main.rcWork.top + (main.rcWork.bottom - main.rcWork.top - height) / 2;
  window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName,
      returning_ ? L"メイン画面へ戻す — VKomado" : L"仮想画面へ送る — VKomado",
      WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner_, nullptr, cls.hInstance, this);
  if (!window_) return;
  dpi_ = GetDpiForWindow(window_);
  font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
  heading_ = CreateFontW(-px(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
  BOOL dark = TRUE; DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
  for (const auto& entry : {std::pair{1101, L"前へ"}, {1102, L"次へ"}, {1103, L"更新"}, {1104, L"キャンセル"}}) {
    HWND button = CreateWindowExW(0, L"BUTTON", entry.second, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(entry.first)), cls.hInstance, nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
  if (direct_) {
    DWORD process{}; GetWindowThreadProcessId(active, &process);
    wchar_t caption[1024]{}; GetWindowTextW(active, caption, 1024);
    choices_ = {{active, process, MonitorFromWindow(active, MONITOR_DEFAULTTONULL), caption, application_name(process)}};
    move_selected(); return; // Hidden host handles completion without opening or activating the chooser.
  }
  refresh(); ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(window_);
  // Bringing the pointer to the chooser makes the remote-to-main action usable immediately.
  SetCursorPos(x + width / 2, y + px(65));
  log_(std::wstring(L"picker_open direction=") + (returning_ ? L"to_primary" : L"to_virtual") + L" count=" + std::to_wstring(choices_.size()));
}

void WindowPicker::clear_thumbnails() {
  for (auto thumbnail : thumbnails_) if (thumbnail) DwmUnregisterThumbnail(thumbnail);
  thumbnails_.clear();
}
void WindowPicker::close(bool restore) {
  if (!window_) return;
  const bool had_focus = GetForegroundWindow() == window_;
  DestroyWindow(window_);
  if (restore && had_focus) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromPoint(original_cursor_, MONITOR_DEFAULTTONULL), &info))
      SetCursorPos(original_cursor_.x, original_cursor_.y);
    if (IsWindow(previous_foreground_)) SetForegroundWindow(previous_foreground_);
  }
}

bool WindowPicker::valid(const WindowChoice& choice) const {
  DWORD pid{}; GetWindowThreadProcessId(choice.window, &pid);
  return pid == choice.process && visible_app(choice.window) &&
      MonitorFromWindow(choice.window, MONITOR_DEFAULTTONULL) == choice.monitor;
}
void WindowPicker::refresh() {
  clear_thumbnails(); choices_.clear();
  EnumWindows([](HWND window, LPARAM param) -> BOOL {
    auto& self = *reinterpret_cast<WindowPicker*>(param);
    DWORD process{}; GetWindowThreadProcessId(window, &process);
    if (window == self.owner_ || (process == GetCurrentProcessId() && !self.only_process_) ||
        (self.only_process_ && process != self.only_process_) || !visible_app(window)) return TRUE;
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) && !(extended & WS_EX_APPWINDOW)) return TRUE;
    HWND owner = GetAncestor(window, GA_ROOTOWNER);
    if (owner != window && visible_app(owner)) return TRUE;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor || ((monitor == self.virtual_) != self.returning_)) return TRUE;
    wchar_t title[1024]{}; if (!GetWindowTextW(window, title, 1024)) return TRUE;
    self.choices_.push_back({window, process, monitor, title, application_name(process)});
    return TRUE;
  }, reinterpret_cast<LPARAM>(this));
  selected_ = std::clamp(selected_, 0, std::max(0, static_cast<int>(choices_.size()) - 1));
  page_ = selected_ / kPageSize; hover_ = pressed_ = -1; layout();
}

void WindowPicker::layout() {
  if (!window_) return;
  RECT client{}; GetClientRect(window_, &client);
  const int gap = px(14), left = px(22), top = px(110);
  const int width = (client.right - 2 * left - 2 * gap) / 3;
  const int height = (client.bottom - top - px(102) - gap) / 2;
  for (int i = 0; i < kPageSize; ++i) {
    const int x = left + (i % 3) * (width + gap), y = top + (i / 3) * (height + gap);
    cards_[i] = {x, y, x + width, y + height};
    previews_[i] = {x + px(8), y + px(8), x + width - px(8), y + height - px(56)};
  }
  const int bottom = client.bottom - px(44);
  MoveWindow(GetDlgItem(window_, 1101), left, bottom, px(64), px(28), TRUE);
  MoveWindow(GetDlgItem(window_, 1102), left + px(74), bottom, px(64), px(28), TRUE);
  MoveWindow(GetDlgItem(window_, 1103), client.right - px(220), bottom, px(78), px(28), TRUE);
  MoveWindow(GetDlgItem(window_, 1104), client.right - px(132), bottom, px(110), px(28), TRUE);
  EnableWindow(GetDlgItem(window_, 1101), !busy_ && page_ > 0);
  EnableWindow(GetDlgItem(window_, 1102), !busy_ && (page_ + 1) * kPageSize < static_cast<int>(choices_.size()));
  EnableWindow(GetDlgItem(window_, 1103), !busy_);
  EnableWindow(GetDlgItem(window_, 1104), !busy_);
  thumbnails(); InvalidateRect(window_, nullptr, FALSE);
}

void WindowPicker::thumbnails() {
  clear_thumbnails();
  for (int i = 0; i < kPageSize && page_ * kPageSize + i < static_cast<int>(choices_.size()); ++i) {
    const auto& choice = choices_[page_ * kPageSize + i];
    HTHUMBNAIL thumbnail{};
    if (valid(choice) && !IsIconic(choice.window) && SUCCEEDED(DwmRegisterThumbnail(window_, choice.window, &thumbnail))) {
      SIZE source{}; DwmQueryThumbnailSourceSize(thumbnail, &source);
      RECT area = previews_[i];
      if (source.cx > 0 && source.cy > 0) {
        const double scale = std::min(static_cast<double>(area.right - area.left) / source.cx,
                                      static_cast<double>(area.bottom - area.top) / source.cy);
        const int w = static_cast<int>(source.cx * scale), h = static_cast<int>(source.cy * scale);
        area.left += (area.right - area.left - w) / 2; area.top += (area.bottom - area.top - h) / 2;
        area.right = area.left + w; area.bottom = area.top + h;
      }
      DWM_THUMBNAIL_PROPERTIES props{};
      props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
      props.rcDestination = area; props.fVisible = TRUE; props.opacity = 255; props.fSourceClientAreaOnly = FALSE;
      if (FAILED(DwmUpdateThumbnailProperties(thumbnail, &props))) { DwmUnregisterThumbnail(thumbnail); thumbnail = nullptr; }
    }
    thumbnails_.push_back(thumbnail);
  }
}

void WindowPicker::paint(HDC output) {
  PAINTSTRUCT ps{}; HDC screen = output ? output : BeginPaint(window_, &ps);
  RECT client{}; GetClientRect(window_, &client);
  HDC dc = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
  auto old_bitmap = SelectObject(dc, bitmap); auto old_font = SelectObject(dc, heading_);
  SetBkMode(dc, TRANSPARENT); fill(dc, client, RGB(20, 25, 34));
  label(dc, returning_ ? L"メイン画面へ戻す" : L"仮想画面へ送る", {px(22), px(15), client.right - px(22), px(51)}, RGB(240, 245, 252));
  SelectObject(dc, font_);
  label(dc, L"ウィンドウを選択  ·  クリック / 矢印＋Enterで移動  ·  Escでキャンセル", {px(22), px(55), client.right - px(22), px(82)}, RGB(158, 177, 201));
  label(dc, returning_ ? L"仮想画面 → メイン画面" : L"他の画面 → 仮想画面", {px(22), px(83), client.right - px(22), px(105)}, RGB(68, 207, 239));
  for (int i = 0; i < kPageSize && page_ * kPageSize + i < static_cast<int>(choices_.size()); ++i) {
    const int index = page_ * kPageSize + i; const auto& choice = choices_[index];
    RECT card = cards_[i]; fill(dc, card, index == selected_ ? RGB(54, 194, 224) : RGB(46, 57, 75));
    InflateRect(&card, -px(2), -px(2)); fill(dc, card, hover_ == i ? RGB(43, 56, 74) : RGB(31, 40, 54));
    fill(dc, previews_[i], RGB(14, 19, 27));
    if (i >= static_cast<int>(thumbnails_.size()) || !thumbnails_[i])
      label(dc, IsIconic(choice.window) ? L"最小化中 · 選ぶと復元して移動" : L"プレビューなし", previews_[i], RGB(153, 172, 197), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT title{card.left + px(10), card.bottom - px(48), card.right - px(10), card.bottom - px(24)};
    label(dc, choice.title, title, RGB(239, 244, 252));
    title.top += px(23); title.bottom += px(21); label(dc, choice.application, title, RGB(146, 168, 196));
  }
  if (choices_.empty()) label(dc, L"移動できるウィンドウはありません", {px(22), px(170), client.right - px(22), px(250)}, RGB(165, 184, 210), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  const std::wstring text = status_.empty() ? L"選んだウィンドウと付属ダイアログを移動します" : status_;
  label(dc, text, {px(22), client.bottom - px(93), client.right - px(22), client.bottom - px(55)}, status_.empty() ? RGB(153, 174, 198) : RGB(255, 204, 130));
  label(dc, std::to_wstring(page_ + 1) + L" / " + std::to_wstring(std::max(1, (static_cast<int>(choices_.size()) + 5) / 6)) + L" ページ   ·   " + std::to_wstring(choices_.size()) + L" 件",
      {px(178), client.bottom - px(44), client.right - px(230), client.bottom - px(16)}, RGB(153, 174, 198));
  BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, old_font); SelectObject(dc, old_bitmap); DeleteObject(bitmap); DeleteDC(dc);
  if (!output) EndPaint(window_, &ps);
}

void WindowPicker::select(int index) {
  if (busy_ || choices_.empty()) return;
  selected_ = std::clamp(index, 0, static_cast<int>(choices_.size()) - 1);
  const int page = selected_ / kPageSize;
  if (page != page_) { page_ = page; layout(); }
  else InvalidateRect(window_, nullptr, FALSE);
}

void WindowPicker::move_selected() {
  if (busy_ || choices_.empty()) return;
  const WindowChoice choice = choices_[selected_];
  MONITORINFO destination{sizeof(destination)};
  if (!valid(choice) || !GetMonitorInfoW(destination_, &destination)) {
    finish(false, L"ウィンドウまたは画面が変わりました。一覧を更新しました。"); return;
  }
  pending_.clear();
  struct Context { WindowPicker* self; WindowChoice choice; RECT work; bool failed = false; } context{this, choice, destination.rcWork};
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto& ctx = *reinterpret_cast<Context*>(data);
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (pid != ctx.choice.process || !visible_app(window)) return TRUE;
    // Follow only this window's ownership chain, never other windows of the same process.
    HWND parent = window;
    for (int depth = 0; parent && parent != ctx.choice.window && depth < 64; ++depth) parent = GetWindow(parent, GW_OWNER);
    if (parent != ctx.choice.window) return TRUE;
    WINDOWPLACEMENT placement{sizeof(placement)}; MONITORINFO source{sizeof(source)};
    if (!GetWindowPlacement(window, &placement) || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &source)) { ctx.failed = true; return TRUE; }
    RECT normal = placement.rcNormalPosition;
    if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
      OffsetRect(&normal, source.rcWork.left - source.rcMonitor.left, source.rcWork.top - source.rcMonitor.top);
    const auto work = ctx.work;
    const LONG w = std::clamp(normal.right - normal.left, 1L, work.right - work.left);
    const LONG h = std::clamp(normal.bottom - normal.top, 1L, work.bottom - work.top);
    const LONG x = std::clamp(work.left + normal.left - source.rcWork.left, work.left, work.right - w);
    const LONG y = std::clamp(work.top + normal.top - source.rcWork.top, work.top, work.bottom - h);
    ctx.self->pending_.push_back({window, pid, {x, y, x + w, y + h},
      IsZoomed(window) != FALSE || (IsIconic(window) && (placement.flags & WPF_RESTORETOMAXIMIZED))});
    return TRUE;
  }, reinterpret_cast<LPARAM>(&context));
  if (context.failed || std::none_of(pending_.begin(), pending_.end(), [&](const auto& item) { return item.window == choice.window; })) {
    finish(false, L"移動対象を取得できませんでした。更新してお試しください。"); return;
  }
  std::stable_sort(pending_.begin(), pending_.end(), [&](const auto& a, const auto& b) {
    return (a.window != choice.window) < (b.window != choice.window);
  });
  busy_ = true; status_ = L"ウィンドウを移動しています…"; layout();
  for (const auto& pending : pending_) {
    DWORD pid{}; GetWindowThreadProcessId(pending.window, &pid);
    if (pid != pending.process) { finish(false, L"移動前にウィンドウが閉じられました。"); return; }
    WINDOWPLACEMENT placement{sizeof(placement)};
    placement.flags = WPF_ASYNCWINDOWPLACEMENT; placement.showCmd = SW_SHOWNOACTIVATE;
    placement.ptMinPosition = placement.ptMaxPosition = POINT{-1, -1}; placement.rcNormalPosition = pending.destination;
    if (!(GetWindowLongPtrW(pending.window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
      OffsetRect(&placement.rcNormalPosition, destination.rcMonitor.left - destination.rcWork.left, destination.rcMonitor.top - destination.rcWork.top);
    if (!SetWindowPlacement(pending.window, &placement)) {
      finish(false, L"移動できないウィンドウがあります。管理者権限のアプリ等は手動で移動してください。"); return;
    }
  }
  log_(L"picker_move hwnd=" + std::to_wstring(reinterpret_cast<UINT_PTR>(choice.window)) + L" count=" + std::to_wstring(pending_.size()));
  deadline_ = GetTickCount64() + 1500; SetTimer(window_, kMoveTimer, 30, nullptr);
}

void WindowPicker::check_move() {
  bool done = true;
  for (const auto& pending : pending_) {
    DWORD pid{}; GetWindowThreadProcessId(pending.window, &pid);
    if (pid != pending.process) { finish(false, L"移動中にウィンドウが閉じられました。"); return; }
    RECT rect{}; GetWindowRect(pending.window, &rect);
    MONITORINFO info{sizeof(info)};
    const POINT title{rect.left + (rect.right - rect.left) / 2, rect.top + 10};
    if (!GetMonitorInfoW(destination_, &info)) { finish(false, L"移動先の画面が切断されました。"); return; }
    if (MonitorFromWindow(pending.window, MONITOR_DEFAULTTONULL) != destination_ || IsIconic(pending.window) || !PtInRect(&info.rcWork, title)) done = false;
  }
  if (done) finish(true);
  else if (GetTickCount64() >= deadline_) finish(false, L"移動を確認できませんでした。応答停止やアプリ側の配置制限を確認してください。");
}
void WindowPicker::finish(bool success, const std::wstring& error) {
  KillTimer(window_, kMoveTimer); busy_ = false;
  log_(std::wstring(L"picker_move_result=") + (success ? L"success" : L"failed"));
  if (!success) {
    status_ = error; pending_.clear(); refresh();
    if (direct_) { direct_ = false; ShowWindow(window_, SW_SHOWNOACTIVATE); }
    return;
  }
  HWND chosen = choices_[selected_].window;
  const bool retain_focus = !direct_ || GetForegroundWindow() == previous_foreground_;
  RECT rect{}; GetWindowRect(chosen, &rect);
  // Restore maximization only after the normal rectangle has reached the new display.
  for (const auto& pending : pending_) if (pending.maximize) ShowWindowAsync(pending.window, SW_MAXIMIZE);
  close();
  if (!retain_focus) return; // A user who changed apps during an asynchronous move keeps focus/cursor.
  HWND focus = GetLastActivePopup(chosen);
  if (!IsWindowVisible(focus)) focus = chosen;
  SetForegroundWindow(focus);
  SetCursorPos(rect.left + (rect.right - rect.left) / 2, rect.top + std::min(60L, (rect.bottom - rect.top) / 2));
}

LRESULT CALLBACK WindowPicker::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<WindowPicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<WindowPicker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}
LRESULT WindowPicker::message(UINT message, WPARAM wp, LPARAM lp) {
  switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(wp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (font_) layout(); return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      if (font_) {
        DeleteObject(font_); DeleteObject(heading_);
        font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        heading_ = CreateFontW(-px(24), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        for (int id = 1101; id <= 1104; ++id) SendMessageW(GetDlgItem(window_, id), WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
      }
      auto rect = reinterpret_cast<RECT*>(lp);
      SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; int hovered = -1;
      for (int i = 0; i < 6; ++i) if (PtInRect(&cards_[i], point)) hovered = i;
      if (hover_ != hovered) { hover_ = hovered; InvalidateRect(window_, nullptr, FALSE); }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: hover_ = -1; InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
      pressed_ = -1; POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      for (int i = 0; i < 6; ++i) if (PtInRect(&cards_[i], point) && page_ * 6 + i < static_cast<int>(choices_.size())) { pressed_ = i; select(page_ * 6 + i); }
      SetFocus(window_); return 0;
    }
    case WM_LBUTTONUP: {
      POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; const int pressed = pressed_; pressed_ = -1;
      if (pressed >= 0 && PtInRect(&cards_[pressed], point)) move_selected(); return 0;
    }
    case WM_CANCELMODE: case WM_KILLFOCUS: pressed_ = -1; return 0;
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) { if (!busy_) close(true); return 0; }
      if (wp == VK_RETURN) { move_selected(); return 0; }
      if (wp == VK_LEFT) select(selected_ - 1);
      if (wp == VK_RIGHT || wp == VK_TAB) select(selected_ + 1);
      if (wp == VK_UP) select(selected_ - 3);
      if (wp == VK_DOWN) select(selected_ + 3);
      if (wp == VK_PRIOR) select(selected_ - 6);
      if (wp == VK_NEXT) select(selected_ + 6);
      if (wp == VK_F5 && !busy_) refresh();
      return 0;
    case WM_MOUSEWHEEL: select(selected_ + (GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 6 : -6)); return 0;
    case WM_COMMAND:
      if (busy_) return 0;
      if (LOWORD(wp) == 1101) select((page_ - 1) * 6);
      if (LOWORD(wp) == 1102) select((page_ + 1) * 6);
      if (LOWORD(wp) == 1103) refresh();
      if (LOWORD(wp) == 1104) { close(true); return 0; }
      SetFocus(window_); return 0;
    case WM_TIMER: if (wp == kMoveTimer) check_move(); return 0;
    case WM_DISPLAYCHANGE: close(); return 0;
    case WM_CLOSE: if (!busy_) close(true); return 0;
    case WM_DESTROY:
      KillTimer(window_, kMoveTimer); clear_thumbnails();
      if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_);
      font_ = heading_ = nullptr; return 0;
    case WM_NCDESTROY: {
      HWND window = window_; window_ = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      return DefWindowProcW(window, message, wp, lp);
    }
  }
  return DefWindowProcW(window_, message, wp, lp);
}
}
