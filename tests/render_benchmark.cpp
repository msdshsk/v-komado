// Repeatable moving-content fixture. Never moves existing user windows.
#include <windows.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

LRESULT CALLBACK paint_fixture(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint);
    RECT rect{}; GetClientRect(window,&rect);
    const auto phase=static_cast<unsigned>(GetWindowLongPtrW(window,GWLP_USERDATA));
    HBRUSH brush=CreateSolidBrush(RGB(30 + phase%150,70,180));
    FillRect(dc,&rect,brush); DeleteObject(brush);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    DrawTextW(dc,L"VKomado moving-window benchmark",-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    EndPaint(window,&paint); return 0;
  }
  return DefWindowProcW(window,message,wp,lp);
}
int wmain(int argc,wchar_t** argv) {
  if ((argc!=3 && argc!=4) || FindWindowW(L"LGWLViewerWindow",nullptr)) return 2;
  const bool boundary_drag=argc==4 && std::wstring(argv[3])==L"--drag-boundary";
  const bool maximized_drag=argc==4 && std::wstring(argv[3])==L"--drag-maximized";
  const bool after_motion=argc==4 && std::wstring(argv[3])==L"--drag-after-motion";
  const bool drag_mode=argc==4 && (std::wstring(argv[3])==L"--drag-viewer" || boundary_drag || maximized_drag || after_motion);
  if(argc==4 && !drag_mode) return 2;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::vector<RECT> secondary;
  EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor,&info);
    if (!(info.dwFlags&MONITORINFOF_PRIMARY)) reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcMonitor);
    return TRUE;
  },reinterpret_cast<LPARAM>(&secondary));
  if (secondary.size()!=1) return 3;
  WNDCLASSW cls{}; cls.lpfnWndProc=paint_fixture; cls.hInstance=GetModuleHandleW(nullptr);
  cls.lpszClassName=L"VKomadoRenderBenchmark"; RegisterClassW(&cls);
  const RECT area=secondary[0];
  HWND fixture=CreateWindowExW(after_motion ? 0 : WS_EX_NOACTIVATE,cls.lpszClassName,L"VKomado moving-content test",
      WS_POPUP,area.left+100,area.top+150,640,480,nullptr,nullptr,cls.hInstance,nullptr);
  ShowWindow(fixture,SW_SHOWNOACTIVATE);
  std::wstring command=L"\""+std::wstring(argv[1])+L"\" --keep-windows --auto-exit-ms 10000 --log \""+argv[2]+L"\"";
  STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
  command += L" --hotkey-config \"" + std::wstring(argv[1]) + L".test-" + std::to_wstring(GetCurrentProcessId()) + L".conf\"";
  if (!CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) {
    DestroyWindow(fixture); return 4;
  }
  auto started=std::chrono::steady_clock::now();
  auto next=started;
  auto next_pointer=started;
  auto drag_started=started;
  auto last_moved=started;
  double maximum_move_gap_ms=0.0;
  unsigned move_updates=0;
  bool drag_attempted=false, button_held=false;
  bool fixture_activated=false, viewer_activated_during_drag=false;
  HWND viewer=nullptr;
  POINT original_cursor{}, drag_origin{};
  RECT last_window{};
  GetCursorPos(&original_cursor);
  unsigned frames=0;
  while (WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT &&
         std::chrono::steady_clock::now()-started<std::chrono::seconds(14)) {
    MSG message{};
    while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    const auto now=std::chrono::steady_clock::now();
    // Exercise moving captured content, followed by a static source while the
    // viewer is dragged. Continuous fixture repainting masks this transition.
    if (drag_mode && !drag_attempted && now-started>std::chrono::seconds(after_motion ? 4 : 2)) {
      drag_attempted=true;
      HWND candidate=FindWindowW(L"LGWLViewerWindow",nullptr);
      DWORD pid{}; if(candidate) GetWindowThreadProcessId(candidate,&pid);
      if(pid==process.dwProcessId) {
        viewer=candidate;
        if(after_motion) {
          RECT fixture_rect{}; GetWindowRect(fixture,&fixture_rect);
          POINT fixture_point{fixture_rect.left+100,fixture_rect.top+100};
          SetWindowPos(fixture,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
          SetCursorPos(fixture_point.x,fixture_point.y);
          if(WindowFromPoint(fixture_point)==fixture) {
            INPUT click[2]{}; click[0].type=click[1].type=INPUT_MOUSE;
            click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
            SendInput(2,click,sizeof(INPUT));
            const auto focus_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            while(std::chrono::steady_clock::now()<focus_deadline) {
              while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
              if(GetForegroundWindow()==fixture) break;
              Sleep(1);
            }
          }
          fixture_activated=GetForegroundWindow()==fixture;
          std::cout<<"fixture_foreground_before_drag="<<fixture_activated<<std::endl;
        }
        if(maximized_drag) SendMessageW(viewer,WM_SYSCOMMAND,SC_MAXIMIZE,0);
        SetWindowPos(viewer,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        GetWindowRect(viewer,&last_window);
        drag_origin=POINT{last_window.left+200,last_window.top+20};
        SetCursorPos(drag_origin.x,drag_origin.y);
        Sleep(160);
        GetWindowThreadProcessId(WindowFromPoint(drag_origin),&pid);
        if(pid==process.dwProcessId) {
          INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
          button_held=SendInput(1,&input,sizeof(INPUT))==1;
          drag_started=last_moved=std::chrono::steady_clock::now();
        }
      }
    }
    if(button_held) {
      if(GetForegroundWindow()==viewer) viewer_activated_during_drag=true;
      const auto elapsed=std::chrono::duration<double,std::milli>(now-drag_started).count();
      if(elapsed>=3000) {
        INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
        SendInput(1,&input,sizeof(INPUT)); button_held=false;
      } else if(now>=next_pointer && elapsed>=0) {
        SetCursorPos(drag_origin.x+static_cast<int>(elapsed*(boundary_drag ? 0.6 : 0.15)),
                     drag_origin.y+(maximized_drag ? static_cast<int>(elapsed*0.10) : 0));
        next_pointer=now+std::chrono::milliseconds(8);
      }
      RECT current{}; GetWindowRect(viewer,&current);
      if(current.left!=last_window.left || current.top!=last_window.top) {
        const double gap=std::chrono::duration<double,std::milli>(now-last_moved).count();
        if(gap>maximum_move_gap_ms) maximum_move_gap_ms=gap;
        last_moved=now; last_window=current; ++move_updates;
      }
    }
    if(now>=next && (!after_motion || now-started<std::chrono::seconds(3))) {
      ++frames;
      const int x=area.left+300+static_cast<int>(std::sin(frames*0.05)*200);
      const int y=area.top+220+static_cast<int>(std::cos(frames*0.04)*100);
      SetWindowLongPtrW(fixture,GWLP_USERDATA,frames);
      if(!drag_mode || after_motion) SetWindowPos(fixture,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
      InvalidateRect(fixture,nullptr,FALSE); UpdateWindow(fixture);
      next=now+std::chrono::microseconds(16667);
    }
    Sleep(1);
  }
  DWORD code{}; GetExitCodeProcess(process.hProcess,&code);
  if(button_held) {
    INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=MOUSEEVENTF_LEFTUP;
    SendInput(1,&input,sizeof(INPUT));
  }
  if(drag_mode) SetCursorPos(original_cursor.x,original_cursor.y);
  if(code==STILL_ACTIVE) TerminateProcess(process.hProcess,99);
  CloseHandle(process.hThread); CloseHandle(process.hProcess); DestroyWindow(fixture);
  std::cout<<"fixture_updates="<<frames<<" viewer_exit="<<code<<std::endl;
  if(drag_mode) std::cout<<"native_drag_updates="<<move_updates<<" maximum_move_gap_ms="<<maximum_move_gap_ms<<std::endl;
  if(after_motion) std::cout<<"viewer_foreground_during_drag="<<viewer_activated_during_drag<<std::endl;
  return code==0 && (!drag_mode || move_updates>10) &&
      (!after_motion || (fixture_activated && viewer_activated_during_drag))?0:1;
}
