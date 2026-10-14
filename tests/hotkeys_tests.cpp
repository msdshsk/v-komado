#include "../app/hotkeys.h"
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace sharedvd;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); std::cout << "PASS " << message << std::endl; }
std::string bytes(const std::filesystem::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
void pump() { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);} }
void screenshot(HWND window, const std::filesystem::path& path) {
  RECT rect{}; GetWindowRect(window,&rect); HDC screen=GetDC(nullptr), dc=CreateCompatibleDC(screen);
  HBITMAP bitmap=CreateCompatibleBitmap(screen,rect.right-rect.left,rect.bottom-rect.top);auto old=SelectObject(dc,bitmap);
  BitBlt(dc,0,0,rect.right-rect.left,rect.bottom-rect.top,screen,rect.left,rect.top,SRCCOPY|CAPTUREBLT);SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);
  ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);
  {Gdiplus::Bitmap image(bitmap,nullptr);UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);std::vector<BYTE> data(size);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data());Gdiplus::GetImageEncoders(count,size,codecs);
    for(UINT i=0;i<count;++i)if(wcscmp(codecs[i].MimeType,L"image/png")==0)image.Save(path.c_str(),&codecs[i].Clsid,nullptr);}
  Gdiplus::GdiplusShutdown(token);DeleteObject(bitmap);
}
int wmain(int argc,wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if(argc!=2&&argc!=3)return 2;
  const auto dir=std::filesystem::path(argv[1])/ (L"hotkeys-test-"+std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(dir); const auto file=dir/L"hotkeys.conf";
  HWND owner=CreateWindowExW(0,L"STATIC",L"VKomado test host",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  HWND competitor=CreateWindowExW(0,L"STATIC",L"VKomado test competitor",WS_POPUP,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  int result=1;
  try {
    check(!FindWindowW(L"SharedVDHotkeyEditor",nullptr),"no existing editor is controlled");
    auto defaults=default_bindings(); check(validate_bindings(defaults).empty(),"legacy defaults accepted");
    auto candidate=defaults;candidate[1]=candidate[0];check(!validate_bindings(candidate).empty(),"duplicate actions rejected");
    candidate=defaults;candidate[1]={MOD_CONTROL,VK_F12};check(!validate_bindings(candidate).empty(),"reserved F12 rejected including modifiers");
    candidate[1]={0,'K'};check(!validate_bindings(candidate).empty(),"plain typing key rejected");
    candidate[1]={MOD_WIN,'K'};check(!validate_bindings(candidate).empty(),"Windows key reserved");
    candidate[1]={0,VK_F13};check(validate_bindings(candidate).empty(),"F13 hardware macro key supported");
    Hotkeys keys;keys.initialize(owner,file);
    check(defaults[2].key==VK_F8&&!keys.follow_dialogs(),"F8 direct move and opt-in dialog following defaults");
    candidate={{{MOD_CONTROL|MOD_ALT,'K'},{MOD_CONTROL|MOD_ALT,'J'},{MOD_CONTROL|MOD_ALT,'L'}}};std::wstring error;
    check(keys.apply(candidate,error,true),"three custom shortcuts and dialog preference register and save");
    check(keys.registered(0)&&keys.registered(1),"both configured actions active");
    auto saved=bytes(file);
    check(RegisterHotKey(competitor,800,MOD_NOREPEAT|MOD_CONTROL|MOD_ALT,'U')!=FALSE,"reserve competing global shortcut");
    auto blocked=candidate;blocked[1]={MOD_CONTROL|MOD_ALT,'U'};
    check(!keys.apply(blocked,error),"global collision rejected");
    check(bytes(file)==saved && keys.bindings()==candidate && keys.registered(0)&&keys.registered(1),"collision preserves file and restores old active keys");
    blocked=candidate;blocked[2]={MOD_CONTROL|MOD_ALT,'U'};
    check(!keys.apply(blocked,error,false)&&keys.follow_dialogs()&&keys.registered(2)&&bytes(file)==saved,"third-key collision preserves all bindings and dialog policy");
    UnregisterHotKey(competitor,800);
    keys.shutdown();keys.initialize(owner,file);check(keys.bindings()==candidate&&keys.follow_dialogs()&&keys.registered(2),"all three bindings and dialog option survive reload");
    auto duplicate=candidate;duplicate[2]=candidate[0];check(!validate_bindings(duplicate).empty(),"third action cannot duplicate the first");
    keys.show_editor();pump();HWND editor=FindWindowW(L"SharedVDHotkeyEditor",nullptr);
    check(editor&&GetDlgItem(editor,2100)&&GetDlgItem(editor,2101)&&GetDlgItem(editor,2102)&&GetDlgItem(editor,2130),"native editor has three key fields and dialog option");
    check(!keys.registered(0)&&!keys.registered(1)&&!keys.registered(2),"all keys suspended while recording");
    SendDlgItemMessageW(editor,2100,HKM_SETHOTKEY,MAKEWORD('Q',HOTKEYF_CONTROL|HOTKEYF_ALT),0);
    SendDlgItemMessageW(editor,2101,HKM_SETHOTKEY,MAKEWORD('Q',HOTKEYF_CONTROL|HOTKEYF_ALT),0);
    SendMessageW(editor,WM_COMMAND,IDOK,0);
    check(IsWindow(editor)&&GetWindowTextLengthW(GetDlgItem(editor,2120))>0&&bytes(file)==saved,"editor shows duplicate error without saving");
    SendMessageW(editor,WM_COMMAND,2121,0);
    check(LOWORD(SendDlgItemMessageW(editor,2100,HKM_GETHOTKEY,0,0))==MAKEWORD(VK_F6,0),"reset restores default field");
    RedrawWindow(editor,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);pump();Sleep(150);pump();
    check(IsWindowVisible(GetDlgItem(editor,IDOK))&&IsWindowVisible(GetDlgItem(editor,IDCANCEL)),"save and cancel buttons are visible");
    SetWindowPos(editor,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);pump();Sleep(150);pump();
    screenshot(editor,std::filesystem::path(argv[1])/L"hotkeys-ui.png");
    SendMessageW(editor,WM_COMMAND,IDCANCEL,0);
    check(keys.bindings()==candidate&&keys.registered(0)&&keys.registered(1),"cancel restores previous shortcuts");
    keys.show_editor();pump();editor=FindWindowW(L"SharedVDHotkeyEditor",nullptr);
    CheckDlgButton(editor,2110,BST_UNCHECKED);CheckDlgButton(editor,2111,BST_UNCHECKED);CheckDlgButton(editor,2112,BST_UNCHECKED);SendMessageW(editor,WM_COMMAND,IDOK,0);
    check(!IsWindow(editor)&&!keys.bindings()[0].key&&!keys.bindings()[1].key&&!keys.bindings()[2].key,"all three actions can be disabled from editor");
    keys.shutdown();keys.initialize(owner,file);check(!keys.registered(0)&&!keys.registered(1)&&!keys.registered(2)&&!keys.bindings()[2].key,"disabled shortcuts remain disabled after restart");
    keys.shutdown();
    {std::ofstream legacy(file);legacy<<"1\n0 119\n4 117\n";}
    keys.initialize(owner,file);check(keys.bindings()[0].key==VK_F8&&keys.bindings()[1]==KeyBinding{MOD_SHIFT,VK_F6}&&!keys.bindings()[2].key&&!keys.follow_dialogs(),"v1 migration preserves existing F8 assignment and leaves dialog following off");
    keys.shutdown();
    {std::ofstream legacy(file);legacy<<"1\n2 75\n2 74\n";}
    keys.initialize(owner,file);check(keys.bindings()[0]==KeyBinding{MOD_CONTROL,'K'}&&keys.bindings()[1]==KeyBinding{MOD_CONTROL,'J'}&&keys.bindings()[2]==KeyBinding{0,VK_F8},"v1 migration adds direct key without overwriting customized actions");
    keys.shutdown();
    const auto invalid=dir/L"directory-instead-of-file";std::filesystem::create_directory(invalid);
    keys.initialize(owner,invalid);
    const auto before=keys.bindings();
    check(!keys.apply(candidate,error)&&keys.bindings()==before,"save failure restores previous bindings");
    keys.shutdown();
    {std::ofstream malformed(file);malformed<<"broken configuration";}
    keys.initialize(owner,file);check(keys.bindings()==defaults&&!keys.load_warning().empty(),"malformed settings fall back with a visible explanation");
    keys.shutdown();
    if(argc==3) {
      STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
      std::wstring command=L"\""+std::wstring(argv[2])+L"\" --shortcuts --monitor-match NO_SUCH_MONITOR_TEST --hotkey-config \""+file.wstring()+L"\"";
      check(CreateProcessW(argv[2],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)!=FALSE,"launch standalone shortcut settings");
      HWND standalone=nullptr;
      for(int i=0;i<100&&!standalone;++i){HWND candidateWindow=FindWindowW(L"SharedVDHotkeyEditor",nullptr);DWORD pid{};if(candidateWindow)GetWindowThreadProcessId(candidateWindow,&pid);if(pid==process.dwProcessId)standalone=candidateWindow;pump();Sleep(20);}
      if(standalone)PostMessageW(standalone,WM_CLOSE,0,0);
      DWORD wait=WaitForSingleObject(process.hProcess,3000);DWORD code{};GetExitCodeProcess(process.hProcess,&code);
      if(wait!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,91);
      CloseHandle(process.hThread);CloseHandle(process.hProcess);
      check(standalone!=nullptr,"settings open without resolving a capture monitor");
      check(wait==WAIT_OBJECT_0&&code==0,"standalone settings close cleanly");
    }
    std::filesystem::remove(invalid);std::filesystem::remove(file);std::filesystem::remove(dir);
    result=0;std::cout<<"hotkeys_tests=PASS"<<std::endl;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;}
  UnregisterHotKey(competitor,800);DestroyWindow(competitor);DestroyWindow(owner);return result;
}
