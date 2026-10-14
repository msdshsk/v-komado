// All selectable windows belong to this process; the viewer is PID-scoped.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
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
    auto cursor_arrived = [&](HWND window) {
      require(wait_for([&] {RECT rect{};GetWindowRect(window,&rect);POINT point{};GetCursorPos(&point);
        return point.x==(rect.left+rect.right)/2 && point.y==rect.top+std::min(60L,(rect.bottom-rect.top)/2);}),"normal-window transfer cursor placement completes");
    };
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
      RECT panel{};GetWindowRect(window,&panel);
      const bool positioned=wait_for([&] {POINT point{};GetCursorPos(&point);return point.x==(panel.left+panel.right)/2 && point.y==panel.top+MulDiv(65,GetDpiForWindow(window),96);});
      if(!positioned){POINT point{};GetCursorPos(&point);std::cout<<"open cursor="<<point.x<<","<<point.y<<" expected="<<(panel.left+panel.right)/2<<","<<panel.top+MulDiv(65,GetDpiForWindow(window),96)<<" dpi="<<GetDpiForWindow(window)<<std::endl;}
      require(positioned,"picker opening cursor placement completes");
      return window;
    };
    HWND chooser = open(false, true); screenshot(chooser, argv[3]);
    PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker(); }), "selection finishes without blocking viewer");
    cursor_arrived(ordinary);
    require(MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == remote, "selected app sent to virtual display");
    require(MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL) == remote, "owned dialog follows selected app");
    require(MonitorFromWindow(unrelated, MONITOR_DEFAULTTONULL) == primary, "other window of same process stays on primary");
    chooser = open(false);
    require(title(GetDlgItem(chooser,1110)).find(L"1 件")!=std::wstring::npos && title(GetDlgItem(chooser,1111)).find(L"1 件")!=std::wstring::npos,"both direction buttons count independent windows across displays");
    POINT before_switch{}; GetCursorPos(&before_switch);
    SendMessageW(GetDlgItem(chooser,1111),BM_CLICK,0,0);
    require(title(chooser).find(L"メイン画面へ戻す")!=std::wstring::npos,"direction can change inside the panel without F6");
    POINT after_switch{}; GetCursorPos(&after_switch);
    require(before_switch.x==after_switch.x && before_switch.y==after_switch.y,"switching lists does not warp the cursor");
    PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    require(wait_for([&] { return !picker(); }), "return selection completes");
    cursor_arrived(ordinary);
    require(MonitorFromWindow(ordinary, MONITOR_DEFAULTTONULL) == primary && MonitorFromWindow(dialog, MONITOR_DEFAULTTONULL) == primary, "app and dialog return to primary");
    auto focus_owned = [&](HWND window) {
      require(wait_for([&] { SetForegroundWindow(window); return GetForegroundWindow() == window; }), "focus only owned fixture");
    };
    auto direct = [&](HWND window, HMONITOR destination) {
      focus_owned(window);
      // Put the cursor on the destination: direction must depend on the window instead.
      const RECT area = destination == remote ? target.rcWork : main.rcWork;
      SetCursorPos(area.left + 40, area.top + 40);
      INPUT key[2]{};key[0].type=key[1].type=INPUT_KEYBOARD;
      key[0].ki.wVk=key[1].ki.wVk=VK_F8;key[1].ki.dwFlags=KEYEVENTF_KEYUP;
      require(SendInput(2,key,sizeof(INPUT))==2,"send real F8 global shortcut");
      require(wait_for([&] { return !picker() && MonitorFromWindow(window,MONITOR_DEFAULTTONULL)==destination; }),"F8 moves active window based on its monitor, without a chooser");
      // The hidden host is destroyed before finish() restores focus and cursor.
      // Wait for the final cursor jump before invoking another cursor-based action.
      require(wait_for([&] {POINT point{};GetCursorPos(&point);return MonitorFromPoint(point,MONITOR_DEFAULTTONULL)==destination && (point.x!=area.left+40 || point.y!=area.top+40);}),"F8 completion cursor jump finishes");
    };
    direct(ordinary,remote);
    require(MonitorFromWindow(dialog,MONITOR_DEFAULTTONULL)==remote&&MonitorFromWindow(unrelated,MONITOR_DEFAULTTONULL)==primary,"F8 follows owned dialog but excludes independent sibling");
    direct(dialog,primary);
    require(MonitorFromWindow(ordinary,MONITOR_DEFAULTTONULL)==remote,"F8 on a dialog moves the dialog without dragging its parent");
    direct(dialog,remote);
    direct(ordinary,primary);
    ShowWindow(ordinary,SW_MAXIMIZE); direct(ordinary,remote);
    require(wait_for([&] { return IsZoomed(ordinary)!=FALSE; }),"F8 preserves maximization");
    direct(ordinary,primary); ShowWindow(ordinary,SW_RESTORE);
    auto following = [&](bool enabled) {
      SendMessageW(session.viewer,WM_COMMAND,1007,0);
      HWND editor=FindWindowW(L"SharedVDHotkeyEditor",nullptr);DWORD pid{};GetWindowThreadProcessId(editor,&pid);
      require(editor&&pid==session.process.dwProcessId,"open owned settings for dialog policy");
      CheckDlgButton(editor,2130,enabled?BST_CHECKED:BST_UNCHECKED);SendMessageW(editor,WM_COMMAND,IDOK,0);
      require(wait_for([&] { return !IsWindow(editor); }),"save dialog-follow setting");
      wait_for([] {return false;},650);
    };
    direct(ordinary,remote);
    HWND off_dialog=make(L"Disabled automatic policy",ordinary,WS_EX_TOOLWINDOW);focus_owned(off_dialog);
    wait_for([] {return false;},900);
    require(MonitorFromWindow(off_dialog,MONITOR_DEFAULTTONULL)==primary,"automatic following is off by default");
    following(true); focus_owned(off_dialog);wait_for([] {return false;},650);
    require(MonitorFromWindow(off_dialog,MONITOR_DEFAULTTONULL)==primary,"enabling policy does not sweep existing dialogs");
    HWND fresh=make(L"New owned dialog",ordinary,WS_EX_TOOLWINDOW);focus_owned(fresh);
    SetCursorPos(main.rcWork.left+40,main.rcWork.top+40);POINT before_follow{};GetCursorPos(&before_follow);
    require(wait_for([&] {return MonitorFromWindow(fresh,MONITOR_DEFAULTTONULL)==remote;}),"new dialog follows virtual parent");
    POINT after_follow{};GetCursorPos(&after_follow);
    if(before_follow.x!=after_follow.x || before_follow.y!=after_follow.y || GetForegroundWindow()!=fresh)
      std::cout<<"follow cursor before="<<before_follow.x<<","<<before_follow.y<<" after="<<after_follow.x<<","<<after_follow.y<<" foreground="<<GetForegroundWindow()<<" expected="<<fresh<<std::endl;
    require(before_follow.x==after_follow.x&&before_follow.y==after_follow.y&&GetForegroundWindow()==fresh,"automatic following preserves cursor and foreground");
    SetWindowPos(fresh,nullptr,main.rcWork.left+100,main.rcWork.top+100,300,200,SWP_NOZORDER|SWP_NOACTIVATE);
    wait_for([] {return false;},1000);
    require(MonitorFromWindow(fresh,MONITOR_DEFAULTTONULL)==primary,"manually returning a dialog is not fought by repeated moves");
    HWND independent=make(L"New independent application");focus_owned(independent);
    wait_for([] {return false;},900);
    require(MonitorFromWindow(independent,MONITOR_DEFAULTTONULL)==primary,"new independent window is not automatically moved");
    HWND local_dialog=make(L"Physical parent's dialog",unrelated,WS_EX_TOOLWINDOW);focus_owned(local_dialog);
    wait_for([] {return false;},900);
    require(MonitorFromWindow(local_dialog,MONITOR_DEFAULTTONULL)==primary,"dialog belonging to physical parent stays on physical display");
    following(false);
    HWND after_off=make(L"Following disabled again",ordinary,WS_EX_TOOLWINDOW);focus_owned(after_off);
    wait_for([] {return false;},900);
    require(MonitorFromWindow(after_off,MONITOR_DEFAULTTONULL)==primary,"disabling the option stops following immediately");
    for(HWND item:{off_dialog,fresh,independent,local_dialog,after_off})DestroyWindow(item);
    direct(ordinary,primary);
    DestroyWindow(dialog); DestroyWindow(unrelated);
    chooser = open(true);
    require(title(GetDlgItem(chooser,1111)).find(L"0 件")!=std::wstring::npos && IsWindowEnabled(GetDlgItem(chooser,1110)),"empty initial return list still offers send list");
    SendMessageW(GetDlgItem(chooser,1110),BM_CLICK,0,0); PostMessageW(chooser,WM_KEYDOWN,VK_RETURN,0);
    require(wait_for([&] { return !picker() && MonitorFromWindow(ordinary,MONITOR_DEFAULTTONULL)==remote; }),"send works after switching away from empty return list");
    cursor_arrived(ordinary);
    chooser = open(false);
    require(title(GetDlgItem(chooser,1110)).find(L"0 件")!=std::wstring::npos && IsWindowEnabled(GetDlgItem(chooser,1111)),"empty initial send list still offers return list");
    SendMessageW(GetDlgItem(chooser,1111),BM_CLICK,0,0); PostMessageW(chooser,WM_KEYDOWN,VK_RETURN,0);
    require(wait_for([&] { return !picker() && MonitorFromWindow(ordinary,MONITOR_DEFAULTTONULL)==primary; }),"return works after switching away from empty send list");
    cursor_arrived(ordinary);
    chooser = open(true);
    require(GetForegroundWindow()==chooser,"local list shortcut targets owned picker");
    INPUT tabs[4]{}; for(auto& key:tabs)key.type=INPUT_KEYBOARD;
    tabs[0].ki.wVk=VK_CONTROL;tabs[1].ki.wVk=VK_TAB;
    tabs[2].ki.wVk=VK_TAB;tabs[2].ki.dwFlags=KEYEVENTF_KEYUP;
    tabs[3].ki.wVk=VK_CONTROL;tabs[3].ki.dwFlags=KEYEVENTF_KEYUP;
    require(SendInput(4,tabs,sizeof(INPUT))==4,"send local Ctrl+Tab");
    require(wait_for([&] {return title(chooser).find(L"仮想画面へ送る")!=std::wstring::npos;}),"Ctrl+Tab switches active list");
    PostMessageW(chooser,WM_KEYDOWN,VK_ESCAPE,0); require(wait_for([&] {return !picker();}),"cancel after switching closes panel");
    POINT restored{};GetCursorPos(&restored);
    require(restored.x==target.rcWork.left+40 && restored.y==target.rcWork.top+40,"cancel restores invocation cursor even after direction changes");
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
    focus_owned(ordinary);PostMessageW(session.viewer,WM_HOTKEY,62,MAKELPARAM(0,VK_F8));
    require(wait_for([&] { return picker()&&IsWindowVisible(picker()); },3500),"failed direct move shows a recoverable error instead of a hidden stuck operation");
    chooser=picker();require(MonitorFromWindow(ordinary,MONITOR_DEFAULTTONULL)==primary,"rejected direct move leaves fixture on primary");
    PostMessageW(chooser,WM_KEYDOWN,VK_ESCAPE,0);require(wait_for([&] {return !picker();}),"failed direct action can be dismissed");
    chooser = open(false); PostMessageW(chooser, WM_KEYDOWN, VK_RETURN, 0);
    wait_for([] { return false; }, 100);
    require(!IsWindowEnabled(GetDlgItem(chooser,1110)) && !IsWindowEnabled(GetDlgItem(chooser,1111)),"direction buttons disabled while a move is pending");
    SendMessageW(chooser,WM_COMMAND,1111,0);
    require(title(chooser).find(L"仮想画面へ送る")!=std::wstring::npos,"pending move cannot change destination");
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
    SendMessageW(GetDlgItem(chooser,1111),BM_CLICK,0,0); SendMessageW(GetDlgItem(chooser,1110),BM_CLICK,0,0);
    require(!IsWindowEnabled(GetDlgItem(chooser,1101)) && IsWindowEnabled(GetDlgItem(chooser,1102)),"switching direction resets page and selection");
    SendMessageW(chooser,WM_COMMAND,1102,0);
    const UINT dpi = GetDpiForWindow(chooser); LPARAM point = MAKELPARAM(MulDiv(80, dpi, 96), MulDiv(180, dpi, 96));
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
