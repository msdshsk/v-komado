#include "core.h"
#include "payload_hashes.h"
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cwctype>

namespace svd {
namespace {
[[noreturn]] void fail(const std::wstring& what,DWORD error=GetLastError()) {
  throw Error(what+L" (Windows error "+std::to_wstring(error)+L")");
}
struct Handle {
  HANDLE h=nullptr;
  explicit Handle(HANDLE value=nullptr):h(value){}
  ~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
  Handle(const Handle&)=delete;
};
struct Info {
  HDEVINFO h;
  explicit Info(HDEVINFO value):h(value){if(h==INVALID_HANDLE_VALUE)fail(L"デバイス一覧の取得に失敗しました");}
  ~Info(){SetupDiDestroyDeviceInfoList(h);}
};
struct Key {
  HKEY h=nullptr;
  ~Key(){if(h)RegCloseKey(h);}
};
void set_reg(const wchar_t* key,const wchar_t* name,const std::wstring& value) {
  Key k;
  auto result=RegCreateKeyExW(HKEY_LOCAL_MACHINE,key,0,nullptr,0,KEY_WRITE|KEY_WOW64_64KEY,nullptr,&k.h,nullptr);
  if(result!=ERROR_SUCCESS)fail(L"設定の保存に失敗しました",result);
  result=RegSetValueExW(k.h,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
  if(result!=ERROR_SUCCESS)fail(L"設定の保存に失敗しました",result);
}
bool key_exists(const wchar_t* path) {
  Key key; const auto result=RegOpenKeyExW(HKEY_LOCAL_MACHINE,path,0,KEY_READ|KEY_WOW64_64KEY,&key.h);
  if(result==ERROR_FILE_NOT_FOUND)return false;
  if(result!=ERROR_SUCCESS)fail(L"設定の確認に失敗しました",result);
  return true;
}
std::vector<std::wstring> hardware(HDEVINFO info,SP_DEVINFO_DATA& data) {
  DWORD type{},bytes{};
  SetupDiGetDeviceRegistryPropertyW(info,&data,SPDRP_HARDWAREID,&type,nullptr,0,&bytes);
  if(!bytes)return {};
  std::vector<wchar_t> buffer(bytes/sizeof(wchar_t)+2,0);
  if(!SetupDiGetDeviceRegistryPropertyW(info,&data,SPDRP_HARDWAREID,&type,reinterpret_cast<BYTE*>(buffer.data()),bytes,nullptr))return {};
  std::vector<std::wstring> ids;
  for(const wchar_t* p=buffer.data();*p;p+=wcslen(p)+1)ids.emplace_back(p);
  return ids;
}
std::wstring instance_id(HDEVINFO info,SP_DEVINFO_DATA& data) {
  wchar_t id[MAX_DEVICE_ID_LEN]{};
  if(!SetupDiGetDeviceInstanceIdW(info,&data,id,MAX_DEVICE_ID_LEN,nullptr))fail(L"デバイスIDの取得に失敗しました");
  return id;
}
std::wstring device_inf(HDEVINFO info,SP_DEVINFO_DATA& data) {
  wchar_t value[MAX_PATH]{}; DEVPROPTYPE type{};
  if(!SetupDiGetDevicePropertyW(info,&data,&DEVPKEY_Device_DriverInfPath,&type,reinterpret_cast<BYTE*>(value),sizeof(value),nullptr,0))return {};
  return type==DEVPROP_TYPE_STRING ? value : L"";
}
void no_reparse(const fs::path& path) {
  for(auto current=path;!current.empty();current=current.parent_path()) {
    const DWORD attributes=GetFileAttributesW(current.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw Error(L"リンクされたインストール先には操作できません: "+current.wstring());
    if(current==current.parent_path())break;
  }
}
void mutation_context() {
  if(!administrator())throw Error(L"この操作には管理者権限が必要です。");
  if(!equal(executable_dir().wstring(),installation_dir().wstring()))throw Error(L"セットアップで導入した管理ツールから実行してください。");
  no_reparse(installation_dir());
}
bool display_matches(const DISPLAY_DEVICEW& display,const Device& device) {
  // EnumDisplayDevices exposes the hardware ID (Root\MttVDD), not the PnP
  // instance ID (ROOT\DISPLAY\0000). Callers require exactly one VDD device.
  return !device.id.empty() && vdd_hardware({display.DeviceID});
}
void physical_primary_required(const Device& device) {
  bool physical_primary=false;
  for(DWORD i=0;;++i) {
    DISPLAY_DEVICEW display{sizeof(display)};
    if(!EnumDisplayDevicesW(nullptr,i,&display,0))break;
    if((display.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE) && !display_matches(display,device))physical_primary=true;
  }
  if(!physical_primary)throw Error(L"物理ディスプレイをメイン画面に設定してから、もう一度操作してください。");
}
bool attached(const Device& device) {
  for(DWORD i=0;;++i) {
    DISPLAY_DEVICEW display{sizeof(display)};
    if(!EnumDisplayDevicesW(nullptr,i,&display,0))break;
    if(display_matches(display,device) && (display.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))return true;
  }
  return false;
}
void attach_display(const Device& device,bool initial) {
  DISPLAY_DEVICEW target{sizeof(target)};
  LONG right=0;
  unsigned candidates=0;
  for(DWORD i=0;;++i) {
    DISPLAY_DEVICEW display{sizeof(display)};
    if(!EnumDisplayDevicesW(nullptr,i,&display,0))break;
    if(display_matches(display,device)){target=display;++candidates;}
    else if(display.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
      DEVMODEW current{};current.dmSize=sizeof(current);
      if(EnumDisplaySettingsExW(display.DeviceName,ENUM_CURRENT_SETTINGS,&current,0))right=std::max(right,current.dmPosition.x+static_cast<LONG>(current.dmPelsWidth));
    }
  }
  if(candidates>1)throw Error(L"仮想画面が複数あるため、自動配置を中止しました。");
  if(!target.DeviceName[0])throw Error(L"仮想画面の準備が完了していません。再起動後にONを試してください。");
  if(!initial && (target.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))return;
  DEVMODEW mode{};mode.dmSize=sizeof(mode); bool found=false;
  for(DWORD i=0;EnumDisplaySettingsExW(target.DeviceName,i,&mode,0);++i) {
    if(mode.dmPelsWidth==1920 && mode.dmPelsHeight==1080 && mode.dmDisplayFrequency==60 && mode.dmBitsPerPel==32){found=true;break;}
  }
  if(!found)throw Error(L"このVDDには1920×1080・60Hzがありません。Windowsのディスプレイ設定を確認してください。");
  mode.dmPosition={right,0};
  mode.dmFields=DM_POSITION|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_BITSPERPEL;
  if(ChangeDisplaySettingsExW(target.DeviceName,&mode,nullptr,CDS_TEST,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw Error(L"1080p60の拡張表示を適用できません。");
  if(ChangeDisplaySettingsExW(target.DeviceName,&mode,nullptr,CDS_UPDATEREGISTRY,nullptr)!=DISP_CHANGE_SUCCESSFUL)throw Error(L"仮想画面の拡張表示に失敗しました。");
}
void remove_device(const Device& device) {
  Info info(SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY,nullptr));
  SP_DEVINFO_DATA data{sizeof(data)};
  if(!SetupDiOpenDeviceInfoW(info.h,device.id.c_str(),nullptr,0,&data))fail(L"削除する仮想画面を取得できません");
  if(!vdd_hardware(hardware(info.h,data)))throw Error(L"デバイスの種類が導入記録と一致しません。");
  BOOL reboot=FALSE;
  if(!DiUninstallDevice(nullptr,info.h,&data,0,&reboot))fail(L"仮想画面の削除に失敗しました");
  if(reboot)throw Error(L"デバイスの削除には再起動が必要です。再起動後、アンインストールをもう一度実行してください。");
}
void remove_owned(State& state) {
  auto list=devices();
  auto item=std::find_if(list.begin(),list.end(),[&](const Device& d){return equal(d.id,state.id);});
  if(state.owned_device && item!=list.end()) {
    // Never remove another VDD or an updated driver package by association.
    physical_primary_required(*item);
    remove_device(*item);
  }
  if(state.owned_inf && !state.inf.empty()) {
    if(!valid_oem_inf(state.inf))throw Error(L"ドライバーの導入記録が不正です。");
    // No force flag: packages referenced by any other device are retained.
    if(!SetupUninstallOEMInfW(state.inf.c_str(),0,nullptr)) {
      const DWORD error=GetLastError();
      if(error!=ERROR_FILE_NOT_FOUND)fail(L"ドライバーが使用中、または削除できません。再起動後に再試行してください",error);
    }
  }
  if(state.owned_vdd_path) {
    const auto current_path=read_reg(vdd_key,L"VDDPATH");
    if(!current_path.empty() && !equal(current_path,(installation_dir()/L"Driver").wstring()))throw Error(L"VDDの設定先が変更されています。設定を確認してください。");
    Key key;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,vdd_key,0,KEY_WRITE|KEY_READ|KEY_WOW64_64KEY,&key.h)==ERROR_SUCCESS) {
      RegDeleteValueW(key.h,L"VDDPATH");
      DWORD subkeys{},values{};
      RegQueryInfoKeyW(key.h,nullptr,nullptr,nullptr,&subkeys,nullptr,nullptr,&values,nullptr,nullptr,nullptr,nullptr);
      if(!subkeys && !values)RegDeleteKeyExW(HKEY_LOCAL_MACHINE,vdd_key,KEY_WOW64_64KEY,0);
    }
  }
  const auto status=RegDeleteKeyExW(HKEY_LOCAL_MACHINE,state_key,KEY_WOW64_64KEY,0);
  if(status!=ERROR_SUCCESS && status!=ERROR_FILE_NOT_FOUND)fail(L"導入記録を削除できません",status);
}
}
bool equal(const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())==0;}
bool vdd_hardware(const std::vector<std::wstring>& ids){return std::any_of(ids.begin(),ids.end(),[](const auto& id){return equal(id,hardware_id)||equal(id,L"MttVDD");});}
bool valid_oem_inf(const std::wstring& name) {
  if(name.size()<8 || !equal(name.substr(0,3),L"oem") || !equal(name.substr(name.size()-4),L".inf"))return false;
  return std::all_of(name.begin()+3,name.end()-4,[](wchar_t c){return c>=L'0' && c<=L'9';});
}
const Device& select_device(const State& state,const std::vector<Device>& list) {
  if(!state.exists || state.id.empty())throw Error(L"初期セットアップが必要です。");
  if(list.size()!=1 || !equal(list.front().id,state.id))throw Error(L"導入時の仮想画面を一意に確認できません。設定は変更しませんでした。");
  return list.front();
}
fs::path executable_dir(){wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))fail(L"実行ファイルを確認できません");return fs::path(path).parent_path();}
fs::path installation_dir(){PWSTR value=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&value)))fail(L"インストール先を確認できません");fs::path path(value);CoTaskMemFree(value);return path/L"SharedVD";}
bool administrator(){SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL result=FALSE;if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&result);FreeSid(sid);}return result!=FALSE;}
std::wstring read_reg(const wchar_t* key,const wchar_t* name){wchar_t buffer[32768]{};DWORD bytes=sizeof(buffer);const auto result=RegGetValueW(HKEY_LOCAL_MACHINE,key,name,RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,buffer,&bytes);if(result==ERROR_FILE_NOT_FOUND)return {};if(result!=ERROR_SUCCESS)fail(L"設定を読み取れません",result);return buffer;}
State read_state(){State s;s.exists=key_exists(state_key);if(!s.exists)return s;s.id=read_reg(state_key,L"DeviceId");s.inf=read_reg(state_key,L"Inf");s.directory=read_reg(state_key,L"Directory");s.phase=read_reg(state_key,L"Phase");s.owned_device=read_reg(state_key,L"OwnDevice")==L"1";s.owned_inf=read_reg(state_key,L"OwnInf")==L"1";s.owned_vdd_path=read_reg(state_key,L"OwnVddPath")==L"1";return s;}
void write_state(const State& s){set_reg(state_key,L"DeviceId",s.id);set_reg(state_key,L"Inf",s.inf);set_reg(state_key,L"Directory",s.directory);set_reg(state_key,L"OwnDevice",s.owned_device?L"1":L"0");set_reg(state_key,L"OwnInf",s.owned_inf?L"1":L"0");set_reg(state_key,L"OwnVddPath",s.owned_vdd_path?L"1":L"0");set_reg(state_key,L"Phase",s.phase);}
std::vector<Device> devices(){
  Info info(SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY,nullptr,nullptr,0));std::vector<Device> result;
  for(DWORD i=0;;++i){SP_DEVINFO_DATA data{sizeof(data)};if(!SetupDiEnumDeviceInfo(info.h,i,&data)){if(GetLastError()!=ERROR_NO_MORE_ITEMS)fail(L"デバイスの列挙に失敗しました");break;}if(!vdd_hardware(hardware(info.h,data)))continue;Device d;d.id=instance_id(info.h,data);d.inf=device_inf(info.h,data);ULONG status{};d.present=CM_Get_DevNode_Status(&status,&d.problem,data.DevInst,0)==CR_SUCCESS;d.enabled=d.present && (status&DN_STARTED) && !(status&DN_HAS_PROBLEM);result.push_back(d);}return result;
}
std::wstring sha256(const fs::path& file){
  std::ifstream stream(file,std::ios::binary);if(!stream)throw Error(L"ファイルを読み取れません: "+file.wstring());
  BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
  if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw Error(L"SHA256の初期化に失敗しました。");
  DWORD size{},bytes{};BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&bytes,0);
  std::vector<BYTE> object(size),buffer(65536);BYTE digest[32]{};
  NTSTATUS status=BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0);
  while(status>=0 && stream){stream.read(reinterpret_cast<char*>(buffer.data()),buffer.size());auto count=static_cast<ULONG>(stream.gcount());if(count)status=BCryptHashData(hash,buffer.data(),count,0);}
  if(stream.bad())status=-1;
  if(status>=0)status=BCryptFinishHash(hash,digest,sizeof(digest),0);
  if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
  if(status<0)throw Error(L"SHA256の計算に失敗しました。");
  std::wostringstream out;out<<std::hex<<std::setfill(L'0');for(BYTE b:digest)out<<std::setw(2)<<static_cast<unsigned>(b);return out.str();
}
void validate_payload(const fs::path& dir){
  for(const auto& entry:std::vector<std::pair<std::wstring,std::wstring>>{{L"MttVDD.inf",MttVDD_inf_hash},{L"MttVDD.dll",MttVDD_dll_hash},{L"mttvdd.cat",mttvdd_cat_hash}}){no_reparse(dir/entry.first);if(!equal(sha256(dir/entry.first),entry.second))throw Error(L"同梱ドライバーの整合性検証に失敗しました: "+entry.first);}
}
DWORD run(const fs::path& exe,const std::wstring& arguments,DWORD timeout){
  std::wstring command=L"\""+exe.wstring()+L"\" "+arguments;STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&process))fail(L"処理を開始できません");
  Handle thread(process.hThread),handle(process.hProcess);if(WaitForSingleObject(handle.h,timeout)!=WAIT_OBJECT_0)throw Error(L"処理が完了していません。しばらく待ってから状態を確認してください。");DWORD code{};GetExitCodeProcess(handle.h,&code);return code;
}
void stop_viewer_and_rescue(){
  const auto list=devices();if(list.size()>1)throw Error(L"VDDが複数あるため、自動退避の対象を確定できません。");
  if(!list.empty() && attached(list.front())) {
    physical_primary_required(list.front());
    const DWORD code=run(executable_dir()/L"VKomado.Viewer.exe",L"--rescue-windows");
    if(code!=0)throw Error(L"ウィンドウを退避できなかったため、停止・削除を中止しました。");
  }
  HWND viewer=FindWindowW(L"LGWLViewerWindow",nullptr);
  if(viewer){DWORD pid{};GetWindowThreadProcessId(viewer,&pid);Handle process(OpenProcess(SYNCHRONIZE,FALSE,pid));if(!process.h)fail(L"ビューアを確認できません");PostMessageW(viewer,WM_CLOSE,0,0);if(WaitForSingleObject(process.h,7000)!=WAIT_OBJECT_0)throw Error(L"ビューアを通常終了してから、もう一度実行してください。");}
}
void launch_viewer(){if(FindWindowW(L"LGWLViewerWindow",nullptr))return;const auto exe=executable_dir()/L"VKomado.Viewer.exe";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};std::wstring command=L"\""+exe.wstring()+L"\"";if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,exe.parent_path().c_str(),&startup,&process))fail(L"ビューアを起動できません");CloseHandle(process.hThread);CloseHandle(process.hProcess);}
std::wstring inspect(){const auto s=read_state();const auto list=devices();std::wostringstream out;out<<L"VKomado 1.1.0\n";out<<L"installation="<<(s.exists?s.directory:L"not installed")<<L"\n";out<<L"phase="<<s.phase<<L"\n";out<<L"driver_ownership="<<(s.owned_device?L"owned":L"existing / external")<<L"\n";out<<L"device_count="<<list.size()<<L"\n";for(const auto& d:list)out<<L"id="<<d.id<<L" inf="<<d.inf<<L" enabled="<<d.enabled<<L" problem="<<d.problem<<L" attached="<<attached(d)<<L"\n";
  for(DWORD i=0;;++i){DISPLAY_DEVICEW display{sizeof(display)};if(!EnumDisplayDevicesW(nullptr,i,&display,0))break;out<<L"display="<<display.DeviceName<<L" id="<<display.DeviceID<<L" flags="<<display.StateFlags<<L"\n";}
  return out.str();}
int preflight(){
  no_reparse(installation_dir());no_reparse(installation_dir()/L"Driver");
  const auto s=read_state();const auto list=devices();
  if(list.size()>1)throw Error(L"既存のVDDが複数あります。対象を1台に整理してから導入してください。");
  if(s.exists){if(!equal(s.directory,installation_dir().wstring()))throw Error(L"既存の導入先が異なります。");if(s.phase!=L"ready")throw Error(L"前回の処理が未完了です。先にVKomadoをアンインストールしてください。");select_device(s,list);}
  if(list.empty() && (key_exists(vdd_key) || fs::exists(L"C:\\VirtualDisplayDriver")))throw Error(L"既存のVDD設定が残っています。上書きせず導入を中止しました。");
  return 0;
}
int install(){
  mutation_context();preflight();validate_payload(executable_dir()/L"Driver");
  auto s=read_state();if(s.exists)return 0;
  auto list=devices();s.exists=true;s.directory=installation_dir().wstring();s.phase=L"installing";
  if(!list.empty()){s.id=list.front().id;s.inf=list.front().inf;s.phase=L"ready";write_state(s);return 0;}
  write_state(s);
  try {
    // Own only a previously absent VDD registry key. Driver files stay under Program Files.
    s.owned_vdd_path=true;write_state(s);
    set_reg(vdd_key,L"VDDPATH",(installation_dir()/L"Driver").wstring());
    wchar_t destination[MAX_PATH]{};
    const BOOL copied=SetupCopyOEMInfW((executable_dir()/L"Driver"/L"MttVDD.inf").c_str(),nullptr,SPOST_PATH,SP_COPY_NOOVERWRITE,destination,MAX_PATH,nullptr,nullptr);
    const DWORD copy_error=GetLastError();
    if(!copied && copy_error!=ERROR_FILE_EXISTS)fail(L"署名付きドライバーを登録できません",copy_error);
    s.inf=fs::path(destination).filename().wstring();if(!valid_oem_inf(s.inf))throw Error(L"登録したドライバー名を確認できません。");s.owned_inf=copied!=FALSE;write_state(s);
    Info info(SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY,nullptr));SP_DEVINFO_DATA data{sizeof(data)};
    if(!SetupDiCreateDeviceInfoW(info.h,L"Display",&GUID_DEVCLASS_DISPLAY,L"VKomado Virtual Display",nullptr,DICD_GENERATE_ID,&data))fail(L"仮想画面を作成できません");
    constexpr wchar_t ids[]=L"Root\\MttVDD\0";
    if(!SetupDiSetDeviceRegistryPropertyW(info.h,&data,SPDRP_HARDWAREID,reinterpret_cast<const BYTE*>(ids),sizeof(ids)))fail(L"仮想画面を設定できません");
    s.id=instance_id(info.h,data);s.owned_device=true;write_state(s);
    if(!SetupDiCallClassInstaller(DIF_REGISTERDEVICE,info.h,&data))fail(L"仮想画面を登録できません");
    SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
    if(!SetupDiGetDeviceInstallParamsW(info.h,&data,&parameters))fail(L"導入情報を取得できません");
    parameters.Flags|=DI_ENUMSINGLEINF;
    wcscpy_s(parameters.DriverPath,(executable_dir()/L"Driver"/L"MttVDD.inf").c_str());
    if(!SetupDiSetDeviceInstallParamsW(info.h,&data,&parameters) || !SetupDiBuildDriverInfoList(info.h,&data,SPDIT_COMPATDRIVER))fail(L"ドライバーの照合に失敗しました");
    SP_DRVINFO_DATA_W driver{sizeof(driver)};if(!SetupDiEnumDriverInfoW(info.h,&data,SPDIT_COMPATDRIVER,0,&driver))fail(L"対応するドライバーがありません");
    BOOL reboot=FALSE;const BOOL installed=DiInstallDevice(nullptr,info.h,&data,&driver,DIIDFLAG_NOFINISHINSTALLUI,&reboot);const DWORD error=GetLastError();SetupDiDestroyDriverInfoList(info.h,&data,SPDIT_COMPATDRIVER);if(!installed)fail(L"ドライバーを導入できません",error);
    s.phase=L"ready";write_state(s);
    if(reboot)return 3010;
    for(int i=0;i<30;++i){auto current=devices();if(current.size()==1 && current.front().enabled){try{attach_display(current.front(),true);return 0;}catch(const Error&){}}Sleep(200);}
    // Installation is valid; a restart may be needed before the monitor arrives.
    return 3010;
  } catch(...) {try{remove_owned(s);}catch(...){s.phase=L"cleanup-required";write_state(s);}throw;}
}
int change_power(bool enabled){
  mutation_context();const auto s=read_state();if(s.phase!=L"ready" || !equal(s.directory,installation_dir().wstring()))throw Error(L"初期セットアップが完了していません。");const auto list=devices();const auto device=select_device(s,list);physical_primary_required(device);
  if(!enabled)stop_viewer_and_rescue();
  if(device.enabled!=enabled){wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);const DWORD code=run(fs::path(system)/L"pnputil.exe",std::wstring(enabled?L"/enable-device ":L"/disable-device ")+L"\""+device.id+L"\"");if(code==3010)return 3010;if(code!=0)throw Error(L"仮想画面の状態変更に失敗しました ("+std::to_wstring(code)+L")");}
  for(int i=0;i<30;++i){const auto current=devices();const auto& d=select_device(s,current);if(enabled && d.enabled){try{attach_display(d,false);return 0;}catch(const Error&){}}if(!enabled && d.present && d.problem==CM_PROB_DISABLED)return 0;Sleep(200);}
  throw Error(L"仮想画面の状態を確認できません。再起動後に状態を確認してください。");
}
int uninstall(){mutation_context();auto s=read_state();if(!s.exists)return 0;if(!equal(s.directory,installation_dir().wstring()))throw Error(L"導入先が導入記録と一致しません。");stop_viewer_and_rescue();s.phase=L"uninstalling";write_state(s);remove_owned(s);return 0;}
}
