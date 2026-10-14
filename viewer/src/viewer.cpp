#include <windows.h>
#include "window_picker.h"
#include "dialog_follower.h"
#include "hotkeys.h"
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

constexpr UINT kFrameMessage = WM_APP + 1;
constexpr UINT kRestartCaptureMessage = WM_APP + 3;
constexpr UINT kPresentReadyMessage = WM_APP + 4;
constexpr UINT_PTR kTelemetryTimer = 1;
constexpr UINT_PTR kChromeTimer = 4;
constexpr int kReturnHotkey = 60;
constexpr int kPickerHotkey = 61;
constexpr UINT kPickWindow = 1006;
constexpr UINT kReturnCursor = 1004;
constexpr UINT kRescueWindows = 1005;
constexpr UINT kSize100 = 1001;
constexpr UINT kSizeFitDefault = 1002;
constexpr UINT kSize50 = 1003;
constexpr DWORD kViewerStyle = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
std::wstring lower_copy(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t value_character) {
                   return static_cast<wchar_t>(std::towlower(value_character));
                 });
  return value;
}

bool contains_case_insensitive(const std::wstring& value,
                               const std::wstring& needle) {
  return lower_copy(value).find(lower_copy(needle)) != std::wstring::npos;
}

std::wstring luid_text(const LUID& luid) {
  wchar_t buffer[40]{};
  swprintf_s(buffer, L"0x%08X:%08X",
             static_cast<std::uint32_t>(luid.HighPart), luid.LowPart);
  return buffer;
}

std::string utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                        static_cast<int>(value.size()), nullptr,
                                        0, nullptr, nullptr);
  if (bytes <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                      static_cast<int>(value.size()), result.data(), bytes,
                      nullptr, nullptr);
  return result;
}

std::wstring now_text() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t buffer[64]{};
  swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u",
             time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
             time.wSecond, time.wMilliseconds);
  return buffer;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      std::clamp(fraction, 0.0, 1.0) *
      static_cast<double>(values.size() - 1));
  return values[index];
}

std::filesystem::path default_log_path() {
  PWSTR local_app_data = nullptr;
  winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                          &local_app_data));
  const std::filesystem::path directory(local_app_data);
  CoTaskMemFree(local_app_data);
  std::wstring timestamp = now_text();
  std::replace(timestamp.begin(), timestamp.end(), L':', L'-');
  return directory / L"SharedVD" / L"Logs" /
         (L"viewer-" + timestamp + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log");
}

struct Options {
  std::filesystem::path log_path;
  std::uint32_t auto_exit_ms = 0;
  bool start_fullscreen = false;
  std::wstring monitor_match = L"VDD by MTT";
  std::wstring adapter_match;
  double required_refresh_hz = 60.0;
  bool rescue_only = false;
  bool list_windows = false;
  bool keep_windows = false;
  DWORD rescue_process_id = 0; // Optional scope for isolated integration tests.
  bool shortcuts_only = false;
  std::filesystem::path hotkey_config = sharedvd::hotkey_config_path();
};

Options parse_options() {
  Options options;
  options.log_path = default_log_path();
  int argument_count = 0;
  auto* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  if (arguments == nullptr) {
    return options;
  }
  for (int index = 1; index < argument_count; ++index) {
    const std::wstring argument = arguments[index];
    if (argument == L"--shortcuts") {
      options.shortcuts_only = true;
    } else if (argument == L"--hotkey-config" && index + 1 < argument_count) {
      options.hotkey_config = arguments[++index];
    } else if (argument == L"--log" && index + 1 < argument_count) {
      options.log_path = arguments[++index];
    } else if (argument == L"--auto-exit-ms" &&
               index + 1 < argument_count) {
      options.auto_exit_ms = static_cast<std::uint32_t>(
          std::wcstoul(arguments[++index], nullptr, 10));
    } else if (argument == L"--fullscreen") {
      options.start_fullscreen = true;
    } else if (argument == L"--monitor-match" &&
               index + 1 < argument_count) {
      options.monitor_match = arguments[++index];
    } else if (argument == L"--adapter-match" &&
               index + 1 < argument_count) {
      options.adapter_match = arguments[++index];
    } else if (argument == L"--require-refresh-hz" &&
               index + 1 < argument_count) {
      options.required_refresh_hz = std::wcstod(arguments[++index], nullptr);
    } else if (argument == L"--rescue-windows") {
      options.rescue_only = true;
    } else if (argument == L"--list-windows") {
      options.list_windows = true;
    } else if (argument == L"--keep-windows") {
      options.keep_windows = true;
    } else if (argument == L"--rescue-process-id" && index + 1 < argument_count) {
      options.rescue_process_id = std::wcstoul(arguments[++index], nullptr, 10);
      if (!options.rescue_process_id) throw std::invalid_argument("Invalid rescue process id");
    }
  }
  LocalFree(arguments);
  return options;
}

class Logger {
 public:
  explicit Logger(std::filesystem::path path) : path_(std::move(path)) {
    if (!path_.empty()) {
      std::error_code error;
      if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), error);
      }
    }
  }

  void write(const std::wstring& message) {
    const std::wstring line = now_text() + L" " + message;
    OutputDebugStringW((line + L"\n").c_str());
    std::lock_guard lock(mutex_);
    if (path_.empty()) {
      return;
    }
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (output) {
      output << utf8(line) << "\n";
    }
  }

 private:
  std::filesystem::path path_;
  std::mutex mutex_;
};

struct TargetMonitor {
  std::wstring source_name;
  std::wstring target_name;
  std::wstring target_path;
  LUID source_luid{};
  HMONITOR monitor = nullptr;
  RECT bounds{};
  double refresh_hz = 0.0;
  bool primary = false;
};

bool has_required_refresh(const TargetMonitor& target,
                          double required_refresh_hz) {
  return required_refresh_hz <= 0.0 ||
         std::abs(target.refresh_hz - required_refresh_hz) < 0.6;
}

bool target_matches(const TargetMonitor& target,
                    const std::wstring& monitor_match) {
  if (monitor_match.empty()) {
    return false;
  }
  return contains_case_insensitive(target.source_name, monitor_match) ||
         contains_case_insensitive(target.target_name, monitor_match) ||
         contains_case_insensitive(target.target_path, monitor_match);
}

std::optional<TargetMonitor> find_target_monitor(
    Logger& logger, const std::wstring& monitor_match) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                  &mode_count) != ERROR_SUCCESS) {
    logger.write(L"target_error=GetDisplayConfigBufferSizes");
    return std::nullopt;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                         &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
    logger.write(L"target_error=QueryDisplayConfig");
    return std::nullopt;
  }
  paths.resize(path_count);
  modes.resize(mode_count);

  std::vector<TargetMonitor> candidates;
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

    TargetMonitor value;
    value.source_name = source.viewGdiDeviceName;
    value.target_name = target.monitorFriendlyDeviceName;
    value.target_path = target.monitorDevicePath;
    value.source_luid = path.sourceInfo.adapterId;
    if (path.targetInfo.refreshRate.Denominator != 0) {
      value.refresh_hz =
          static_cast<double>(path.targetInfo.refreshRate.Numerator) /
          static_cast<double>(path.targetInfo.refreshRate.Denominator);
    }
    if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
        path.sourceInfo.modeInfoIdx < modes.size()) {
      const auto& mode = modes[path.sourceInfo.modeInfoIdx];
      if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const RECT desktop_rect{
            mode.sourceMode.position.x,
            mode.sourceMode.position.y,
            mode.sourceMode.position.x +
                static_cast<LONG>(mode.sourceMode.width),
            mode.sourceMode.position.y +
                static_cast<LONG>(mode.sourceMode.height)};
        const POINT origin{0, 0};
        value.primary = PtInRect(&desktop_rect, origin) != FALSE;
      }
    }
    candidates.push_back(std::move(value));
  }

  std::vector<TargetMonitor> matching;
  if (!monitor_match.empty()) {
    for (const auto& candidate : candidates) {
      if (target_matches(candidate, monitor_match)) {
        matching.push_back(candidate);
      }
    }
  } else {
    for (const auto& candidate : candidates) {
      if (!candidate.primary) {
        matching.push_back(candidate);
      }
    }
  }

  if (matching.size() != 1) {
    logger.write(L"target_error=expected_one_target observed=" +
                 std::to_wstring(matching.size()) + L" monitor_match=" +
                 monitor_match);
    return std::nullopt;
  }

  std::optional<TargetMonitor> target_result = std::move(matching.front());

  struct MonitorSearch {
    std::wstring source_name;
    HMONITOR monitor = nullptr;
    RECT bounds{};
    unsigned matches = 0;
  } search{target_result->source_name};

  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<MonitorSearch*>(parameter);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) != FALSE &&
            search->source_name == info.szDevice) {
          search->monitor = monitor;
          search->bounds = info.rcMonitor;
          ++search->matches;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));

  if (search.matches != 1 || search.monitor == nullptr) {
    logger.write(L"target_error=hmonitor_match_count_" +
                 std::to_wstring(search.matches));
    return std::nullopt;
  }

  target_result->monitor = search.monitor;
  target_result->bounds = search.bounds;
  return target_result;
}

struct RescueCandidate {
  HWND window{};
  DWORD process{};
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  RECT destination{};
  bool tool_window = false;
};

struct RescueResult {
  std::size_t found = 0;
  std::size_t failed = 0;
};

RescueResult rescue_windows(const TargetMonitor& target, Logger& logger,
                            bool dry_run = false, DWORD only_process = 0) {
  const HMONITOR primary_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO primary{sizeof(MONITORINFO)};
  if (!GetMonitorInfoW(primary_monitor, &primary)) return {0, 1};
  HMONITOR source_monitor = MonitorFromRect(&target.bounds, MONITOR_DEFAULTTONULL);
  MONITORINFO source{sizeof(MONITORINFO)};
  source.rcMonitor = source.rcWork = target.bounds;
  if (source_monitor &&
      (!GetMonitorInfoW(source_monitor, &source) || !EqualRect(&source.rcMonitor, &target.bounds))) {
    source_monitor = nullptr;
    source.rcMonitor = source.rcWork = target.bounds;
  }
  // Never treat the main display as an evacuation source.
  if (source_monitor == primary_monitor) return {};
  struct Context {
    HMONITOR source_monitor;
    MONITORINFO source;
    MONITORINFO primary;
    DWORD only_process;
    std::vector<RescueCandidate> candidates;
    std::size_t failures = 0;
  } context{source_monitor, source, primary, only_process, {}, 0};
  const BOOL enumerated = EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto& ctx = *reinterpret_cast<Context*>(data);
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() || (ctx.only_process && ctx.only_process != process) ||
        !IsWindowVisible(window) || window == GetShellWindow() || window == GetDesktopWindow()) return TRUE;
    wchar_t class_name[128]{};
    GetClassNameW(window, class_name, 128);
    for (const auto* shell_class : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
      if (std::wcscmp(class_name, shell_class) == 0) return TRUE;
    }
    DWORD cloaked{};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    RescueCandidate candidate;
    candidate.window = window;
    candidate.process = process;
    if (!GetWindowPlacement(window, &candidate.placement)) {
      if (ctx.source_monitor && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == ctx.source_monitor) ++ctx.failures;
      return TRUE;
    }
    candidate.tool_window = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    RECT normal = candidate.placement.rcNormalPosition;
    if (!candidate.tool_window) OffsetRect(&normal,
        ctx.source.rcWork.left - ctx.source.rcMonitor.left,
        ctx.source.rcWork.top - ctx.source.rcMonitor.top);
    if (ctx.source_monitor) {
      if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != ctx.source_monitor) return TRUE;
    } else {
      RECT actual = normal;
      if (!IsIconic(window)) GetWindowRect(window, &actual);
      RECT overlap{};
      if (MonitorFromRect(&actual, MONITOR_DEFAULTTONULL) ||
          !IntersectRect(&overlap, &actual, &ctx.source.rcMonitor)) return TRUE;
    }
    const RECT work = ctx.primary.rcWork;
    const LONG width = std::clamp(normal.right - normal.left, 1L, work.right - work.left);
    const LONG height = std::clamp(normal.bottom - normal.top, 1L, work.bottom - work.top);
    const LONG x = std::clamp(work.left + normal.left - ctx.source.rcWork.left, work.left, work.right - width);
    const LONG y = std::clamp(work.top + normal.top - ctx.source.rcWork.top, work.top, work.bottom - height);
    candidate.destination = RECT{x, y, x + width, y + height};
    ctx.candidates.push_back(candidate);
    return TRUE;
  }, reinterpret_cast<LPARAM>(&context));
  if (!enumerated) {
    logger.write(L"rescue_error=EnumWindows");
    return {0, 1};
  }
  // Move owners before their dialogs; all positions were snapshotted above.
  std::stable_sort(context.candidates.begin(), context.candidates.end(),
      [](const auto& a, const auto& b) {
        return (GetWindow(a.window, GW_OWNER) != nullptr) < (GetWindow(b.window, GW_OWNER) != nullptr);
      });
  logger.write(L"rescue_begin count=" + std::to_wstring(context.candidates.size()) +
               L" dry_run=" + (dry_run ? L"true" : L"false"));
  std::vector<HWND> pending;
  std::size_t failed = context.failures;
  for (auto& candidate : context.candidates) {
    logger.write(L"rescue_candidate hwnd=" + std::to_wstring(reinterpret_cast<UINT_PTR>(candidate.window)) +
        L" pid=" + std::to_wstring(candidate.process) + L" show=" + std::to_wstring(candidate.placement.showCmd));
    if (dry_run) continue;
    DWORD current_process{};
    GetWindowThreadProcessId(candidate.window, &current_process);
    if (current_process != candidate.process) continue;
    auto placement = candidate.placement;
    placement.flags = WPF_ASYNCWINDOWPLACEMENT;
    placement.showCmd = SW_SHOWNOACTIVATE;
    placement.ptMinPosition = placement.ptMaxPosition = POINT{-1, -1};
    placement.rcNormalPosition = candidate.destination;
    if (!candidate.tool_window) OffsetRect(&placement.rcNormalPosition,
        primary.rcMonitor.left - primary.rcWork.left, primary.rcMonitor.top - primary.rcWork.top);
    if (!SetWindowPlacement(candidate.window, &placement)) {
      ++failed;
      logger.write(L"rescue_error=SetWindowPlacement code=" + std::to_wstring(GetLastError()));
    } else pending.push_back(candidate.window);
  }
  // Async placement avoids waiting on an unresponsive foreign UI thread.
  // Bounded verification catches rejected moves and applications that move themselves back.
  for (int attempt = 0; attempt < 20 && !pending.empty(); ++attempt) {
    Sleep(25);
    std::erase_if(pending, [&](HWND window) {
      if (!IsWindow(window)) return true;
      RECT actual{};
      if (!GetWindowRect(window, &actual) || IsIconic(window) || IsZoomed(window)) return false;
      const POINT title_point{actual.left + (actual.right - actual.left) / 2,
                              actual.top + std::min(20L, (actual.bottom - actual.top) / 2)};
      return MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == primary_monitor &&
             PtInRect(&primary.rcWork, title_point);
    });
  }
  failed += pending.size();
  logger.write(L"rescue_end found=" + std::to_wstring(context.candidates.size()) +
               L" failed=" + std::to_wstring(failed));
  return {context.candidates.size(), failed};
}

struct D3DState {
  ComPtr<IDXGIAdapter1> adapter;
  DXGI_ADAPTER_DESC1 adapter_desc{};
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  direct3d::IDirect3DDevice winrt_device{nullptr};
  ComPtr<IDXGIFactory2> factory;
};

std::optional<D3DState> create_target_d3d_device(
    const LUID& target_luid, const std::wstring& adapter_match,
    Logger& logger) {
  ComPtr<IDXGIFactory6> enumeration_factory;
  winrt::check_hresult(
      CreateDXGIFactory2(0, IID_PPV_ARGS(&enumeration_factory)));

  D3DState state;
  const HRESULT adapter_result = enumeration_factory->EnumAdapterByLuid(
      target_luid, IID_PPV_ARGS(&state.adapter));
  if (FAILED(adapter_result)) {
    logger.write(L"d3d_error=EnumAdapterByLuid_" +
                 std::to_wstring(static_cast<std::uint32_t>(adapter_result)));
    return std::nullopt;
  }
  winrt::check_hresult(state.adapter->GetDesc1(&state.adapter_desc));
  if (!adapter_match.empty() &&
      !contains_case_insensitive(state.adapter_desc.Description,
                                 adapter_match)) {
    logger.write(L"d3d_error=target_source_adapter_did_not_match description=" +
                 std::wstring(state.adapter_desc.Description) +
                 L" required_match=" + adapter_match);
    return std::nullopt;
  }

  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL selected{};
  const HRESULT device_result = D3D11CreateDevice(
      state.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
      static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &state.device,
      &selected, &state.context);
  if (FAILED(device_result)) {
    logger.write(L"d3d_error=D3D11CreateDevice_" +
                 std::to_wstring(static_cast<std::uint32_t>(device_result)));
    return std::nullopt;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  winrt::check_hresult(state.device.As(&dxgi_device));
  winrt::com_ptr<IInspectable> inspectable;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
      dxgi_device.Get(), inspectable.put()));
  state.winrt_device = inspectable.as<direct3d::IDirect3DDevice>();
  winrt::check_hresult(state.adapter->GetParent(IID_PPV_ARGS(&state.factory)));
  return state;
}

struct ContentRect {
  float left = 0.0F;
  float top = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

class ViewerApp {
 public:
  explicit ViewerApp(Options options)
      : options_(std::move(options)), logger_(options_.log_path) {}

  ~ViewerApp() { shutdown(); }

  bool initialize(HINSTANCE instance) {
    instance_ = instance;

    target_ = find_target_monitor(logger_, options_.monitor_match);
    if (!target_.has_value()) {
      return false;
    }
    if (!has_required_refresh(*target_, options_.required_refresh_hz)) {
      logger_.write(L"refresh_error=required_hz_" +
                    std::to_wstring(options_.required_refresh_hz) +
                    L" current_hz=" + std::to_wstring(target_->refresh_hz));
      return false;
    }
    d3d_ = create_target_d3d_device(target_->source_luid,
                                    options_.adapter_match, logger_);
    if (!d3d_.has_value()) {
      return false;
    }

    logger_.write(L"target_source=" + target_->source_name +
                  L" target_path=" + target_->target_path +
                  L" target_luid=" + luid_text(target_->source_luid) +
                  L" target_refresh_hz=" +
                  std::to_wstring(target_->refresh_hz));
    logger_.write(L"viewer_adapter=" +
                  std::wstring(d3d_->adapter_desc.Description) +
                  L" viewer_luid=" + luid_text(d3d_->adapter_desc.AdapterLuid));
    logger_.write(L"adapter_requirement=PASS exact_target_luid=true "
                  L"required_match=" + options_.adapter_match);
    logger_.write(L"refresh_requirement=PASS required_hz=" +
                  std::to_wstring(options_.required_refresh_hz) +
                  L" current_hz=" + std::to_wstring(target_->refresh_hz));

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &ViewerApp::window_proc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
#ifdef SHAREDVD_ICON_AVAILABLE
    window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(instance_,
        MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_SHARED));
#else
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
#endif
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"LGWLViewerWindow";
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      logger_.write(L"window_error=RegisterClassEx");
      return false;
    }

    const HMONITOR physical_monitor = MonitorFromPoint(POINT{0, 0},
                                                       MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO physical_info{};
    physical_info.cbSize = sizeof(physical_info);
    GetMonitorInfoW(physical_monitor, &physical_info);

    RECT desired{0, 0, 1280, 720};
    const int window_width = desired.right - desired.left;
    const int window_height = desired.bottom - desired.top;
    const int window_left = physical_info.rcWork.left +
                            ((physical_info.rcWork.right -
                              physical_info.rcWork.left) -
                             window_width) /
                                2;
    const int window_top = physical_info.rcWork.top +
                           ((physical_info.rcWork.bottom -
                             physical_info.rcWork.top) -
                            window_height) /
                               2;

    hwnd_ = CreateWindowExW(
        0, window_class.lpszClassName, L"VKomado",
        kViewerStyle, window_left, window_top, window_width,
        window_height, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
      logger_.write(L"window_error=CreateWindowEx");
      return false;
    }

    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    if (!create_overlays()) return false;
    hotkeys_.initialize(hwnd_, options_.hotkey_config, [this] {
      return_hotkey_registered_ = hotkeys_.registered(0);
      picker_hotkey_registered_ = hotkeys_.registered(1);
      dialog_follower_.reset();
      if (target_) dialog_follower_.tick(target_->monitor, hotkeys_.follow_dialogs(), options_.rescue_process_id);
      update_title(0.0);
      if (chrome_) InvalidateRect(chrome_, nullptr, FALSE);
      logger_.write(L"hotkeys return=" + hotkeys_.label(0) + L" picker=" + hotkeys_.label(1) +
          L" registered=" + std::to_wstring(return_hotkey_registered_) + L"," + std::to_wstring(picker_hotkey_registered_));
    });
    set_view_size(2.0 / 3.0);
    // Exclude the viewer itself, including transient shell moves.
    const BOOL excluded = SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    logger_.write(L"viewer_capture_excluded=" + std::wstring(excluded ? L"true" : L"false"));

    if (!create_renderer()) {
      return false;
    }
    if (!start_capture()) {
      return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTelemetryTimer, 1000, nullptr);
    SetTimer(hwnd_, kChromeTimer, 80, nullptr);
    start_time_ = std::chrono::steady_clock::now();
    previous_telemetry_time_ = start_time_;
    logger_.write(L"viewer_ready=true mode=cursor_jump input_forwarding=false cursor_confinement=false");

    if (options_.start_fullscreen) {
      toggle_fullscreen();
    }
    return true;
  }

  int run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      HWND editor = FindWindowW(L"SharedVDHotkeyEditor", nullptr);
      DWORD editor_process{}; if (editor) GetWindowThreadProcessId(editor, &editor_process);
      if (editor_process == GetCurrentProcessId() && IsDialogMessageW(editor, &message)) continue;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
  }

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM w_param, LPARAM l_param) {
    ViewerApp* app = nullptr;
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
      app = static_cast<ViewerApp*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(app));
      app->hwnd_ = window;
    } else {
      app = reinterpret_cast<ViewerApp*>(
          GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (app != nullptr) {
      return app->handle_message(message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  LRESULT handle_message(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
      case WM_NCCALCSIZE:
        return 0; // The entire window is the image; chrome is a hover overlay.
      case WM_NCHITTEST:
        return border_hit_test(l_param);
      case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
        limits->ptMinTrackSize = POINT{dpi_pixels(400), dpi_pixels(225)};
        MONITORINFO info{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &info)) {
          limits->ptMaxPosition = POINT{info.rcWork.left - info.rcMonitor.left,
                                       info.rcWork.top - info.rcMonitor.top};
          limits->ptMaxSize = POINT{info.rcWork.right - info.rcWork.left,
                                   info.rcWork.bottom - info.rcWork.top};
        }
        return 0;
      }
      case WM_WINDOWPOSCHANGED: {
        const auto begin = std::chrono::steady_clock::now();
        position_chrome();
        chrome_position_samples_ms_.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count());
        break;
      }
      case WM_ENTERSIZEMOVE:
        moving_window_ = true;
        logger_.write(L"native_drag=begin");
        break;
      case WM_EXITSIZEMOVE:
        moving_window_ = false;
        logger_.write(L"native_drag=end");
        break;
      case WM_LBUTTONDOWN:
        click_pending_ = true;
        return 0;
      case WM_LBUTTONUP:
        if (click_pending_) {
          click_pending_ = false;
          jump_to_target(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
        }
        return 0;
      case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&track);
        update_chrome_visibility();
        return 0;
      }
      case WM_MOUSELEAVE:
      case WM_CANCELMODE:
      case WM_KILLFOCUS:
        click_pending_ = false;
        break;
      case WM_HOTKEY:
        if (w_param == kReturnHotkey && return_hotkey_registered_) { picker_.close(); return_to_primary(); }
        if (w_param == kPickerHotkey && picker_hotkey_registered_) toggle_picker();
        if (w_param == 62 && hotkeys_.registered(2) && target_)
          picker_.move_active(hwnd_, target_->monitor, options_.rescue_process_id,
            [this](const std::wstring& message) { logger_.write(message); });
        return 0;
      case WM_WINDOWPOSCHANGING:
        if (IsZoomed(hwnd_) && !fullscreen_ && !IsIconic(hwnd_)) {
          auto* position = reinterpret_cast<WINDOWPOS*>(l_param);
          MONITORINFO host{sizeof(MONITORINFO)};
          if (!(position->flags & SWP_NOSIZE) &&
              GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &host)) {
            position->x = host.rcWork.left;
            position->y = host.rcWork.top;
            position->cx = host.rcWork.right - host.rcWork.left;
            position->cy = host.rcWork.bottom - host.rcWork.top;
            position->flags &= ~SWP_NOMOVE;
          }
        }
        keep_viewer_off_target(reinterpret_cast<WINDOWPOS*>(l_param));
        return 0; // Do not let DefWindowProc reapply the standard nonclient frame limits.
      case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<const RECT*>(l_param);
        SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                     rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(w_param)) {
          case kSize100: set_view_size(1.0); return 0;
          case kSizeFitDefault: set_view_size(2.0 / 3.0); return 0;
          case kSize50: set_view_size(0.5); return 0;
          case kReturnCursor: return_to_primary(); return 0;
          case kRescueWindows: evacuate_windows(); return 0;
          case kPickWindow: toggle_picker(); return 0;
          case sharedvd::kShortcutSettingsCommand: picker_.close(); hotkeys_.show_editor(); return 0;
        }
        break;
      case WM_ERASEBKGND:
        return 1;
      case WM_SIZE:
        if (swap_chain_ && w_param != SIZE_MINIMIZED) {
          resize_swap_chain(LOWORD(l_param), HIWORD(l_param));
        }
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        EndPaint(hwnd_, &paint);
        render_latest_frame();
        return 0;
      }
      case WM_KEYDOWN:
        if (w_param == VK_F11) {
          toggle_fullscreen();
          return 0;
        }
        if (w_param == VK_ESCAPE && fullscreen_) {
          toggle_fullscreen();
          return 0;
        }
        break;
      case WM_DISPLAYCHANGE:
        picker_.close();
        dialog_follower_.reset();
        PostMessageW(hwnd_, kRestartCaptureMessage, 0, 0);
        return 0;
      case kFrameMessage:
        frame_message_pending_.store(false);
        render_latest_frame();
        return 0;
      case sharedvd::kReloadShortcutsMessage:
        hotkeys_.reload(); return 0;
      case kPresentReadyMessage:
        render_wait_armed_ = false;
        render_slot_available_ = true;
        frame_wait_samples_ms_.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - render_wait_started_).count());
        if (render_requested_) render_latest_frame();
        return 0;
      case kRestartCaptureMessage:
        restart_capture();
        return 0;
      case WM_TIMER:
        if (w_param == kChromeTimer) {
          const auto now = std::chrono::steady_clock::now();
          if (previous_ui_tick_ != std::chrono::steady_clock::time_point{}) {
            ui_tick_samples_ms_.push_back(std::chrono::duration<double, std::milli>(
                now - previous_ui_tick_).count());
          }
          previous_ui_tick_ = now;
          update_chrome_visibility();
          if (target_) dialog_follower_.tick(target_->monitor,
              hotkeys_.follow_dialogs() && !picker_.open() && !moving_window_ && !FindWindowW(L"SharedVDHotkeyEditor", nullptr),
              options_.rescue_process_id);
        }
        if (w_param == kTelemetryTimer) {
          update_telemetry();
        }
        return 0;
      case WM_CLOSE:
        if (options_.keep_windows || evacuate_windows()) DestroyWindow(hwnd_);
        return 0;
      case WM_DESTROY:
        dialog_follower_.reset();
        picker_.close();
        hotkeys_.shutdown();
        picker_hotkey_registered_ = false;
        stop_render_wait();
        return_hotkey_registered_ = false;
        if (chrome_) DestroyWindow(chrome_);
        if (ring_) DestroyWindow(ring_);
        chrome_ = ring_ = nullptr;
        stop_capture();
        KillTimer(hwnd_, kTelemetryTimer);
        KillTimer(hwnd_, kChromeTimer);
        PostQuitMessage(0);
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd_, message, w_param, l_param);
  }

  void toggle_picker() {
    if (!target_) return;
    picker_.toggle(hwnd_, target_->monitor, options_.rescue_process_id,
        [this](const std::wstring& text) { logger_.write(text); });
  }

  bool evacuate_windows() {
    if (!target_) return true;
    const auto result = rescue_windows(*target_, logger_, false, options_.rescue_process_id);
    if (result.failed) {
      MessageBoxW(hwnd_,
          L"一部のウィンドウをメイン画面へ戻せませんでした。ビューアの終了を中止します。\n"
          L"応答停止中・管理者権限のアプリなどを手動で戻してから、もう一度終了してください。\n"
          L"詳細はログに記録しました。",
          L"VKomado — ウィンドウの退避", MB_OK | MB_ICONWARNING);
      return false;
    }
    return true;
  }

  int dpi_pixels(int value) const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return MulDiv(value, dpi ? dpi : 96, 96);
  }

  LRESULT border_hit_test(LPARAM coordinates) const {
    if (fullscreen_ || IsZoomed(hwnd_)) return HTCLIENT;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const int x = GET_X_LPARAM(coordinates), y = GET_Y_LPARAM(coordinates);
    const int edge = dpi_pixels(6);
    const bool left = x < window.left + edge, right = x >= window.right - edge;
    const bool top = y < window.top + edge, bottom = y >= window.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCLIENT;
  }

  void jump_to_target(int x, int y) {
    if (!target_ || y < dpi_pixels(40)) return; // Reserved for hover chrome.
    int width{}, height{};
    {
      std::lock_guard lock(frame_mutex_);
      width = latest_width_;
      height = latest_height_;
    }
    const auto content = calculate_content_rect(width, height);
    if (content.width <= 0 || content.height <= 0 ||
        x < content.left || y < content.top ||
        x >= content.left + content.width || y >= content.top + content.height) return;
    const RECT bounds = target_->bounds;
    const int target_x = bounds.left + std::clamp(
        static_cast<int>((x - content.left) * (bounds.right - bounds.left) / content.width),
        0, static_cast<int>(bounds.right - bounds.left - 1));
    const int target_y = bounds.top + std::clamp(
        static_cast<int>((y - content.top) * (bounds.bottom - bounds.top) / content.height),
        0, static_cast<int>(bounds.bottom - bounds.top - 1));
    const BOOL moved = SetCursorPos(target_x, target_y);
    logger_.write(std::wstring(L"cursor_jump=") + (moved ? L"true" : L"false") +
                  L" x=" + std::to_wstring(target_x) + L" y=" + std::to_wstring(target_y));
    update_chrome_visibility();
  }

  void return_to_primary() {
    MONITORINFO primary{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary)) return;
    const POINT center{(primary.rcMonitor.left + primary.rcMonitor.right) / 2,
                       (primary.rcMonitor.top + primary.rcMonitor.bottom) / 2};
    const BOOL moved = SetCursorPos(center.x, center.y);
    logger_.write(std::wstring(L"cursor_return=") + (moved ? L"true" : L"false"));
    if (!moved || !ring_) return;
    const int size = dpi_pixels(144);
    ring_started_ = GetTickCount64();
    SetWindowPos(ring_, HWND_TOPMOST, center.x - size / 2, center.y - size / 2,
                 size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(ring_, nullptr, FALSE);
    SetTimer(ring_, 1, 16, nullptr);
  }

  static LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<ViewerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      app = static_cast<ViewerApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (window == app->ring_) {
      if (message == WM_NCHITTEST) return HTTRANSPARENT;
      if (message == WM_TIMER) {
        if (GetTickCount64() - app->ring_started_ >= 700) {
          KillTimer(window, 1);
          ShowWindow(window, SW_HIDE);
        } else InvalidateRect(window, nullptr, FALSE);
        return 0;
      }
      if (message == WM_PAINT) { app->paint_ring(); return 0; }
      return DefWindowProcW(window, message, wp, lp);
    }
    if (window != app->chrome_) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
      case WM_NCHITTEST:
        return app->border_hit_test(lp) != HTCLIENT ? HTTRANSPARENT : HTCLIENT;
      case WM_PAINT: app->paint_chrome(); return 0;
      case WM_MOUSEMOVE: {
        const int hit = app->chrome_button(GET_X_LPARAM(lp));
        if (hit != app->chrome_hover_) {
          app->chrome_hover_ = hit;
          InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
      }
      case WM_MOUSELEAVE:
        app->chrome_pressed_ = app->chrome_hover_ = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
      case WM_LBUTTONDOWN:
        app->chrome_pressed_ = app->chrome_button(GET_X_LPARAM(lp));
        if (app->chrome_pressed_ < 0 && !app->fullscreen_) {
          // The hover overlay deliberately does not activate on mouse-over or
          // click. A native caption drag, however, must start with its owner
          // in the foreground; otherwise input stays with the previously used
          // app and live movement can stall until the mouse button is released.
          const BOOL activated = SetForegroundWindow(app->hwnd_);
          app->logger_.write(std::wstring(L"caption_drag_foreground=") +
                            (activated ? L"true" : L"false"));
          SendMessageW(app->hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, GetMessagePos());
        }
        return 0;
      case WM_LBUTTONUP: {
        const int button = app->chrome_button(GET_X_LPARAM(lp));
        if (button < 0 || button != app->chrome_pressed_) return 0;
        app->chrome_pressed_ = -1;
        if (button == 0) app->show_size_menu();
        if (button == 1) ShowWindow(app->hwnd_, SW_MINIMIZE);
        if (button == 2) app->toggle_maximized();
        if (button == 3) PostMessageW(app->hwnd_, WM_CLOSE, 0, 0);
        return 0;
      }
      case WM_LBUTTONDBLCLK:
        if (app->chrome_button(GET_X_LPARAM(lp)) < 0) app->toggle_maximized();
        return 0;
      case WM_RBUTTONUP: app->show_size_menu(); return 0;
      default: return DefWindowProcW(window, message, wp, lp);
    }
  }

  bool create_overlays() {
    WNDCLASSEXW cls{sizeof(WNDCLASSEXW)};
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = &ViewerApp::overlay_proc;
    cls.hInstance = instance_;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"SharedVDHoverChrome";
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    chrome_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        L"VKomado — 表示サイズ / 最小化 / 最大化 / 閉じる", WS_POPUP,
        0, 0, 1, 1, hwnd_, nullptr, instance_, this);
    cls.lpszClassName = L"SharedVDCursorRing";
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    ring_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        cls.lpszClassName, L"カーソル位置", WS_POPUP, 0, 0, 1, 1, hwnd_, nullptr, instance_, this);
    if (!chrome_ || !ring_) return false;
    SetLayeredWindowAttributes(ring_, RGB(0, 0, 0), 255, LWA_COLORKEY);
    SetWindowDisplayAffinity(chrome_, WDA_EXCLUDEFROMCAPTURE);
    SetWindowDisplayAffinity(ring_, WDA_EXCLUDEFROMCAPTURE);
    position_chrome();
    return true;
  }

  void position_chrome() {
    if (!chrome_ || !hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    POINT origin{};
    ClientToScreen(hwnd_, &origin);
    SetWindowPos(chrome_, nullptr, origin.x, origin.y, client.right, dpi_pixels(40),
                 SWP_NOACTIVATE | SWP_NOZORDER);
  }

  void update_chrome_visibility() {
    if (!chrome_) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    HWND hit = WindowFromPoint(cursor);
    const bool over = hit == hwnd_ || hit == chrome_;
    const bool show = !IsIconic(hwnd_) && IsWindowVisible(hwnd_) &&
                      (over || moving_window_ || menu_open_);
    if (show != (IsWindowVisible(chrome_) != FALSE)) {
      ShowWindow(chrome_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
      if (!show) chrome_pressed_ = chrome_hover_ = -1;
    }
  }

  int chrome_button(int x) const {
    RECT rect{};
    GetClientRect(chrome_, &rect);
    const int start = rect.right - 4 * dpi_pixels(48);
    return x >= start && x < rect.right ? (x - start) / dpi_pixels(48) : -1;
  }

  void toggle_maximized() {
    if (fullscreen_) toggle_fullscreen();
    else ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
  }

  void show_size_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kSize100, L"100%（等倍）");
    AppendMenuW(menu, MF_STRING, kSizeFitDefault, L"66.7%（起動時のサイズ）");
    AppendMenuW(menu, MF_STRING, kSize50, L"50%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::wstring pick_label = L"ウィンドウを選んで移動\t" + hotkeys_.label(1) +
        (!picker_hotkey_registered_ && hotkeys_.bindings()[1].key ? L"（登録不可）" : L"");
    AppendMenuW(menu, MF_STRING, kPickWindow, pick_label.c_str());
    const std::wstring active_label = L"アクティブウィンドウの往復キー\t" + hotkeys_.label(2) +
        (!hotkeys_.registered(2) && hotkeys_.bindings()[2].key ? L"（登録不可）" : L"");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, active_label.c_str());
    AppendMenuW(menu, MF_STRING, kRescueWindows, L"仮想画面のウィンドウをメイン画面へ戻す");
    const std::wstring return_label = L"メイン画面へ戻る\t" + hotkeys_.label(0) +
        (!return_hotkey_registered_ && hotkeys_.bindings()[0].key ? L"（登録不可）" : L"");
    AppendMenuW(menu, MF_STRING, kReturnCursor, return_label.c_str());
    AppendMenuW(menu, MF_STRING, sharedvd::kShortcutSettingsCommand, L"ショートカット設定…");
    RECT rect{};
    GetWindowRect(chrome_, &rect);
    menu_open_ = true;
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN,
        rect.right - 3 * dpi_pixels(48), rect.bottom, 0, hwnd_, nullptr);
    menu_open_ = false;
    DestroyMenu(menu);
    if (command) SendMessageW(hwnd_, WM_COMMAND, command, 0);
  }

  void paint_chrome() {
    PAINTSTRUCT paint{};
    HDC destination = BeginPaint(chrome_, &paint);
    RECT rect{};
    GetClientRect(chrome_, &rect);
    HDC dc = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, rect.right, rect.bottom);
    auto old_bitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(26, 29, 35));
    FillRect(dc, &rect, background);
    DeleteObject(background);
    HFONT font = CreateFontW(-dpi_pixels(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(225, 230, 238));
    std::wostringstream text;
    text << L"VKomado  ·  ";
    if (target_) {
      const int width = target_->bounds.right - target_->bounds.left;
      const auto content = calculate_content_rect(width, target_->bounds.bottom - target_->bounds.top);
      text << std::fixed << std::setprecision(1) << 100.0 * content.width / width << L"%  ·  ";
    }
    text << (return_hotkey_registered_ ? hotkeys_.label(0) + L"で戻る" : hotkeys_.label(0) + L"：帰還キー停止中");
    text << L"  /  " << hotkeys_.label(1) << (picker_hotkey_registered_ ? L"でウィンドウ移動" : L"：移動キー停止中");
    RECT label{dpi_pixels(14), 0, rect.right - 4 * dpi_pixels(48) - dpi_pixels(8), rect.bottom};
    DrawTextW(dc, text.str().c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    const wchar_t* captions[] = {L"表示", L"−", L"□", L"×"};
    for (int i = 0; i < 4; ++i) {
      RECT button{rect.right - (4 - i) * dpi_pixels(48), 0,
                  rect.right - (3 - i) * dpi_pixels(48), rect.bottom};
      if (chrome_hover_ == i) {
        HBRUSH hover = CreateSolidBrush(i == 3 ? RGB(192, 48, 62) : RGB(50, 56, 67));
        FillRect(dc, &button, hover);
        DeleteObject(hover);
      }
      DrawTextW(dc, captions[i], -1, &button, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    BitBlt(destination, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(chrome_, &paint);
  }

  void paint_ring() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(ring_, &paint);
    RECT rect{};
    GetClientRect(ring_, &rect);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const float progress = std::min(1.0F, static_cast<float>(GetTickCount64() - ring_started_) / 700.0F);
    const int radius = static_cast<int>(dpi_pixels(60) * (1.0F - progress) + dpi_pixels(8));
    const int center = rect.right / 2;
    auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN outline = CreatePen(PS_SOLID, dpi_pixels(5), RGB(250, 250, 250));
    auto old_pen = SelectObject(dc, outline);
    Ellipse(dc, center - radius, center - radius, center + radius, center + radius);
    HPEN accent = CreatePen(PS_SOLID, dpi_pixels(3), RGB(30, 145, 245));
    SelectObject(dc, accent);
    Ellipse(dc, center - radius, center - radius, center + radius, center + radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(outline);
    DeleteObject(accent);
    EndPaint(ring_, &paint);
  }

  bool create_renderer() {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = 0;
    description.Height = 0;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    const HRESULT swap_result = d3d_->factory->CreateSwapChainForHwnd(
        d3d_->device.Get(), hwnd_, &description, nullptr, nullptr,
        &swap_chain_);
    if (FAILED(swap_result)) {
      logger_.write(L"renderer_error=CreateSwapChainForHwnd_" +
                    std::to_wstring(static_cast<std::uint32_t>(swap_result)));
      return false;
    }
    d3d_->factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> latency_swap_chain;
    if (FAILED(swap_chain_.As(&latency_swap_chain)) ||
        FAILED(latency_swap_chain->SetMaximumFrameLatency(1))) {
      logger_.write(L"renderer_error=maximum_frame_latency_unavailable");
      return false;
    }
    frame_latency_waitable_ = latency_swap_chain->GetFrameLatencyWaitableObject();
    if (frame_latency_waitable_ == nullptr) {
      logger_.write(L"renderer_error=frame_latency_waitable_object_missing");
      return false;
    }
    frame_latency_wait_ = CreateThreadpoolWait(
        [](PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
          auto* app = static_cast<ViewerApp*>(context);
          PostMessageW(app->hwnd_, kPresentReadyMessage, 0, 0);
        }, this, nullptr);
    if (!frame_latency_wait_) {
      logger_.write(L"renderer_error=CreateThreadpoolWait");
      return false;
    }
    logger_.write(L"swapchain_mode=flip_discard vsync=true "
                  L"maximum_frame_latency=1 waitable_object=true pacing=event_driven");

    static constexpr char vertex_shader_source[] = R"(
      struct VertexOutput {
        float4 position : SV_Position;
        float2 uv : TEXCOORD0;
      };
      VertexOutput main(uint vertex_id : SV_VertexID) {
        const float2 positions[3] = {
          float2(-1.0,  1.0),
          float2( 3.0,  1.0),
          float2(-1.0, -3.0)
        };
        const float2 uvs[3] = {
          float2(0.0, 0.0),
          float2(2.0, 0.0),
          float2(0.0, 2.0)
        };
        VertexOutput output;
        output.position = float4(positions[vertex_id], 0.0, 1.0);
        output.uv = uvs[vertex_id];
        return output;
      }
    )";
    static constexpr char pixel_shader_source[] = R"(
      Texture2D source_texture : register(t0);
      SamplerState source_sampler : register(s0);
      float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0)
          : SV_Target {
        return source_texture.Sample(source_sampler, uv);
      }
    )";

    ComPtr<ID3DBlob> vertex_blob;
    ComPtr<ID3DBlob> pixel_blob;
    ComPtr<ID3DBlob> errors;
    HRESULT compile_result = D3DCompile(
        vertex_shader_source, sizeof(vertex_shader_source) - 1,
        "viewer_vertex_shader", nullptr, nullptr, "main", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex_blob, &errors);
    if (FAILED(compile_result)) {
      logger_.write(L"renderer_error=vertex_shader_compile");
      return false;
    }
    errors.Reset();
    compile_result = D3DCompile(
        pixel_shader_source, sizeof(pixel_shader_source) - 1,
        "viewer_pixel_shader", nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel_blob, &errors);
    if (FAILED(compile_result)) {
      logger_.write(L"renderer_error=pixel_shader_compile");
      return false;
    }
    winrt::check_hresult(d3d_->device->CreateVertexShader(
        vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr,
        &vertex_shader_));
    winrt::check_hresult(d3d_->device->CreatePixelShader(
        pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr,
        &pixel_shader_));

    D3D11_SAMPLER_DESC sampler_description{};
    sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(d3d_->device->CreateSamplerState(
        &sampler_description, &sampler_));
    return recreate_render_target();
  }

  bool recreate_render_target() {
    render_target_.Reset();
    ComPtr<ID3D11Texture2D> back_buffer;
    const HRESULT buffer_result =
        swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(buffer_result)) {
      logger_.write(L"renderer_error=GetBuffer");
      return false;
    }
    const HRESULT target_result = d3d_->device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &render_target_);
    if (FAILED(target_result)) {
      logger_.write(L"renderer_error=CreateRenderTargetView");
      return false;
    }
    return true;
  }

  void resize_swap_chain(UINT width, UINT height) {
    if (width == 0 || height == 0) {
      return;
    }
    ++resize_count_;
    const auto resize_begin = std::chrono::steady_clock::now();
    d3d_->context->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_.Reset();
    const HRESULT resize_result = swap_chain_->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (SUCCEEDED(resize_result)) {
      recreate_render_target();
      render_latest_frame();
      log_window_geometry(L"resize");
      update_title(0.0);
    } else {
      logger_.write(L"renderer_error=ResizeBuffers_" +
                    std::to_wstring(static_cast<std::uint32_t>(resize_result)));
    }
    resize_samples_ms_.push_back(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - resize_begin).count());
  }

  bool start_capture() {
    if (!capture::GraphicsCaptureSession::IsSupported()) {
      logger_.write(L"capture_error=GraphicsCaptureSession_unsupported");
      return false;
    }
    auto interop = winrt::get_activation_factory<
        capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(
        target_->monitor, winrt::guid_of<capture::GraphicsCaptureItem>(),
        winrt::put_abi(item)));
    capture_item_ = item;
    const auto size = capture_item_.Size();
    if (size.Width <= 0 || size.Height <= 0) {
      logger_.write(L"capture_error=invalid_item_size");
      return false;
    }
    frame_pool_ = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        d3d_->winrt_device,
        directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    session_ = frame_pool_.CreateCaptureSession(capture_item_);
    try {
      session_.IsCursorCaptureEnabled(true);
    } catch (...) {
      logger_.write(L"capture_warning=cursor_property_unavailable");
    }
    frame_token_ = frame_pool_.FrameArrived(
        [this](const capture::Direct3D11CaptureFramePool& sender,
               const winrt::Windows::Foundation::IInspectable&) noexcept {
          try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
              return;
            }
            auto access = frame.Surface().as<
                Windows::Graphics::DirectX::Direct3D11::
                    IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(
                access->GetInterface(IID_PPV_ARGS(&texture)));
            const auto content_size = frame.ContentSize();
            const std::uint64_t generation =
                received_frames_.fetch_add(1) + 1;
            {
              std::lock_guard lock(frame_mutex_);
              latest_texture_ = std::move(texture);
              latest_width_ = content_size.Width;
              latest_height_ = content_size.Height;
              latest_generation_ = generation;
            }
            if (!frame_message_pending_.exchange(true)) {
              PostMessageW(hwnd_, kFrameMessage, 0, 0);
            }
          } catch (...) {
            capture_errors_.fetch_add(1);
          }
        });
    session_.StartCapture();
    logger_.write(L"capture_started=true size=" +
                  std::to_wstring(size.Width) + L"x" +
                  std::to_wstring(size.Height));
    return true;
  }

  void stop_capture() {
    if (frame_pool_) {
      try {
        frame_pool_.FrameArrived(frame_token_);
      } catch (...) {
      }
    }
    if (session_) {
      session_.Close();
    }
    if (frame_pool_) {
      frame_pool_.Close();
    }
    session_ = nullptr;
    frame_pool_ = nullptr;
    capture_item_ = nullptr;
    {
      std::lock_guard lock(frame_mutex_);
      latest_texture_.Reset();
      latest_generation_ = 0;
    }
    presentation_texture_.Reset();
    presentation_view_.Reset();
  }

  void restart_capture() {
    logger_.write(L"capture_restart_begin=true");
    stop_capture();
    auto new_target = find_target_monitor(logger_, options_.monitor_match);
    if (!new_target.has_value()) {
      logger_.write(L"capture_restart_result=target_missing");
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    if (!has_required_refresh(*new_target, options_.required_refresh_hz)) {
      logger_.write(L"capture_restart_result=refresh_mismatch current_hz=" +
                    std::to_wstring(new_target->refresh_hz));
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    target_ = std::move(new_target);
    if (!start_capture()) {
      logger_.write(L"capture_restart_result=failed");
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    logger_.write(L"capture_restart_result=pass");
  }

  void ensure_presentation_texture(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    bool recreate = presentation_texture_ == nullptr;
    if (!recreate) {
      D3D11_TEXTURE2D_DESC current{};
      presentation_texture_->GetDesc(&current);
      recreate = current.Width != source_description.Width ||
                 current.Height != source_description.Height ||
                 current.Format != source_description.Format;
    }
    if (!recreate) {
      return;
    }

    D3D11_TEXTURE2D_DESC description = source_description;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_DEFAULT;
    presentation_texture_.Reset();
    presentation_view_.Reset();
    winrt::check_hresult(d3d_->device->CreateTexture2D(
        &description, nullptr, &presentation_texture_));
    winrt::check_hresult(d3d_->device->CreateShaderResourceView(
        presentation_texture_.Get(), nullptr, &presentation_view_));
    logger_.write(L"presentation_texture_size=" +
                  std::to_wstring(description.Width) + L"x" +
                  std::to_wstring(description.Height));
  }

  ContentRect calculate_content_rect(int source_width, int source_height) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const float client_width = static_cast<float>(client.right - client.left);
    const float client_height = static_cast<float>(client.bottom - client.top);
    if (client_width <= 0.0F || client_height <= 0.0F || source_width <= 0 ||
        source_height <= 0) {
      return {};
    }
    const float source_aspect =
        static_cast<float>(source_width) / static_cast<float>(source_height);
    const float client_aspect = client_width / client_height;
    ContentRect result;
    if (client_aspect > source_aspect) {
      result.height = client_height;
      result.width = result.height * source_aspect;
      result.left = (client_width - result.width) * 0.5F;
      result.top = 0.0F;
    } else {
      result.width = client_width;
      result.height = result.width / source_aspect;
      result.left = 0.0F;
      result.top = (client_height - result.height) * 0.5F;
    }
    return result;
  }

  void render_latest_frame() {
    render_requested_ = true;
    if (!render_target_ || !swap_chain_ || IsIconic(hwnd_)) {
      return;
    }
    if (!render_slot_available_) {
      // Wait once off the UI thread; new captures update the latest image but
      // never postpone an already pending presentation. WM_APP also works in
      // the native move/resize and menu loops, unlike a custom outer render loop.
      if (!render_wait_armed_ && frame_latency_wait_) {
        render_wait_armed_ = true;
        render_wait_started_ = std::chrono::steady_clock::now();
        SetThreadpoolWait(frame_latency_wait_, frame_latency_waitable_, nullptr);
      }
      return;
    }
    render_requested_ = false;
    render_slot_available_ = false;
    const auto render_begin = std::chrono::steady_clock::now();
    ComPtr<ID3D11Texture2D> source;
    int source_width = 0;
    int source_height = 0;
    std::uint64_t generation = 0;
    {
      std::lock_guard lock(frame_mutex_);
      source = latest_texture_;
      source_width = latest_width_;
      source_height = latest_height_;
      generation = latest_generation_;
    }

    const float clear_colour[4] = {0.015F, 0.015F, 0.02F, 1.0F};
    d3d_->context->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
    d3d_->context->ClearRenderTargetView(render_target_.Get(), clear_colour);

    if (source) {
      try {
        ensure_presentation_texture(source.Get());
        d3d_->context->CopyResource(presentation_texture_.Get(), source.Get());
        const ContentRect content =
            calculate_content_rect(source_width, source_height);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = content.left;
        viewport.TopLeftY = content.top;
        viewport.Width = content.width;
        viewport.Height = content.height;
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        d3d_->context->RSSetViewports(1, &viewport);
        d3d_->context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d_->context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        d3d_->context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        d3d_->context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        d3d_->context->PSSetShaderResources(
            0, 1, presentation_view_.GetAddressOf());
        d3d_->context->Draw(3, 0);
        ID3D11ShaderResourceView* null_view = nullptr;
        d3d_->context->PSSetShaderResources(0, 1, &null_view);
      } catch (const winrt::hresult_error& error) {
        logger_.write(L"render_error=" +
                      std::wstring(error.message().c_str()));
      }
    }

    const HRESULT present_result = swap_chain_->Present(0, 0);
    const auto present_end = std::chrono::steady_clock::now();
    if (previous_present_time_.time_since_epoch().count() != 0) {
      present_interval_samples_ms_.push_back(
          std::chrono::duration<double, std::milli>(present_end -
                                                    previous_present_time_)
              .count());
    }
    previous_present_time_ = present_end;
    render_duration_samples_ms_.push_back(
        std::chrono::duration<double, std::milli>(present_end - render_begin)
            .count());
    if (present_result == DXGI_ERROR_DEVICE_REMOVED ||
        present_result == DXGI_ERROR_DEVICE_RESET) {
      logger_.write(L"render_error=device_removed");
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    if (source && generation != 0 && generation != consumed_generation_.load()) {
      const auto previous_generation = consumed_generation_.load();
      if (previous_generation != 0 && generation > previous_generation + 1) {
        coalesced_frames_.fetch_add(generation - previous_generation - 1);
      }
      consumed_generation_.store(generation);
      rendered_frames_.fetch_add(1);
    }
  }

  void keep_viewer_off_target(WINDOWPOS* position) {
    if (!target_ || !hwnd_ || !position || IsIconic(hwnd_)) return;
    MONITORINFO host{sizeof(MONITORINFO)};
    const auto primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (primary == target_->monitor || !GetMonitorInfoW(primary, &host)) return;
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const LONG x = (position->flags & SWP_NOMOVE) ? current.left : position->x;
    const LONG y = (position->flags & SWP_NOMOVE) ? current.top : position->y;
    const LONG width = (position->flags & SWP_NOSIZE) ? current.right - current.left : position->cx;
    const LONG height = (position->flags & SWP_NOSIZE) ? current.bottom - current.top : position->cy;
    const RECT proposed{x, y, x + width, y + height};
    RECT overlap{};
    if (!IntersectRect(&overlap, &proposed, &target_->bounds)) return;
    position->cx = std::min(width, host.rcWork.right - host.rcWork.left);
    position->cy = std::min(height, host.rcWork.bottom - host.rcWork.top);
    position->x = std::clamp(x, host.rcWork.left, host.rcWork.right - position->cx);
    position->y = std::clamp(y, host.rcWork.top, host.rcWork.bottom - position->cy);
    position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    logger_.write(L"viewer_placement_guard=returned_to_physical");
  }

  void set_view_size(double scale) {
    if (!hwnd_ || !target_) return;
    if (fullscreen_) toggle_fullscreen();
    if (IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    RECT desired{0, 0,
        static_cast<LONG>(std::lround((target_->bounds.right - target_->bounds.left) * scale)),
        static_cast<LONG>(std::lround((target_->bounds.bottom - target_->bounds.top) * scale))};
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    update_title(0.0);
  }

  void toggle_fullscreen() {
    if (!fullscreen_) {
      windowed_style_ = static_cast<DWORD>(
          GetWindowLongPtrW(hwnd_, GWL_STYLE));
      windowed_placement_.length = sizeof(windowed_placement_);
      GetWindowPlacement(hwnd_, &windowed_placement_);
      GetWindowRect(hwnd_, &windowed_bounds_);
      const HMONITOR physical_monitor =
          MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      GetMonitorInfoW(physical_monitor, &info);
      fullscreen_ = true;
      SetWindowLongPtrW(hwnd_, GWL_STYLE,
                        windowed_style_ & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(hwnd_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                   info.rcMonitor.right - info.rcMonitor.left,
                   info.rcMonitor.bottom - info.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
      logger_.write(L"fullscreen_entered=true");
      log_window_geometry(L"fullscreen_enter");
    } else {
      fullscreen_ = false;
      SetWindowLongPtrW(hwnd_, GWL_STYLE, windowed_style_);
      SetWindowPlacement(hwnd_, &windowed_placement_);
      SetWindowPos(hwnd_, nullptr, windowed_bounds_.left, windowed_bounds_.top,
                   windowed_bounds_.right - windowed_bounds_.left,
                   windowed_bounds_.bottom - windowed_bounds_.top,
                   SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
      logger_.write(L"fullscreen_exited=true");
      log_window_geometry(L"fullscreen_exit");
    }
    update_title(0.0);
  }

  void log_window_geometry(const std::wstring& event) {
    RECT window{};
    RECT client{};
    GetWindowRect(hwnd_, &window);
    GetClientRect(hwnd_, &client);
    logger_.write(L"window_geometry event=" + event + L" left=" +
                  std::to_wstring(window.left) + L" top=" +
                  std::to_wstring(window.top) + L" width=" +
                  std::to_wstring(window.right - window.left) + L" height=" +
                  std::to_wstring(window.bottom - window.top) +
                  L" client_width=" +
                  std::to_wstring(client.right - client.left) +
                  L" client_height=" +
                  std::to_wstring(client.bottom - client.top) +
                  L" fullscreen=" + (fullscreen_ ? L"true" : L"false"));
  }

  void update_title(double frames_per_second) {
    if (!hwnd_ || !target_) return;
    const int source_width = target_->bounds.right - target_->bounds.left;
    const int source_height = target_->bounds.bottom - target_->bounds.top;
    const auto content = calculate_content_rect(source_width, source_height);
    std::wostringstream title;
    title << L"VKomado | " << std::fixed
          << std::setprecision(1)
          << source_width << L"x" << source_height << L" → "
          << std::lround(content.width) << L"x" << std::lround(content.height)
          << L" (" << (100.0 * content.width / source_width) << L"%) | "
          << (return_hotkey_registered_ ? L"クリックで移動 / " + hotkeys_.label(0) + L"でメイン画面へ" : hotkeys_.label(0) + L"登録不可・画面端から戻る")
          << L" | " << frames_per_second << L" fps";
    SetWindowTextW(hwnd_, title.str().c_str());
    if (chrome_ && IsWindowVisible(chrome_)) InvalidateRect(chrome_, nullptr, FALSE);
  }

  void update_telemetry() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration<double>(now - previous_telemetry_time_).count();
    const auto rendered = rendered_frames_.load();
    const std::uint64_t rendered_delta = rendered - previous_rendered_frames_;
    const double frames_per_second =
        elapsed > 0.0 ? static_cast<double>(rendered_delta) / elapsed : 0.0;
    previous_rendered_frames_ = rendered;
    previous_telemetry_time_ = now;
    const auto interval_samples = std::move(present_interval_samples_ms_);
    const auto wait_samples = std::move(frame_wait_samples_ms_);
    const auto render_samples = std::move(render_duration_samples_ms_);
    const auto chrome_samples = std::move(chrome_position_samples_ms_);
    const auto resize_samples = std::move(resize_samples_ms_);
    const auto ui_tick_samples = std::move(ui_tick_samples_ms_);
    present_interval_samples_ms_.clear();
    frame_wait_samples_ms_.clear();
    render_duration_samples_ms_.clear();
    chrome_position_samples_ms_.clear();
    resize_samples_ms_.clear();
    ui_tick_samples_ms_.clear();
    const double interval_p50 = percentile(interval_samples, 0.50);
    const double interval_p95 = percentile(interval_samples, 0.95);
    const double interval_p99 = percentile(interval_samples, 0.99);
    const double interval_max = percentile(interval_samples, 1.00);
    const double wait_p95 = percentile(wait_samples, 0.95);
    const double render_p95 = percentile(render_samples, 0.95);
    update_title(frames_per_second);
    logger_.write(L"telemetry received=" +
                  std::to_wstring(received_frames_.load()) + L" rendered=" +
                  std::to_wstring(rendered) + L" capture_errors=" +
                  std::to_wstring(capture_errors_.load()) + L" fps=" +
                  std::to_wstring(frames_per_second) + L" fullscreen=" +
                  (fullscreen_ ? L"true" : L"false") + L" input=false mode=cursor_jump" +
                  L" coalesced=" + std::to_wstring(coalesced_frames_.load()) +
                  L" interval_p50_ms=" + std::to_wstring(interval_p50) +
                  L" interval_p95_ms=" + std::to_wstring(interval_p95) +
                  L" interval_p99_ms=" + std::to_wstring(interval_p99) +
                  L" interval_max_ms=" + std::to_wstring(interval_max) +
                  L" wait_p95_ms=" + std::to_wstring(wait_p95) +
                  L" render_p95_ms=" + std::to_wstring(render_p95) +
                  L" render_max_ms=" + std::to_wstring(percentile(render_samples, 1.0)) +
                  L" moving=" + (moving_window_ ? L"true" : L"false") +
                  L" chrome_move_p95_ms=" + std::to_wstring(percentile(chrome_samples, 0.95)) +
                  L" chrome_move_max_ms=" + std::to_wstring(percentile(chrome_samples, 1.0)) +
                  L" resize_count=" + std::to_wstring(resize_count_) +
                  L" resize_max_ms=" + std::to_wstring(percentile(resize_samples, 1.0)) +
                  L" ui_timer_gap_max_ms=" + std::to_wstring(percentile(ui_tick_samples, 1.0)));
    if (options_.auto_exit_ms > 0) {
      const auto runtime = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - start_time_);
      if (runtime.count() >= options_.auto_exit_ms) {
        logger_.write(L"auto_exit=true");
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      }
    }
  }

  void stop_render_wait() {
    if (frame_latency_wait_) {
      SetThreadpoolWait(frame_latency_wait_, nullptr, nullptr);
      WaitForThreadpoolWaitCallbacks(frame_latency_wait_, TRUE);
      CloseThreadpoolWait(frame_latency_wait_);
      frame_latency_wait_ = nullptr;
    }
    if (frame_latency_waitable_) {
      CloseHandle(frame_latency_waitable_);
      frame_latency_waitable_ = nullptr;
    }
    render_wait_armed_ = false;
    render_slot_available_ = false;
  }

  void shutdown() {
    stop_render_wait();
    stop_capture();
  }

  Options options_;
  Logger logger_;
  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND chrome_ = nullptr;
  HWND ring_ = nullptr;
  bool return_hotkey_registered_ = false;
  bool picker_hotkey_registered_ = false;
  sharedvd::WindowPicker picker_;
  sharedvd::Hotkeys hotkeys_;
  sharedvd::DialogFollower dialog_follower_;
  bool click_pending_ = false;
  bool moving_window_ = false;
  bool menu_open_ = false;
  int chrome_pressed_ = -1;
  int chrome_hover_ = -1;
  ULONGLONG ring_started_ = 0;
  std::optional<TargetMonitor> target_;
  std::optional<D3DState> d3d_;

  ComPtr<IDXGISwapChain1> swap_chain_;
  HANDLE frame_latency_waitable_ = nullptr;
  PTP_WAIT frame_latency_wait_ = nullptr;
  bool render_wait_armed_ = false;
  bool render_slot_available_ = false;
  bool render_requested_ = false;
  std::chrono::steady_clock::time_point render_wait_started_{};
  ComPtr<ID3D11RenderTargetView> render_target_;
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Texture2D> presentation_texture_;
  ComPtr<ID3D11ShaderResourceView> presentation_view_;

  capture::GraphicsCaptureItem capture_item_{nullptr};
  capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
  capture::GraphicsCaptureSession session_{nullptr};
  winrt::event_token frame_token_{};

  std::mutex frame_mutex_;
  ComPtr<ID3D11Texture2D> latest_texture_;
  int latest_width_ = 0;
  int latest_height_ = 0;
  std::uint64_t latest_generation_ = 0;
  std::atomic<bool> frame_message_pending_{false};
  std::atomic<std::uint64_t> consumed_generation_{0};
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> rendered_frames_{0};
  std::atomic<std::uint64_t> capture_errors_{0};
  std::atomic<std::uint64_t> coalesced_frames_{0};

  bool fullscreen_ = false;
  DWORD windowed_style_ = kViewerStyle;
  WINDOWPLACEMENT windowed_placement_{sizeof(WINDOWPLACEMENT)};
  RECT windowed_bounds_{};

  std::chrono::steady_clock::time_point start_time_{};
  std::chrono::steady_clock::time_point previous_telemetry_time_{};
  std::chrono::steady_clock::time_point previous_present_time_{};
  std::uint64_t previous_rendered_frames_ = 0;
  std::vector<double> present_interval_samples_ms_;
  std::vector<double> frame_wait_samples_ms_;
  std::vector<double> render_duration_samples_ms_;
  std::vector<double> chrome_position_samples_ms_;
  std::vector<double> resize_samples_ms_;
  std::uint64_t resize_count_ = 0;
  std::chrono::steady_clock::time_point previous_ui_tick_{};
  std::vector<double> ui_tick_samples_ms_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const Options options = parse_options();
    if (options.shortcuts_only) return sharedvd::run_shortcut_editor(options.hotkey_config);
    if (options.rescue_only || options.list_windows) {
      Logger logger(options.log_path);
      const auto target = find_target_monitor(logger, options.monitor_match);
      if (!target) return 2;
      return rescue_windows(*target, logger, options.list_windows, options.rescue_process_id).failed ? 4 : 0;
    }
    winrt::handle single_instance(CreateMutexW(nullptr, FALSE, L"Local\\SharedVdLoopbackViewer"));
    if (!single_instance || GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBoxW(nullptr, L"ビューアは既に起動中です。既存のウィンドウを使用してください。",
                  L"VKomado", MB_OK | MB_ICONINFORMATION);
      return 3;
    }
    ViewerApp viewer(options);
    if (!viewer.initialize(instance)) {
      const std::wstring message =
          L"ビューアを起動できませんでした。\n"
          L"仮想ディスプレイが拡張表示・60Hzで有効になっているか確認してください。\n"
          L"既定では VDD by MTT が1台だけ存在する構成に対応します。\n\n"
          L"詳細ログ:\n" + options.log_path.wstring();
      MessageBoxW(nullptr, message.c_str(), L"VKomado", MB_ICONERROR | MB_OK);
      return 2;
    }
    return viewer.run();
  } catch (const winrt::hresult_error& error) {
    MessageBoxW(nullptr, error.message().c_str(), L"VKomado",
                MB_ICONERROR | MB_OK);
    return 10;
  } catch (const std::exception& error) {
    const std::wstring message(error.what(), error.what() +
                                                 std::strlen(error.what()));
    MessageBoxW(nullptr, message.c_str(), L"VKomado",
                MB_ICONERROR | MB_OK);
    return 11;
  }
}
