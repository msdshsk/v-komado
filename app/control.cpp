#include "core.h"
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
HWND main_window{},status_label{},description{},on_button{},off_button{},open_button{},uninstall_button{};
HFONT font{},heading_font{};
HANDLE operation{};
bool open_after=false;
int scale(int value){return MulDiv(value,static_cast<int>(GetDpiForWindow(main_window)),96);}
void error(const std::wstring& message){MessageBoxW(main_window,message.c_str(),L"VKomado",MB_OK|MB_ICONERROR);}
void refresh(){
  try {
    const auto state=svd::read_state();const auto list=svd::devices();
    std::wstring status;
    bool usable=false,enabled=false;
    if(!state.exists)status=L"セットアップが必要です";
    else if(state.phase!=L"ready")status=L"前回の処理が未完了です。アンインストールで整理できます。";
    else {const auto& device=svd::select_device(state,list);usable=true;enabled=device.enabled;status=enabled?L"仮想ディスプレイ：ON":L"仮想ディスプレイ：OFF";if(device.problem && device.problem!=22)status=L"仮想ディスプレイに問題があります（再起動をお試しください）";}
    SetWindowTextW(status_label,operation?L"処理中です…":status.c_str());
    const wchar_t* detail=!state.exists ? L"VKomado-Setup.exeを実行してください。\r\n初回導入後は、この画面からON／OFFできます。" :
      state.owned_device ? L"このアプリが導入した仮想ディスプレイを管理しています。\r\nOFFにする前に、仮想画面のアプリをメイン画面へ戻します。" :
      L"既存のVDDを再利用しています。\r\nON／OFFはできます。VKomadoを削除しても既存VDDは残ります。";
    SetWindowTextW(description,detail);
    EnableWindow(on_button,usable && !operation);
    EnableWindow(off_button,usable && !operation && enabled);
    EnableWindow(open_button,usable && !operation && enabled);
    EnableWindow(uninstall_button,!operation && svd::fs::exists(svd::executable_dir()/L"unins000.exe"));
  }catch(const svd::Error& e){SetWindowTextW(status_label,e.message.c_str());EnableWindow(on_button,FALSE);EnableWindow(off_button,FALSE);EnableWindow(open_button,FALSE);}
}
void elevate(bool enabled){
  if(operation)return;
  const auto exe=svd::executable_dir()/L"VKomado.Device.exe";
  const std::wstring args=enabled?L"--enable --ui-errors":L"--disable --ui-errors";
  SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS;info.hwnd=main_window;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args.c_str();info.nShow=SW_HIDE;
  if(!ShellExecuteExW(&info)){if(GetLastError()!=ERROR_CANCELLED)error(L"管理者権限での処理を開始できませんでした。");return;}
  operation=info.hProcess;open_after=enabled;refresh();
}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
  switch(message){
    case WM_CTLCOLORSTATIC:
      SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_CREATE:{
      main_window=window;
      font=CreateFontW(-scale(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
      heading_font=CreateFontW(-scale(23),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Yu Gothic UI");
      auto control=[&](const wchar_t* type,const wchar_t* text,int x,int y,int width,int height,int id,DWORD extra=0){HWND h=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|extra,scale(x),scale(y),scale(width),scale(height),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;};
      HWND heading=control(L"STATIC",L"VKomado",24,20,520,40,0);SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(heading_font),TRUE);
      status_label=control(L"STATIC",L"状態を確認しています…",24,78,550,58,0);
      description=control(L"STATIC",L"",24,140,550,64,0);
      on_button=control(L"BUTTON",L"仮想画面をON",24,222,172,42,101,WS_TABSTOP);
      off_button=control(L"BUTTON",L"仮想画面をOFF",210,222,172,42,102,WS_TABSTOP);
      open_button=control(L"BUTTON",L"ビューアを開く",396,222,172,42,103,WS_TABSTOP);
      uninstall_button=control(L"BUTTON",L"アンインストール",24,284,172,34,104,WS_TABSTOP);
      control(L"BUTTON",L"ショートカット設定",210,284,172,34,105,WS_TABSTOP);
      control(L"STATIC",L"1.2.0  ·  ON／OFF時に管理者の確認が表示されます",24,338,540,30,0);
      SendMessageW(on_button,BCM_SETSHIELD,0,TRUE);SendMessageW(off_button,BCM_SETSHIELD,0,TRUE);
      SetTimer(window,1,1000,nullptr);refresh();return 0;
    }
    case WM_TIMER:
      if(operation && WaitForSingleObject(operation,0)==WAIT_OBJECT_0){DWORD code{};GetExitCodeProcess(operation,&code);CloseHandle(operation);operation=nullptr;
        if(code==3010)MessageBoxW(window,L"変更の反映にはWindowsの再起動が必要です。作業を保存してから再起動してください。",L"VKomado",MB_OK|MB_ICONINFORMATION);
        else if(code==0 && open_after){try{svd::launch_viewer();}catch(const svd::Error& e){error(e.message);}}
        open_after=false;
      }
      refresh();return 0;
    case WM_COMMAND:
      try {
        if(LOWORD(wp)==101)elevate(true);
        if(LOWORD(wp)==102)elevate(false);
        if(LOWORD(wp)==103)svd::launch_viewer();
        if(LOWORD(wp)==105){const auto exe=svd::executable_dir()/L"VKomado.Viewer.exe";if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",exe.c_str(),L"--shortcuts",nullptr,SW_SHOWNORMAL))<=32)error(L"ショートカット設定を開けませんでした。");}
        if(LOWORD(wp)==104){const auto exe=svd::executable_dir()/L"unins000.exe";SHELLEXECUTEINFOW info{sizeof(info)};info.hwnd=window;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=L"/NORESTART";info.nShow=SW_SHOWNORMAL;if(ShellExecuteExW(&info))DestroyWindow(window);else if(GetLastError()!=ERROR_CANCELLED)error(L"アンインストールを開始できませんでした。");}
      }catch(const svd::Error& e){error(e.message);}return 0;
    case WM_CLOSE:
      if(operation){MessageBoxW(window,L"処理が完了するまでお待ちください。",L"VKomado",MB_OK);return 0;}
      DestroyWindow(window);return 0;
    case WM_DESTROY:KillTimer(window,1);DeleteObject(font);DeleteObject(heading_font);PostQuitMessage(0);return 0;
  }
  return DefWindowProcW(window,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\SharedVD.Control");if(GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(L"SharedVDControlWindow",nullptr);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}if(mutex)CloseHandle(mutex);return 0;}
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
  WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));cls.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);cls.lpszClassName=L"SharedVDControlWindow";RegisterClassExW(&cls);
  const UINT dpi=GetDpiForSystem();RECT rect{0,0,MulDiv(594,dpi,96),MulDiv(390,dpi,96)};const DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;AdjustWindowRectExForDpi(&rect,style,FALSE,0,dpi);
  HWND window=CreateWindowExW(0,cls.lpszClassName,L"VKomado — 仮想ディスプレイ",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,instance,nullptr);if(!window){if(mutex)CloseHandle(mutex);return 1;}ShowWindow(window,SW_SHOWNORMAL);
  MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
  if(mutex)CloseHandle(mutex);return 0;
}
