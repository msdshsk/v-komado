// Observe actual paint traffic on owned windows; never control another application's UI.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../app/window_picker.h"

namespace {
unsigned control_paints{}, panel_paints{};
struct Probe { HWND window; WNDPROC previous; bool panel; };
std::vector<Probe> probes;
LRESULT CALLBACK observe(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  for (const auto& probe : probes) if (probe.window == window) {
    if (message == WM_PAINT) ++(probe.panel ? panel_paints : control_paints);
    return CallWindowProcW(probe.previous, window, message, wp, lp);
  }
  return DefWindowProcW(window, message, wp, lp);
}
void watch(HWND window, bool panel = false) {
  probes.push_back({window, reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)), panel});
  SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(observe));
}
void flush(HWND panel) {
  UpdateWindow(panel);
  for (const auto& probe : probes) if (!probe.panel) UpdateWindow(probe.window);
}
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
  std::cout << "PASS " << message << std::endl;
}
void screenshot(HWND panel, const wchar_t* path) {
  SetWindowDisplayAffinity(panel,WDA_NONE);DwmFlush();
  RECT rect{};GetClientRect(panel,&rect);POINT origin{};ClientToScreen(panel,&origin);
  HDC screen=GetDC(nullptr), dc=CreateCompatibleDC(screen);
  HBITMAP bitmap=CreateCompatibleBitmap(screen,rect.right,rect.bottom);auto old=SelectObject(dc,bitmap);
  BitBlt(dc,0,0,rect.right,rect.bottom,screen,origin.x,origin.y,SRCCOPY|CAPTUREBLT);
  SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);
  ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);
  { Gdiplus::Bitmap image(bitmap,nullptr);UINT count{},bytes{};Gdiplus::GetImageEncodersSize(&count,&bytes);
    std::vector<BYTE> storage(bytes);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    Gdiplus::GetImageEncoders(count,bytes,codecs);
    for(UINT i=0;i<count;++i)if(wcscmp(codecs[i].MimeType,L"image/png")==0) image.Save(path,&codecs[i].Clsid,nullptr);
  }
  Gdiplus::GdiplusShutdown(token);DeleteObject(bitmap);
}
}
int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  POINT cursor{}; GetCursorPos(&cursor); HWND foreground = GetForegroundWindow();
  HMONITOR primary = MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY), remote{};
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
    MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
    if (!(info.dwFlags & MONITORINFOF_PRIMARY)) *reinterpret_cast<HMONITOR*>(data) = monitor;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&remote));
  if (!remote) { std::cerr << "A secondary display is required\n"; return 2; }
  MONITORINFO info{sizeof(info)}; GetMonitorInfoW(primary, &info);
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = DefWindowProcW;
  cls.lpszClassName = L"VKomadoPaintFixture"; cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
  RegisterClassW(&cls);
  std::vector<HWND> fixtures;
  for (int i=0;i<3;++i) {
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Paint fixture", WS_OVERLAPPEDWINDOW,
        info.rcWork.left+100+i*30,info.rcWork.top+100,400,300,nullptr,nullptr,cls.hInstance,nullptr);
    fixtures.push_back(window); if (i) ShowWindow(window,SW_SHOWNOACTIVATE);
  }
  sharedvd::WindowPicker picker;
  int result = 0;
  try {
    SetCursorPos(info.rcWork.left+40,info.rcWork.top+40);
    picker.toggle(fixtures[0],remote,GetCurrentProcessId(),[](const std::wstring&){});
    HWND panel = FindWindowW(L"SharedVDWindowPicker",nullptr); DWORD pid{};GetWindowThreadProcessId(panel,&pid);
    require(panel && pid==GetCurrentProcessId(),"paint test uses its own picker");
    watch(panel,true);
    for (int id : {1101,1102,1103,1104,1110,1111}) watch(GetDlgItem(panel,id));
    RedrawWindow(panel,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    control_paints=panel_paints=0;
    const UINT dpi=GetDpiForWindow(panel);
    auto point=[&](int x,int y){return MAKELPARAM(MulDiv(x,dpi,96),MulDiv(y,dpi,96));};
    SendMessageW(panel,WM_MOUSEMOVE,0,point(80,180));
    HRGN region=CreateRectRgn(0,0,0,0);GetUpdateRgn(panel,region,FALSE);
    const bool preview_untouched=!PtInRegion(region,MulDiv(80,dpi,96),MulDiv(180,dpi,96));
    DeleteObject(region);
    require(preview_untouched,"hover leaves live thumbnail pixels outside the paint region");
    flush(panel);SendMessageW(panel,WM_MOUSELEAVE,0,0);flush(panel);
    control_paints=panel_paints=0;
    for (int i=0;i<30;++i) {
      SendMessageW(panel,WM_MOUSEMOVE,0,point(i%2?400:80,180));
      flush(panel);
    }
    std::cout<<"hover_panel_paints="<<panel_paints<<" hover_control_paints="<<control_paints<<std::endl;
    require(panel_paints>0,"hover updates card appearance");
    require(control_paints==0,"hover never erases or repaints direction and footer buttons");
    const auto settled=panel_paints;
    for (int i=0;i<30;++i) {SendMessageW(panel,WM_MOUSEMOVE,0,point(400+i,180));flush(panel);}
    require(panel_paints==settled,"moving within one card does not repaint the panel");
    SendMessageW(panel,WM_MOUSELEAVE,0,0);flush(panel);
    const auto left=panel_paints;
    SendMessageW(panel,WM_MOUSELEAVE,0,0);flush(panel);
    require(panel_paints==left,"repeated mouse-leave does not repaint an unchanged panel");
    SendMessageW(panel,WM_KEYDOWN,VK_RIGHT,0);flush(panel);
    require(control_paints==0,"selection change on the same page leaves buttons untouched");
    SendMessageW(GetDlgItem(panel,1111),BM_CLICK,0,0);flush(panel);
    require(control_paints>0,"direction buttons still repaint when the selected list changes");
    // A full exposure repaint must also keep child windows outside the parent DC.
    HDC dc=GetDC(panel);RECT tab{};GetWindowRect(GetDlgItem(panel,1110),&tab);
    POINT corner{tab.left+10,tab.top+10};ScreenToClient(panel,&corner);
    const bool clipped=!PtVisible(dc,corner.x,corner.y);ReleaseDC(panel,dc);
    require(clipped,"parent drawing cannot cover child controls even on full repaint");
    SendMessageW(GetDlgItem(panel,1110),BM_CLICK,0,0);flush(panel);
    if(argc==2) screenshot(panel,argv[1]);
    std::cout<<"picker_paint_regression=PASS"<<std::endl;
  } catch (const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<std::endl;result=1;}
  for (const auto& probe:probes) if(IsWindow(probe.window)) SetWindowLongPtrW(probe.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(probe.previous));
  probes.clear();picker.close();
  for(HWND fixture:fixtures)DestroyWindow(fixture);
  SetCursorPos(cursor.x,cursor.y);if(IsWindow(foreground))SetForegroundWindow(foreground);
  return result;
}
