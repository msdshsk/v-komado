#include "hotkeys.h"
#include <commctrl.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

namespace sharedvd {
KeyBindings default_bindings() { return {{{0, VK_F6}, {MOD_SHIFT, VK_F6}, {0, VK_F8}}}; }
std::filesystem::path hotkey_config_path() {
  PWSTR value{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) throw std::runtime_error("Cannot locate LocalAppData");
  std::filesystem::path path = std::filesystem::path(value) / L"SharedVD" / L"hotkeys.conf";
  CoTaskMemFree(value); return path;
}
std::wstring key_label(KeyBinding binding) {
  if (!binding.key) return L"無効";
  std::wstring result;
  if (binding.modifiers & MOD_CONTROL) result += L"Ctrl+";
  if (binding.modifiers & MOD_ALT) result += L"Alt+";
  if (binding.modifiers & MOD_SHIFT) result += L"Shift+";
  if (binding.key >= VK_F1 && binding.key <= VK_F24) return result + L"F" + std::to_wstring(binding.key - VK_F1 + 1);
  wchar_t name[128]{};
  UINT scan = MapVirtualKeyW(binding.key, MAPVK_VK_TO_VSC);
  if (binding.key >= VK_PRIOR && binding.key <= VK_DELETE) scan |= 0x100;
  if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 128)) return result + name;
  return result + L"Key " + std::to_wstring(binding.key);
}
std::wstring validate_bindings(const KeyBindings& bindings) {
  for (const auto key : bindings) {
    if (!key.key && !key.modifiers) continue;
    if (key.modifiers & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT)) return L"Windowsキーを含む組み合わせは使用できません。";
    if (key.key == VK_F12) return L"F12はWindowsのデバッガー用に予約されています。別のキーを選んでください。";
    if (key.key < VK_BACK || key.key > 0xFE || key.key == VK_SHIFT || key.key == VK_CONTROL || key.key == VK_MENU ||
        key.key == VK_LWIN || key.key == VK_RWIN || (key.key >= VK_LSHIFT && key.key <= VK_RMENU))
      return L"修飾キーだけでは登録できません。キーを入力するか「使用する」を外してください。";
    const bool function = key.key >= VK_F1 && key.key <= VK_F24;
    if (!function && !(key.modifiers & (MOD_CONTROL | MOD_ALT)))
      return L"文字や通常キーにはCtrlまたはAltを組み合わせてください。Fキーは単独でも使えます。";
    if ((key.modifiers == MOD_ALT && (key.key == VK_F4 || key.key == VK_TAB || key.key == VK_ESCAPE)) ||
        (key.key == VK_DELETE && key.modifiers == (MOD_CONTROL | MOD_ALT)) ||
        (key.key == VK_ESCAPE && key.modifiers == (MOD_CONTROL | MOD_SHIFT)))
      return L"Windowsの基本操作に使われる組み合わせです。別のキーを選んでください。";
  }
  for (int i = 0; i < kShortcutCount; ++i)
    for (int j = i + 1; j < kShortcutCount; ++j)
      if (bindings[i].key && bindings[i] == bindings[j]) return L"複数の操作に同じキーは設定できません。";
  return {};
}
Hotkeys::~Hotkeys() { shutdown(); }
void Hotkeys::unbind() {
  for (int i = 0; i < kShortcutCount; ++i) { if (registered_[i]) UnregisterHotKey(owner_, 60 + i); registered_[i] = false; }
}
void Hotkeys::bind() {
  unbind();
  if (!active_) return;
  for (int i = 0; i < kShortcutCount; ++i) if (bindings_[i].key)
    registered_[i] = RegisterHotKey(owner_, 60 + i, bindings_[i].modifiers | MOD_NOREPEAT, bindings_[i].key) != FALSE;
}
void Hotkeys::initialize(HWND owner, std::filesystem::path path, std::function<void()> changed, bool activate) {
  owner_ = owner; path_ = std::move(path); changed_ = std::move(changed); active_ = activate; closing_ = false; reload();
}
void Hotkeys::reload() {
  if (editor_) return;
  bindings_ = default_bindings(); follow_dialogs_ = false; load_warning_.clear();
  if (std::filesystem::exists(path_)) {
    std::ifstream file(path_); unsigned version{}, follow{}; auto candidate = default_bindings(); std::string trailing;
    bool ok = static_cast<bool>(file >> version) && (version == 1 || version == 2);
    for (int i = 0; ok && i < (version == 1 ? 2 : kShortcutCount); ++i)
      ok = static_cast<bool>(file >> candidate[i].modifiers >> candidate[i].key);
    if (ok && version == 2) ok = static_cast<bool>(file >> follow) && follow <= 1;
    // Preserve both old bindings if the user already assigned F8 to either of them.
    if (version == 1 && (candidate[2] == candidate[0] || candidate[2] == candidate[1])) candidate[2] = {};
    if (ok && !(file >> trailing) && validate_bindings(candidate).empty()) { bindings_ = candidate; follow_dialogs_ = follow != 0; }
    else load_warning_ = L"保存された設定を読み取れなかったため、初期値で起動しました。保存すると修復できます。";
  }
  bind(); if (changed_) changed_();
}
void Hotkeys::shutdown() {
  closing_ = true;
  if (editor_) DestroyWindow(editor_);
  unbind(); owner_ = nullptr;
}
bool Hotkeys::register_pair(const KeyBindings& candidate, std::wstring& error) {
  unbind();
  for (int i = 0; i < kShortcutCount; ++i) if (candidate[i].key) {
    registered_[i] = RegisterHotKey(owner_, 60 + i, candidate[i].modifiers | MOD_NOREPEAT, candidate[i].key) != FALSE;
    if (!registered_[i]) {
      error = key_label(candidate[i]) + L" を登録できません。他のアプリやWindowsで使用されている可能性があります。";
      unbind(); return false;
    }
  }
  return true;
}
bool Hotkeys::apply(const KeyBindings& candidate, std::wstring& error, std::optional<bool> follow_dialogs) {
  error = validate_bindings(candidate); if (!error.empty()) return false;
  if (!register_pair(candidate, error)) { if (!editor_) bind(); return false; }
  std::filesystem::path temporary = path_;
  temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
  try {
    std::filesystem::create_directories(path_.parent_path());
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output << "2\n";
      for (const auto key : candidate) output << key.modifiers << " " << key.key << "\n";
      output << follow_dialogs.value_or(follow_dialogs_) << "\n";
      output.close(); if (!output) throw std::runtime_error("write");
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("replace");
  } catch (...) {
    std::error_code ec; std::filesystem::remove(temporary, ec);
    unbind(); if (!editor_) bind(); error = L"設定ファイルを保存できません。以前の設定を保持しました。"; return false;
  }
  bindings_ = candidate; follow_dialogs_ = follow_dialogs.value_or(follow_dialogs_); load_warning_.clear();
  if (!active_) unbind();
  if (changed_) changed_();
  HWND viewer = FindWindowW(L"LGWLViewerWindow", nullptr);
  if (viewer && viewer != owner_) PostMessageW(viewer, kReloadShortcutsMessage, 0, 0);
  return true;
}
void Hotkeys::populate(const KeyBindings& bindings) {
  for (int i = 0; i < kShortcutCount; ++i) {
    UINT flags = (bindings[i].modifiers & MOD_CONTROL ? HOTKEYF_CONTROL : 0) |
                 (bindings[i].modifiers & MOD_ALT ? HOTKEYF_ALT : 0) |
                 (bindings[i].modifiers & MOD_SHIFT ? HOTKEYF_SHIFT : 0);
    SendDlgItemMessageW(editor_, 2100 + i, HKM_SETHOTKEY, MAKEWORD(bindings[i].key, flags), 0);
    CheckDlgButton(editor_, 2110 + i, bindings[i].key ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(editor_, 2100 + i), bindings[i].key != 0);
  }
}
KeyBindings Hotkeys::edited() const {
  KeyBindings result{};
  for (int i = 0; i < kShortcutCount; ++i) if (IsDlgButtonChecked(editor_, 2110 + i) == BST_CHECKED) {
    WORD key = static_cast<WORD>(SendDlgItemMessageW(editor_, 2100 + i, HKM_GETHOTKEY, 0, 0));
    UINT flags = HIBYTE(key);
    result[i] = {static_cast<UINT>((flags & HOTKEYF_CONTROL ? MOD_CONTROL : 0) |
        (flags & HOTKEYF_ALT ? MOD_ALT : 0) | (flags & HOTKEYF_SHIFT ? MOD_SHIFT : 0)), LOBYTE(key)};
    if (!result[i].key) result[i].modifiers = MOD_SHIFT; // Validation asks for a key rather than silently disabling.
  }
  return result;
}
void Hotkeys::show_editor(std::function<void()> closed) {
  if (HWND existing = FindWindowW(L"SharedVDHotkeyEditor", nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); return; }
  closed_ = std::move(closed); committed_ = false; unbind(); if (changed_) changed_();
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_HOTKEY_CLASS}; InitCommonControlsEx(&controls);
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = editor_proc;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(101)); cls.lpszClassName = L"SharedVDHotkeyEditor"; RegisterClassW(&cls);
  const UINT dpi = GetDpiForWindow(owner_); auto px = [dpi](int n) { return MulDiv(n, dpi ? dpi : 96, 96); };
  MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
  RECT rect{0, 0, px(640), px(540)};
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_CONTROLPARENT, dpi);
  int width = rect.right - rect.left, height = rect.bottom - rect.top;
  editor_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"ショートカット設定 — VKomado", style,
      monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
      monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
      width, height, owner_, nullptr, cls.hInstance, this);
  if (!editor_) { bind(); if (changed_) changed_(); if (closed_) closed_(); return; }
  font_ = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
  auto control = [&](const wchar_t* type, const wchar_t* text, int x, int y, int w, int h, int id, DWORD extra = 0) {
    HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | extra, px(x), px(y), px(w), px(h), editor_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE); return result;
  };
  control(L"STATIC", L"入力欄をクリックして、使いたいキーの組み合わせを押してください。", 24, 22, 594, 26, 0);
  for (int i = 0; i < kShortcutCount; ++i) {
    const wchar_t* labels[] = {L"メイン画面へカーソルを戻す", L"ウィンドウを選んで移動", L"アクティブウィンドウを往復移動"};
    control(L"STATIC", labels[i], 24, 72 + i * 60, 254, 30, 0);
    control(L"BUTTON", L"使用する", 282, 72 + i * 60, 94, 30, 2110 + i, BS_AUTOCHECKBOX | WS_TABSTOP);
    HWND field = control(HOTKEY_CLASSW, L"", 390, 70 + i * 60, 222, 34, 2100 + i, WS_TABSTOP | WS_BORDER);
    SendMessageW(field, HKM_SETRULES, 0, 0);
  }
  control(L"STATIC", L"Ctrl / Alt / Shift＋キー、またはFキー単独を設定できます。\r\nF12・Windowsキーは対象外です。設定中はショートカットを一時停止します。\r\n他アプリ内のショートカットとの重複は自動検出できません。", 24, 252, 594, 72, 0);
  control(L"BUTTON", L"新しい付属ダイアログを仮想画面の親へ寄せる（試験的）", 24, 336, 594, 30, 2130, BS_AUTOCHECKBOX | WS_TABSTOP);
  CheckDlgButton(editor_, 2130, follow_dialogs_ ? BST_CHECKED : BST_UNCHECKED);
  control(L"STATIC", L"同一プロセスで親子関係のある新規ダイアログだけが対象です。\r\n共有中は移動した内容も相手に表示されます。独立した新規アプリは対象外です。", 24, 370, 594, 44, 0);
  control(L"STATIC", load_warning_.c_str(), 24, 422, 594, 56, 2120);
  control(L"BUTTON", L"初期値に戻す", 24, 490, 128, 32, 2121, WS_TABSTOP);
  control(L"BUTTON", L"保存", 390, 490, 100, 32, IDOK, WS_TABSTOP | BS_DEFPUSHBUTTON);
  control(L"BUTTON", L"キャンセル", 502, 490, 110, 32, IDCANCEL, WS_TABSTOP);
  populate(bindings_); ShowWindow(editor_, SW_SHOW); SetForegroundWindow(editor_); SetFocus(GetDlgItem(editor_, 2101));
}
LRESULT CALLBACK Hotkeys::editor_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<Hotkeys*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<Hotkeys*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->editor_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) return DefWindowProcW(window, message, wp, lp);
  switch (message) {
    case WM_CTLCOLORSTATIC: SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW)); return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) {
        std::wstring error;
        if (self->apply(self->edited(), error, IsDlgButtonChecked(window, 2130) == BST_CHECKED)) { self->committed_ = true; DestroyWindow(window); }
        else SetDlgItemTextW(window, 2120, error.c_str());
      } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(window);
      else if (LOWORD(wp) == 2121) { self->populate(default_bindings()); CheckDlgButton(window, 2130, BST_UNCHECKED); SetDlgItemTextW(window, 2120, L"保存すると初期値を適用します。"); }
      else if (LOWORD(wp) >= 2110 && LOWORD(wp) < 2110 + kShortcutCount) EnableWindow(GetDlgItem(window, LOWORD(wp) - 10), IsDlgButtonChecked(window, LOWORD(wp)) == BST_CHECKED);
      return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
      self->editor_ = nullptr; if (self->font_) DeleteObject(self->font_); self->font_ = nullptr;
      if (!self->closing_) {
        if (!self->committed_) self->bind();
        if (self->changed_) self->changed_();
        if (self->closed_) self->closed_();
      }
      return 0;
  }
  return DefWindowProcW(window, message, wp, lp);
}
int run_shortcut_editor(const std::filesystem::path& config) {
  if (HWND viewer = FindWindowW(L"LGWLViewerWindow", nullptr)) {
    DWORD pid{}; GetWindowThreadProcessId(viewer, &pid); AllowSetForegroundWindow(pid);
    PostMessageW(viewer, WM_COMMAND, kShortcutSettingsCommand, 0); return 0;
  }
  if (HWND editor = FindWindowW(L"SharedVDHotkeyEditor", nullptr)) { SetForegroundWindow(editor); return 0; }
  HWND owner = CreateWindowExW(0, L"STATIC", L"SharedVDShortcutHost", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!owner) return 1;
  Hotkeys keys; keys.initialize(owner, config, {}, false); keys.show_editor([] { PostQuitMessage(0); });
  DWORD editor_pid{}; GetWindowThreadProcessId(FindWindowW(L"SharedVDHotkeyEditor", nullptr), &editor_pid);
  if (editor_pid != GetCurrentProcessId()) { keys.shutdown(); DestroyWindow(owner); return 0; }
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    HWND editor = FindWindowW(L"SharedVDHotkeyEditor", nullptr);
    if (!editor || !IsDialogMessageW(editor, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  keys.shutdown(); DestroyWindow(owner); return 0;
}
}
