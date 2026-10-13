#include "core.h"
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>

int wmain(int argc,wchar_t** argv) {
  _setmode(_fileno(stdout),_O_U8TEXT);
  _setmode(_fileno(stderr),_O_U8TEXT);
  const std::wstring action=argc>1?argv[1]:L"--inspect";
  const bool ui=argc>2 && std::wstring(argv[2])==L"--ui-errors";
  HANDLE mutex=nullptr;
  try {
    if(action==L"--inspect"){std::wcout<<svd::inspect();return 0;}
    if(action==L"--preflight")return svd::preflight();
    if(action==L"--verify-payload"){svd::validate_payload(svd::executable_dir()/L"Driver");std::wcout<<L"payload=verified\n";return 0;}
    if(action!=L"--install" && action!=L"--enable" && action!=L"--disable" && action!=L"--uninstall")throw svd::Error(L"不明な操作です。");
    if(!svd::administrator())throw svd::Error(L"管理者権限が必要です。SharedVDの管理画面から実行してください。");
    mutex=CreateMutexW(nullptr,FALSE,L"Global\\SharedVD.DriverOperation");
    const DWORD lock_result=mutex?WaitForSingleObject(mutex,0):WAIT_FAILED;
    if(lock_result!=WAIT_OBJECT_0 && lock_result!=WAIT_ABANDONED)throw svd::Error(L"別のSharedVD操作が進行中です。");
    int result=0;
    if(action==L"--install")result=svd::install();
    if(action==L"--enable")result=svd::change_power(true);
    if(action==L"--disable")result=svd::change_power(false);
    if(action==L"--uninstall")result=svd::uninstall();
    ReleaseMutex(mutex);CloseHandle(mutex);
    std::wcout<<L"result="<<result<<L"\n";
    return result;
  } catch(const svd::Error& error) {
    if(mutex)CloseHandle(mutex);
    std::wcerr<<error.message<<L"\n";
    if(ui)MessageBoxW(nullptr,error.message.c_str(),L"SharedVD — 操作を完了できませんでした",MB_OK|MB_ICONERROR);
    return 1;
  } catch(const std::exception&) {
    if(mutex)CloseHandle(mutex);
    if(ui)MessageBoxW(nullptr,L"ファイルまたはWindowsの設定にアクセスできません。導入記録を残して処理を中止しました。",L"SharedVD",MB_OK|MB_ICONERROR);
    return 1;
  }
}
