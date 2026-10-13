// All selectable windows belong to this process; the viewer is PID-scoped.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "../app/window_picker.h"

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); std::cout << "PASS " << message << std::endl; }
bool wait_for(const std::function<bool()>& predicate, int timeout = 3500) {
  const ULONGLONG end = GetTickCount64() + timeout;
  do { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (predicate()) return true; Sleep(10);
  } while (GetTickCount64() < end); return false;
}
std::wstring title(HWND window) { wchar_t text[1024]{}; GetWindowTextW(window, text, 1024); return text; }
LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_WINDOWPOSCHANGING && GetPropW(window, L"RejectMove")) {
    RECT rect{}; GetWindowRect(window, &rect); auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
    pos->x = rect.left; pos->y = rect.top; pos->cx = rect.right - rect.left; pos->cy = rect.bottom - rect.top;
    pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE); return 0;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps); RECT rect{}; GetClientRect(window, &rect);
    HBRUSH brush = CreateSolidBrush(RGB(22, 115, 150)); FillRect(dc, &rect, brush); DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"VKomado  ·  Window transfer test", -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(window, &ps); return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}
struct Session {
  PROCESS_INFORMATION process{}; HWND viewer{}; POINT cursor{}; HWND foreground{}; std::vector<HWND> windows;
  Session() { GetCursorPos(&cursor); foreground = GetForegroundWindow(); }
  ~Session() {
    if (viewer) PostMessageW(viewer, WM_CLOSE, 0, 0);
    if (process.hProcess) {
      if (!wait_for([&] { return WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0; })) TerminateProcess(process.hProcess, 90);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    for (HWND window : windows) if (IsWindow(window)) DestroyWindow(window);
    SetCursorPos(cursor.x, cursor.y); if (IsWindow(foreground)) SetForegroundWindow(foreground);
  }
};
void screenshot(HWND window, const wchar_t* path, bool live = false) {
  RECT rect{}; GetWindowRect(window, &rect); HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
  auto old = SelectObject(dc, bitmap);
  if (live) BitBlt(dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
  else PrintWindow(window, dc, 2);
  SelectObject(dc, old); DeleteDC(dc); ReleaseDC(nullptr, screen);
  ULONG_PTR token{}; Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token, &input, nullptr);
  { Gdiplus::Bitmap image(bitmap, nullptr); UINT count{}, bytes{}; Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes); auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data()); Gdiplus::GetImageEncoders(count, bytes, codecs);
    for (UINT i = 0; i < count; ++i) if (wcscmp(codecs[i].MimeType, L"image/png") == 0) image.Save(path, &codecs[i].Clsid, nullptr);
  }
  Gdiplus::GdiplusShutdown(token); DeleteObject(bitmap);
}
int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Session session;
  try {
    require(argc == 4, "arguments viewer.exe log screenshot.png");
    require(!FindWindowW(L"LGWLViewerWindow", nullptr), "no existing viewer is controlled");
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
      MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
      if (!(info.dwFlags & MONITORINFOF_PRIMARY)) reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor); return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    require(monitors.size() == 1, "exactly one secondary test display"); HMONITOR remote = monitors.front();
    MONITORINFO main{sizeof(main)}, target{sizeof(target)}; GetMonitorInfoW(primary, &main); GetMonitorInfoW(remote, &target);
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = fixture_proc; cls.lpszClassName = L"VKomadoPickerFixture"; RegisterClassW(&cls);
    auto make = [&](const wchar_t* caption, HWND owner = nullptr, DWORD ex = 0) {
      HWND window = CreateWindowExW(ex, cls.lpszClassName, caption, WS_OVERLAPPEDWINDOW,
          main.rcWork.left + 100, main.rcWork.top + 100, 640, 420, owner, nullptr, cls.hInstance, nullptr);
      session.windows.push_back(window); ShowWindow(window, SW_SHOWNOACTIVATE); return window;
    };
    HWND unrelated = make(L"Unrelated window stays here");
    HWND ordinary = make(L"Browser — Project meeting");
    HWND dialog = make(L"Attached dialog", ordinary, WS_EX_TOOLWINDOW);
    SetWindowPos(dialog, nullptr, main.rcWork.left + 350, main.rcWork.top + 300, 300, 180, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(ordinary, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    STARTUPINFOW startup{sizeof(startup)};
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" --keep-windows --rescue-process-id " + std::to_wstring(GetCurrentProcessId()) + L" --log \"" + argv[2] + L"\"";
    command += L" --hotkey-config \"" + std::wstring(argv[1]) + L".test-" + std::to_wstring(GetCurrentProcessId()) + L".conf\"";
    require(CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &session.process) != FALSE, "launch scoped viewer");
    require(wait_for([&] { session.viewer = FindWindowW(L"LGWLViewerWindow", nullptr); DWORD pid{}; GetWindowThreadProcessId(session.viewer, &pid); return pid == session.process.dwProcessId; }), "owned viewer ready");
    wait_for([] { return false; }, 450);
    auto picker = [&] { HWND window = FindWindowW(L"SharedVDWindowPicker", nullptr); DWORD pid{}; GetWindowThreadProcessId(window, &pid); return pid == session.process.dwProcessId ? window : nullptr; };
    auto open = [&](bool returning, bool real_hotkey = false) {
      const auto area = returning ? target.rcWork : main.rcWork;
      SetCursorPos(area.left + 40, area.top + 40);
      if (real_hotkey) {
        SetForegroundWindow(ordinary);
        if (GetForegroundWindow() != ordinary) {
          SetWindowPos(ordinary, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          POINT point{40, 70}; ClientToScreen(ordinary, &point); SetCursorPos(point.x, point.y);
          require(WindowFromPoint(point) == ordinary, "focus click targets only owned fixture");
          INPUT mouse[2]{}; mouse[0].type = mouse[1].type = INPUT_MOUSE;
          mouse[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; mouse[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
          SendInput(2, mouse, sizeof(INPUT)); wait_for([&] { return GetForegroundWindow() == ordinary; });
          SetWindowPos(ordinary, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        require(GetForegroundWindow() == ordinary, "key injection targets owned fixture");
        SetCursorPos(area.left + 40, area.top + 40);
        INPUT keys[4]{}; for (auto& key : keys) key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_SHIFT; keys[1].ki.wVk = VK_F6; keys[2].ki.wVk = VK_F6; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[3].ki.wVk = VK_SHIFT; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        require(SendInput(4, keys, sizeof(INPUT)) == 4, "send Shift+F6 global hotkey");
      } else PostMessageW(session.viewer, WM_HOTKEY, 61, MAKELPARAM(MOD_SHIFT, VK_F6));
      require(wait_for([&] { return picker() != nullptr; }), "picker opens");
      HWND window = picker();
      wait_for([&] { SetForegroundWindow(window); return GetForegroundWindow() == window; });
      require(title(window).find(returning ? L"メイン画面へ戻す" : L"仮想画面へ送る") != std::wstring::npos, "direction follows cursor at invocation");
      require(MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == primary, "picker appears on primary");
      return window;
    };
    HWND chooser = open(false, true); screenshot(chooser, argv[3]);
    PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker(); }), "selection finishes without blocking viewer");
    require(MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == remote, "selected app sent to virtual display");
    require(MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL) == remote, "owned dialog follows selected app");
    require(MonitorFromWindow(unrelated, MONITOR_DEFAULTTONULL) == primary, "other window of same process stays on primary");
    chooser = open(true); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker(); }), "return selection completes");
    require(MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == primary && MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL) == primary, "app and dialog return to primary");
    DestroyWindow(dialog); DestroyWindow(unrelated);
    chooser = open(false); PostMessageW(chooser, WM_KEYDOWN, VK_ESCAPE, 0);
    require(wait_for([&] { return !picker(); }), "Esc cancels");
    POINT cursor{}; GetCursorPos(&cursor); std::cout << "cancel_cursor=" << cursor.x << "," << cursor.y << std::endl;
    require(cursor.x == main.rcWork.left + 40 && cursor.y == main.rcWork.top + 40, "cancel restores original pointer");
    require(MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == primary, "cancel does not move application");
    ShowWindow(ordinary, SW_MAXIMIZE); chooser = open(false); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker() && IsZoomed(ordinary) && MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == remote; }), "maximized window stays maximized on destination");
    ShowWindow(ordinary, SW_MINIMIZE); chooser = open(true); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker() && !IsIconic(ordinary) && MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == primary; }), "minimized window is restored on primary");
    ShowWindow(ordinary, SW_RESTORE); SetPropW(ordinary, L"RejectMove", reinterpret_cast<HANDLE>(1));
    chooser = open(false); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    wait_for([] { return false; }, 100);
    require(wait_for([&] { return IsWindowEnabled(GetDlgItem(chooser, 1103)) != FALSE; }, 3000), "rejected move returns to usable chooser");
    require(IsWindow(chooser) && MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == primary, "rejected move keeps window and chooser alive");
    RemovePropW(ordinary, L"RejectMove"); PostMessageW(chooser, WM_KEYDOWN, VK_ESCAPE, 0); wait_for([&] { return !picker(); });
    chooser = open(false); DestroyWindow(ordinary); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    wait_for([] { return false; }, 120); require(IsWindow(chooser), "closed candidate is rejected without closing chooser");
    PostMessageW(session.viewer, WM_HOTKEY, 60, MAKELPARAM(0, VK_F6));
    require(wait_for([&] { return !picker(); }), "F6 closes picker and keeps existing return behavior");
    GetCursorPos(&cursor); require(MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL) == primary, "F6 returns cursor to primary");
    std::vector<HWND> pages;
    for (int i = 0; i < 7; ++i) pages.push_back(make((L"Workspace window " + std::to_wstring(i + 1)).c_str()));
    chooser = open(false); wait_for([] { return false; }, 100);
    require(IsWindowEnabled(GetDlgItem(chooser, 1102)) != FALSE, "more than six windows enables next page");
    PostMessageW(chooser, WM_COMMAND, 1102, 0);
    require(wait_for([&] { return IsWindowEnabled(GetDlgItem(chooser, 1101)) && !IsWindowEnabled(GetDlgItem(chooser, 1102)); }), "last page remains reachable");
    const UINT dpi = GetDpiForWindow(chooser); LPARAM point = MAKELPARAM(MulDiv(80, dpi, 96), MulDiv(150, dpi, 96));
    PostMessageW(chooser, WM_LBUTTONDOWN, MK_LBUTTON, point); PostMessageW(chooser, WM_LBUTTONUP, 0, point);
    require(wait_for([&] { return !picker(); }), "thumbnail card mouse click moves selection");
    int moved = 0; for (HWND window : pages) if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == remote) ++moved;
    require(moved == 1, "page selection moves exactly one window");
    chooser = open(false); PostMessageW(session.viewer, WM_HOTKEY, 61, MAKELPARAM(MOD_SHIFT, VK_F6));
    require(wait_for([&] { return !picker(); }), "second Shift+F6 dismisses existing chooser");
    for (HWND window : pages) DestroyWindow(window);
    chooser = open(false); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0); wait_for([] { return false; }, 80);
    require(IsWindow(chooser) && !IsWindowEnabled(GetDlgItem(chooser, 1102)), "empty list ignores Enter safely");
    PostMessageW(chooser, WM_KEYDOWN, VK_ESCAPE, 0); require(wait_for([&] { return !picker(); }), "empty list can be cancelled");
    // Visual QA uses the same picker in this test process, so only its own
    // capture affinity can be changed. Real viewer privacy remains enabled.
    for (auto caption : {L"Browser — Project meeting", L"Editor — VKomado", L"Explorer — Documents", L"Notes — Weekly review", L"Terminal — Build output", L"Calendar — October"}) make(caption);
    HWND previewOwner = CreateWindowExW(0, cls.lpszClassName, L"Preview host", WS_OVERLAPPEDWINDOW,
        100, 100, 500, 300, nullptr, nullptr, cls.hInstance, nullptr);
    session.windows.push_back(previewOwner);
    sharedvd::WindowPicker preview;
    SetCursorPos(main.rcWork.left + 40, main.rcWork.top + 40);
    preview.toggle(previewOwner, remote, GetCurrentProcessId(), [](const std::wstring&) {});
    HWND previewWindow = FindWindowW(L"SharedVDWindowPicker", nullptr);
    require(previewWindow != nullptr, "owned visual preview opens");
    SetWindowDisplayAffinity(previewWindow, WDA_NONE); SetForegroundWindow(previewWindow);
    wait_for([] { return false; }, 200);
    require(GetForegroundWindow() == previewWindow, "visual capture is restricted to owned foreground window");
    screenshot(previewWindow, argv[3], true); preview.close(true);
    std::cout << "picker_regression=PASS" << std::endl; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << std::endl; return 1; }
}
