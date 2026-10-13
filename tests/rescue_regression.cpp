// Own-window integration fixtures; rescue is restricted to this test process.
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
  std::cout << "PASS " << message << std::endl;
}
void pump() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message); DispatchMessageW(&message);
  }
}
struct Fixtures {
  std::vector<HWND> windows;
  ~Fixtures() { for (HWND window : windows) DestroyWindow(window); }
};
LRESULT CALLBACK fixture_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_WINDOWPOSCHANGING && GetPropW(window, L"RejectMove")) {
    RECT current{}; GetWindowRect(window, &current);
    auto* position = reinterpret_cast<WINDOWPOS*>(lp);
    position->x=current.left; position->y=current.top;
    position->cx=current.right-current.left; position->cy=current.bottom-current.top;
    position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    return 0;
  }
  return DefWindowProcW(window,message,wp,lp);
}
DWORD run_rescue(const wchar_t* viewer, const wchar_t* log, const wchar_t* mode) {
  std::wstring command = L"\"" + std::wstring(viewer) +
      L"\" " + mode +
      L" --rescue-process-id " + std::to_wstring(GetCurrentProcessId()) + L" --log \"" + log + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  command += L" --hotkey-config \"" + std::wstring(viewer) + L".test-" + std::to_wstring(GetCurrentProcessId()) + L".conf\"";
  PROCESS_INFORMATION process{};
  require(CreateProcessW(viewer, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup, &process) != FALSE, "start scoped rescue utility");
  for (int i=0; i<600 && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT; ++i) {
    pump(); Sleep(10);
  }
  DWORD code{};
  GetExitCodeProcess(process.hProcess, &code);
  if (code == STILL_ACTIVE) TerminateProcess(process.hProcess, 99);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return code;
}
int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    require(argc == 3, "arguments viewer.exe log-path");
    HMONITOR primaryMonitor = MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO primary{sizeof(primary)};
    GetMonitorInfoW(primaryMonitor, &primary);
    std::vector<HMONITOR> secondary;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
      MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor,&info);
      if (!(info.dwFlags & MONITORINFOF_PRIMARY)) reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
      return TRUE;
    },reinterpret_cast<LPARAM>(&secondary));
    require(secondary.size()==1, "exactly one secondary fixture monitor");
    MONITORINFO source{sizeof(source)}; GetMonitorInfoW(secondary[0],&source);
    WNDCLASSW cls{}; cls.lpfnWndProc=fixture_proc; cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"SharedVDRescueFixture"; RegisterClassW(&cls);
    Fixtures fixtures;
    auto make = [&](int x,int y,HWND owner=nullptr,DWORD extended=0) {
      HWND window=CreateWindowExW(extended,cls.lpszClassName,L"SharedVD rescue test",
        WS_OVERLAPPEDWINDOW,x,y,600,400,owner,nullptr,cls.hInstance,nullptr);
      fixtures.windows.push_back(window); ShowWindow(window,SW_SHOWNOACTIVATE); return window;
    };
    HWND ordinary=make(source.rcWork.left+100,source.rcWork.top+100);
    HWND maximized=make(source.rcWork.left+130,source.rcWork.top+140); ShowWindow(maximized,SW_MAXIMIZE);
    HWND minimized=make(source.rcWork.left+160,source.rcWork.top+180); ShowWindow(minimized,SW_MINIMIZE);
    HWND dialog=make(source.rcWork.left+200,source.rcWork.top+200,ordinary,WS_EX_TOOLWINDOW);
    HWND physical=make(primary.rcWork.left+200,primary.rcWork.top+200);
    RECT before{}; GetWindowRect(physical,&before);
    require(run_rescue(argv[1],argv[2],L"--list-windows")==0,"dry-run succeeds");
    require(MonitorFromWindow(ordinary,MONITOR_DEFAULTTONULL)==secondary[0],"dry-run does not move windows");
    require(run_rescue(argv[1],argv[2],L"--rescue-windows")==0,"rescue reports no failed moves");
    for(HWND window : {ordinary,maximized,minimized,dialog}) {
      RECT rect{}; GetWindowRect(window,&rect);
      require(MonitorFromWindow(window,MONITOR_DEFAULTTONULL)==primaryMonitor,"fixture moves to primary");
      require(!IsIconic(window)&&!IsZoomed(window),"fixture is restored to visible normal state");
      require(rect.left>=primary.rcWork.left && rect.top>=primary.rcWork.top &&
        rect.right<=primary.rcWork.right && rect.bottom<=primary.rcWork.bottom,"fixture fits the primary work area");
    }
    RECT after{}; GetWindowRect(physical,&after);
    require(EqualRect(&before,&after),"existing primary window is not moved");
    require(run_rescue(argv[1],argv[2],L"--rescue-windows")==0,"repeat rescue with no remaining targets succeeds");
    HWND closeFixture=make(source.rcWork.left+120,source.rcWork.top+120);
    require(run_rescue(argv[1],argv[2],L"--auto-exit-ms 1200")==0,"viewer normal close succeeds with a target app present");
    require(MonitorFromWindow(closeFixture,MONITOR_DEFAULTTONULL)==primaryMonitor,
      "viewer WM_CLOSE automatically evacuates target windows");
    HWND refusing=make(source.rcWork.left+150,source.rcWork.top+150);
    SetPropW(refusing,L"RejectMove",reinterpret_cast<HANDLE>(1));
    require(run_rescue(argv[1],argv[2],L"--rescue-windows")==4,
      "an application that rejects relocation is reported as a failure");
    require(MonitorFromWindow(refusing,MONITOR_DEFAULTTONULL)==secondary[0],
      "failed relocation does not close the application");
    std::cout<<"rescue_regression=PASS"<<std::endl;
    return 0;
  } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<std::endl; return 1; }
}
