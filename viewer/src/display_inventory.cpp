#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring hresult_text(HRESULT hr) {
  wchar_t* buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD length = FormatMessageW(
      flags, nullptr, static_cast<DWORD>(hr), 0,
      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

  std::wostringstream stream;
  stream << L"0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill(L'0') << static_cast<std::uint32_t>(hr);
  if (length != 0 && buffer != nullptr) {
    std::wstring message(buffer, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            std::iswspace(message.back()))) {
      message.pop_back();
    }
    stream << L" (" << message << L")";
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return stream.str();
}

std::wstring luid_text(const LUID& luid) {
  std::wostringstream stream;
  stream << L"0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill(L'0') << static_cast<std::uint32_t>(luid.HighPart)
         << L":" << std::setw(8) << luid.LowPart;
  return stream.str();
}

bool same_luid(const LUID& left, const LUID& right) {
  return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

std::wstring lower_copy(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return value;
}

bool contains_case_insensitive(const std::wstring& value,
                               const std::wstring& needle) {
  return lower_copy(value).find(lower_copy(needle)) != std::wstring::npos;
}

std::wstring feature_level_text(D3D_FEATURE_LEVEL level) {
  switch (level) {
    case D3D_FEATURE_LEVEL_12_2: return L"12_2";
    case D3D_FEATURE_LEVEL_12_1: return L"12_1";
    case D3D_FEATURE_LEVEL_12_0: return L"12_0";
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    case D3D_FEATURE_LEVEL_10_1: return L"10_1";
    case D3D_FEATURE_LEVEL_10_0: return L"10_0";
    default: return L"unknown";
  }
}

struct ActivePath {
  LUID adapter_luid{};
  UINT source_id = 0;
  UINT target_id = 0;
  std::wstring source_name;
  std::wstring target_friendly_name;
  std::wstring target_device_path;
  RECT desktop_rect{};
  double refresh_hz = 0.0;
  bool primary = false;
};

std::vector<ActivePath> enumerate_active_paths() {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  LONG result = GetDisplayConfigBufferSizes(
      QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
  if (result != ERROR_SUCCESS) {
    std::wcerr << L"GetDisplayConfigBufferSizes failed: " << result << L"\n";
    return {};
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                              &mode_count, modes.data(), nullptr);
  if (result != ERROR_SUCCESS) {
    std::wcerr << L"QueryDisplayConfig failed: " << result << L"\n";
    return {};
  }
  paths.resize(path_count);
  modes.resize(mode_count);

  POINT primary_point{0, 0};
  std::vector<ActivePath> output;
  output.reserve(paths.size());

  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
      continue;
    }

    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
      continue;
    }

    ActivePath item;
    item.adapter_luid = path.sourceInfo.adapterId;
    item.source_id = path.sourceInfo.id;
    item.target_id = path.targetInfo.id;
    item.source_name = source.viewGdiDeviceName;
    item.target_friendly_name = target.monitorFriendlyDeviceName;
    item.target_device_path = target.monitorDevicePath;
    if (path.targetInfo.refreshRate.Denominator != 0) {
      item.refresh_hz =
          static_cast<double>(path.targetInfo.refreshRate.Numerator) /
          static_cast<double>(path.targetInfo.refreshRate.Denominator);
    }

    if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
        path.sourceInfo.modeInfoIdx < modes.size()) {
      const auto& mode = modes[path.sourceInfo.modeInfoIdx];
      if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const LONG left = mode.sourceMode.position.x;
        const LONG top = mode.sourceMode.position.y;
        item.desktop_rect = RECT{
            left,
            top,
            left + static_cast<LONG>(mode.sourceMode.width),
            top + static_cast<LONG>(mode.sourceMode.height)};
      }
    }
    item.primary = PtInRect(&item.desktop_rect, primary_point) != FALSE;
    output.push_back(std::move(item));
  }
  return output;
}

std::vector<DWORD> available_refresh_rates(const ActivePath& path) {
  const DWORD width = static_cast<DWORD>(
      std::max<LONG>(0, path.desktop_rect.right - path.desktop_rect.left));
  const DWORD height = static_cast<DWORD>(
      std::max<LONG>(0, path.desktop_rect.bottom - path.desktop_rect.top));
  std::set<DWORD> rates;
  for (DWORD mode_index = 0;; ++mode_index) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(path.source_name.c_str(), mode_index, &mode, 0) ==
        FALSE) {
      break;
    }
    if (mode.dmPelsWidth == width && mode.dmPelsHeight == height &&
        mode.dmDisplayFrequency > 1) {
      rates.insert(mode.dmDisplayFrequency);
    }
  }
  return {rates.begin(), rates.end()};
}

struct RefreshChangeResult {
  LONG test_result = DISP_CHANGE_FAILED;
  LONG apply_result = DISP_CHANGE_FAILED;
  bool matching_mode_found = false;
};

RefreshChangeResult set_refresh_rate(const ActivePath& path,
                                     DWORD requested_refresh_hz) {
  RefreshChangeResult result;

  DEVMODEW current{};
  current.dmSize = sizeof(current);
  if (EnumDisplaySettingsExW(path.source_name.c_str(), ENUM_CURRENT_SETTINGS,
                             &current, 0) == FALSE) {
    return result;
  }

  std::optional<DEVMODEW> best_match;
  std::optional<DEVMODEW> fallback_match;
  for (DWORD mode_index = 0;; ++mode_index) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(path.source_name.c_str(), mode_index, &mode, 0) ==
        FALSE) {
      break;
    }
    if (mode.dmPelsWidth != current.dmPelsWidth ||
        mode.dmPelsHeight != current.dmPelsHeight ||
        mode.dmDisplayFrequency != requested_refresh_hz) {
      continue;
    }
    if (!fallback_match.has_value()) {
      fallback_match = mode;
    }
    if (mode.dmBitsPerPel == current.dmBitsPerPel &&
        mode.dmDisplayOrientation == current.dmDisplayOrientation) {
      best_match = mode;
      break;
    }
  }

  if (!best_match.has_value()) {
    best_match = fallback_match;
  }
  if (!best_match.has_value()) {
    result.test_result = DISP_CHANGE_BADMODE;
    result.apply_result = DISP_CHANGE_BADMODE;
    return result;
  }

  result.matching_mode_found = true;
  DEVMODEW selected = *best_match;
  selected.dmPosition = current.dmPosition;
  selected.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT |
                       DM_DISPLAYFREQUENCY | DM_BITSPERPEL;

  result.test_result = ChangeDisplaySettingsExW(
      path.source_name.c_str(), &selected, nullptr, CDS_TEST, nullptr);
  if (result.test_result != DISP_CHANGE_SUCCESSFUL) {
    result.apply_result = result.test_result;
    return result;
  }

  result.apply_result = ChangeDisplaySettingsExW(
      path.source_name.c_str(), &selected, nullptr, CDS_UPDATEREGISTRY,
      nullptr);
  return result;
}

struct InventoryOptions {
  std::wstring monitor_match;
  std::optional<DWORD> requested_refresh;
};

InventoryOptions parse_options(int argument_count, wchar_t** arguments) {
  InventoryOptions options;
  for (int index = 1; index < argument_count; ++index) {
    const std::wstring argument = arguments[index];
    if (argument == L"--monitor-match" && index + 1 < argument_count) {
      options.monitor_match = arguments[++index];
    } else if (argument == L"--set-refresh" &&
               index + 1 < argument_count) {
      wchar_t* end = nullptr;
      const unsigned long parsed = std::wcstoul(arguments[++index], &end, 10);
      if (end == arguments[index] || *end != L'\0' || parsed < 24 ||
          parsed > 500) {
        std::wcerr << L"Invalid refresh rate: " << arguments[index] << L"\n";
        std::exit(64);
      }
      options.requested_refresh = static_cast<DWORD>(parsed);
    } else {
      std::wcerr << L"Usage: lgwl_display_inventory.exe "
                    L"[--monitor-match <text>] [--set-refresh <Hz>]\n";
      std::exit(64);
    }
  }
  return options;
}

bool path_matches(const ActivePath& path, const std::wstring& match) {
  return contains_case_insensitive(path.source_name, match) ||
         contains_case_insensitive(path.target_friendly_name, match) ||
         contains_case_insensitive(path.target_device_path, match);
}

std::vector<const ActivePath*> select_paths(
    const std::vector<ActivePath>& paths, const std::wstring& monitor_match) {
  std::vector<const ActivePath*> matches;
  for (const auto& path : paths) {
    if ((!monitor_match.empty() && path_matches(path, monitor_match)) ||
        (monitor_match.empty() && !path.primary)) {
      matches.push_back(&path);
    }
  }
  return matches;
}

struct DuplicationProbe {
  bool d3d11_device_created = false;
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
  HRESULT device_result = E_FAIL;
  HRESULT duplication_result = E_FAIL;
  std::wstring method = L"none";
};

DuplicationProbe probe_duplication(IDXGIAdapter1* adapter, IDXGIOutput* output) {
  DuplicationProbe probe;
  const D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  probe.device_result = D3D11CreateDevice(
      adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
      &device, &probe.feature_level, &context);
  if (FAILED(probe.device_result)) {
    return probe;
  }
  probe.d3d11_device_created = true;

  ComPtr<IDXGIOutput5> output5;
  if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output5)))) {
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };
    ComPtr<IDXGIOutputDuplication> duplication;
    probe.method = L"DuplicateOutput1";
    probe.duplication_result = output5->DuplicateOutput1(
        device.Get(), 0, static_cast<UINT>(std::size(formats)), formats,
        &duplication);
    return probe;
  }

  ComPtr<IDXGIOutput1> output1;
  probe.duplication_result = output->QueryInterface(IID_PPV_ARGS(&output1));
  if (FAILED(probe.duplication_result)) {
    probe.method = L"IDXGIOutput1 unavailable";
    return probe;
  }

  ComPtr<IDXGIOutputDuplication> duplication;
  probe.method = L"DuplicateOutput";
  probe.duplication_result = output1->DuplicateOutput(device.Get(), &duplication);
  return probe;
}

void print_path(const ActivePath& path) {
  const auto refresh_rates = available_refresh_rates(path);
  std::wcout << L"  source=" << path.source_name
             << L" adapter_luid=" << luid_text(path.adapter_luid)
             << L" source_id=" << path.source_id
             << L" target_id=" << path.target_id << L"\n"
             << L"  target_friendly=" << path.target_friendly_name << L"\n"
             << L"  target_path=" << path.target_device_path << L"\n"
             << L"  rect=[" << path.desktop_rect.left << L"," << path.desktop_rect.top
             << L"," << path.desktop_rect.right << L"," << path.desktop_rect.bottom
             << L"] primary=" << (path.primary ? L"true" : L"false")
             << L" current_refresh_hz=" << std::fixed << std::setprecision(3)
             << path.refresh_hz << L"\n"
             << L"  available_refresh_hz=[";
  for (std::size_t index = 0; index < refresh_rates.size(); ++index) {
    if (index != 0) {
      std::wcout << L",";
    }
    std::wcout << refresh_rates[index];
  }
  std::wcout << L"]\n";
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
  // SetConsoleOutputCP alone does not change the CRT's C locale. Localized
  // FormatMessageW text otherwise sets wcout failbit and truncates the report.
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
  SetConsoleOutputCP(CP_UTF8);
  std::wcout << L"LGWL display inventory\n";

  const InventoryOptions options = parse_options(argument_count, arguments);
  auto active_paths = enumerate_active_paths();
  auto selected_paths = select_paths(active_paths, options.monitor_match);

  if (options.requested_refresh.has_value()) {
    if (selected_paths.size() != 1) {
      std::wcerr << L"FAIL CLOSED: refresh change requires exactly one active "
                    L"target, observed "
                 << selected_paths.size() << L". Use --monitor-match.\n";
      return 4;
    }

    const ActivePath before = *selected_paths.front();
    const auto offered_rates = available_refresh_rates(before);
    if (std::find(offered_rates.begin(), offered_rates.end(),
                  *options.requested_refresh) == offered_rates.end()) {
      std::wcerr << L"FAIL CLOSED: target does not advertise "
                 << *options.requested_refresh << L" Hz at the active "
                 << before.desktop_rect.right - before.desktop_rect.left
                 << L"x"
                 << before.desktop_rect.bottom - before.desktop_rect.top
                 << L" resolution.\n";
      return 5;
    }

    std::wcout << L"\n[Refresh change request]\n"
               << L"target_path=" << before.target_device_path << L"\n"
               << L"target_luid=" << luid_text(before.adapter_luid) << L"\n"
               << L"source_before=" << before.source_name << L"\n"
               << L"refresh_before_hz=" << std::fixed << std::setprecision(3)
               << before.refresh_hz << L"\n"
               << L"refresh_requested_hz=" << *options.requested_refresh
               << L"\n";

    if (std::abs(before.refresh_hz -
                 static_cast<double>(*options.requested_refresh)) >= 0.5) {
      const RefreshChangeResult change =
          set_refresh_rate(before, *options.requested_refresh);
      std::wcout << L"matching_mode_found="
                 << (change.matching_mode_found ? L"true" : L"false")
                 << L"\n"
                 << L"refresh_test_result=" << change.test_result << L"\n"
                 << L"refresh_apply_result=" << change.apply_result << L"\n";
      if (change.test_result != DISP_CHANGE_SUCCESSFUL ||
          change.apply_result != DISP_CHANGE_SUCCESSFUL) {
        std::wcerr << L"FAIL CLOSED: refresh change was rejected.\n";
        return 6;
      }
      Sleep(1200);
      active_paths = enumerate_active_paths();
      selected_paths = select_paths(active_paths, options.monitor_match);
    } else {
      std::wcout << L"refresh_change_skipped=already_active\n";
    }

    const ActivePath* after = nullptr;
    if (selected_paths.size() == 1) {
      after = selected_paths.front();
    }
    if (after == nullptr ||
        after->target_device_path != before.target_device_path ||
        !same_luid(after->adapter_luid, before.adapter_luid) ||
        after->desktop_rect.left != before.desktop_rect.left ||
        after->desktop_rect.top != before.desktop_rect.top ||
        after->desktop_rect.right != before.desktop_rect.right ||
        after->desktop_rect.bottom != before.desktop_rect.bottom ||
        std::abs(after->refresh_hz -
                 static_cast<double>(*options.requested_refresh)) >= 0.5) {
      std::wcerr << L"FAIL CLOSED: post-change target identity, geometry, or "
                    L"refresh verification failed.\n";
      return 7;
    }
    std::wcout << L"refresh_after_hz=" << std::fixed << std::setprecision(3)
               << after->refresh_hz << L"\n"
               << L"refresh_change_result=PASS\n";
  }

  std::wcout << L"\n[DisplayConfig active paths: " << active_paths.size() << L"]\n";
  for (const auto& path : active_paths) {
    print_path(path);
  }

  ComPtr<IDXGIFactory6> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    std::wcerr << L"CreateDXGIFactory2 failed: " << hresult_text(hr) << L"\n";
    return 2;
  }

  unsigned adapter_count = 0;
  unsigned output_count = 0;
  unsigned selected_output_match_count = 0;
  unsigned selected_duplication_success_count = 0;

  std::wcout << L"\n[DXGI adapters and outputs]\n";
  for (UINT adapter_index = 0;; ++adapter_index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    ++adapter_count;

    DXGI_ADAPTER_DESC1 adapter_desc{};
    hr = adapter->GetDesc1(&adapter_desc);
    if (FAILED(hr)) {
      continue;
    }

    ComPtr<ID3D12Device> d3d12_device;
    const HRESULT d3d12_result = D3D12CreateDevice(
        adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device));

    std::wcout << L"adapter[" << adapter_index << L"] description=\""
               << adapter_desc.Description << L"\" luid="
               << luid_text(adapter_desc.AdapterLuid) << L" vendor=0x"
               << std::hex << std::uppercase << adapter_desc.VendorId
               << L" device=0x" << adapter_desc.DeviceId << std::dec
               << L" software="
               << ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L"true" : L"false")
               << L" d3d12=" << hresult_text(d3d12_result) << L"\n";

    for (UINT output_index = 0;; ++output_index) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      ++output_count;

      DXGI_OUTPUT_DESC desc{};
      if (FAILED(output->GetDesc(&desc))) {
        continue;
      }

      const ActivePath* matching_path = nullptr;
      unsigned source_name_match_count = 0;
      for (const auto& path : active_paths) {
        if (path.source_name == desc.DeviceName) {
          matching_path = &path;
          ++source_name_match_count;
        }
      }

      if (source_name_match_count != 1) {
        matching_path = nullptr;
      }

      const bool is_selected =
          matching_path != nullptr &&
          ((options.monitor_match.empty() && !matching_path->primary) ||
           (!options.monitor_match.empty() &&
            path_matches(*matching_path, options.monitor_match)));
      const bool adapter_luid_matches =
          matching_path != nullptr &&
          same_luid(matching_path->adapter_luid, adapter_desc.AdapterLuid);
      if (is_selected) {
        ++selected_output_match_count;
      }

      const DuplicationProbe probe = probe_duplication(adapter.Get(), output.Get());
      if (is_selected && SUCCEEDED(probe.duplication_result)) {
        ++selected_duplication_success_count;
      }

      std::wcout << L"  output[" << output_index << L"] device="
                 << desc.DeviceName << L" attached="
                 << (desc.AttachedToDesktop ? L"true" : L"false")
                 << L" rect=[" << desc.DesktopCoordinates.left << L","
                 << desc.DesktopCoordinates.top << L","
                 << desc.DesktopCoordinates.right << L","
                 << desc.DesktopCoordinates.bottom << L"]"
                 << L" displayconfig_match="
                 << (matching_path != nullptr ? L"true" : L"false")
                 << L" adapter_luid_match="
                 << (adapter_luid_matches ? L"true" : L"false")
                 << L" selected_target=" << (is_selected ? L"true" : L"false")
                 << L"\n"
                 << L"    d3d11_device=" << hresult_text(probe.device_result);
      if (probe.d3d11_device_created) {
        std::wcout << L" feature_level=" << feature_level_text(probe.feature_level);
      }
      std::wcout << L" duplication_method=" << probe.method
                 << L" duplication=" << hresult_text(probe.duplication_result)
                 << L"\n";
    }
  }

  std::wcout << L"\n[Summary]\n"
             << L"adapter_count=" << adapter_count << L"\n"
             << L"output_count=" << output_count << L"\n"
             << L"selected_target_matches=" << selected_paths.size() << L"\n"
             << L"selected_output_matches=" << selected_output_match_count
             << L"\n"
             << L"selected_duplication_successes="
             << selected_duplication_success_count << L"\n";

  if (!options.monitor_match.empty() && selected_paths.size() != 1) {
    std::wcerr << L"FAIL CLOSED: monitor selector matched "
               << selected_paths.size() << L" active targets.\n";
    return 3;
  }

  std::wcout << L"desktop_duplication_supported="
             << (selected_duplication_success_count == 1 ? L"true" : L"false")
             << L"\n";
  std::wcout << L"inventory_result=PASS\n";
  return 0;
}
