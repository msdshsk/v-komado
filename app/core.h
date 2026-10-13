#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>

namespace svd {
namespace fs = std::filesystem;
inline constexpr wchar_t state_key[] = L"SOFTWARE\\SharedVD";
inline constexpr wchar_t vdd_key[] = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
inline constexpr wchar_t hardware_id[] = L"Root\\MttVDD";
struct Error : std::runtime_error {
  std::wstring message;
  explicit Error(std::wstring text) : std::runtime_error("SharedVD error"), message(std::move(text)) {}
};
struct Device { std::wstring id, inf; bool present=false, enabled=false; ULONG problem=0; };
struct State {
  bool exists=false, owned_device=false, owned_inf=false, owned_vdd_path=false;
  std::wstring id, inf, directory, phase;
};
bool equal(const std::wstring& a,const std::wstring& b);
bool vdd_hardware(const std::vector<std::wstring>& ids);
bool valid_oem_inf(const std::wstring& name);
// Fails closed when an installation's device is missing/replaced or ambiguous.
const Device& select_device(const State& state,const std::vector<Device>& devices);
fs::path executable_dir();
fs::path installation_dir();
bool administrator();
std::vector<Device> devices();
State read_state();
void write_state(const State& state);
void validate_payload(const fs::path& dir);
std::wstring sha256(const fs::path& file);
std::wstring read_reg(const wchar_t* key,const wchar_t* name);
std::wstring inspect();
int install();
int change_power(bool enabled);
int uninstall();
int preflight();
void stop_viewer_and_rescue();
void launch_viewer();
DWORD run(const fs::path& exe,const std::wstring& args,DWORD timeout=30000);
}
