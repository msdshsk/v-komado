#include "../app/core.h"
#include <objidl.h>
#include <gdiplus.h>
#include <iostream>
int wmain(int argc,wchar_t** argv){
  if(argc!=3 || FindWindowW(L"SharedVDControlWindow",nullptr))return 2;
  PROCESS_INFORMATION process{};STARTUPINFOW startup{sizeof(startup)};
  std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
  if(!CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process))return 3;
  HWND window=nullptr;bool passed=false;
  try{
    for(int i=0;i<100;++i){window=FindWindowW(L"SharedVDControlWindow",nullptr);DWORD pid{};if(window)GetWindowThreadProcessId(window,&pid);if(pid==process.dwProcessId)break;window=nullptr;Sleep(30);}
    if(!window)throw std::runtime_error("manager window missing");
    wchar_t title[256]{};GetWindowTextW(window,title,256);
    if(std::wstring(title).find(L"VKomado")!=0)throw std::runtime_error("manager title has obsolete branding");
    Sleep(250);
    for(int id:{101,102,103,104,105})if(!GetDlgItem(window,id))throw std::runtime_error("missing action button");
    if(!svd::read_state().exists && (IsWindowEnabled(GetDlgItem(window,101)) || IsWindowEnabled(GetDlgItem(window,102))))throw std::runtime_error("power controls enabled before installation");
    DWORD_PTR response{};if(!SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG,1000,&response))throw std::runtime_error("UI not responsive");
    RECT rect{};GetWindowRect(window,&rect);HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,rect.right-rect.left,rect.bottom-rect.top);auto old=SelectObject(dc,bitmap);
    const BOOL captured=PrintWindow(window,dc,2);SelectObject(dc,old);DeleteDC(dc);ReleaseDC(nullptr,screen);
    if(!captured){DeleteObject(bitmap);throw std::runtime_error("capture failed");}
    ULONG_PTR token{};Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);
    {
      Gdiplus::Bitmap image(bitmap,nullptr);UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);std::vector<BYTE> storage(size);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());Gdiplus::GetImageEncoders(count,size,codecs);bool saved=false;
      for(UINT i=0;i<count;++i)if(wcscmp(codecs[i].MimeType,L"image/png")==0)saved=image.Save(argv[2],&codecs[i].Clsid,nullptr)==Gdiplus::Ok;
      if(!saved)throw std::runtime_error("PNG save failed");
    }
    Gdiplus::GdiplusShutdown(token);DeleteObject(bitmap);passed=true;
  }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;}
  if(window)PostMessageW(window,WM_CLOSE,0,0);
  const DWORD wait=WaitForSingleObject(process.hProcess,3000);CloseHandle(process.hThread);CloseHandle(process.hProcess);
  std::cout<<"manager_ui="<<(passed && wait==WAIT_OBJECT_0?"PASS":"FAIL")<<std::endl;
  return passed && wait==WAIT_OBJECT_0?0:1;
}
