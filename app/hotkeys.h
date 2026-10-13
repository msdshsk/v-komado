#pragma once
#include <windows.h>
#include <array>
#include <filesystem>
#include <functional>
#include <string>

namespace sharedvd {
constexpr UINT kShortcutSettingsCommand = 1007;
constexpr UINT kReloadShortcutsMessage = WM_APP + 5;
struct KeyBinding {
  UINT modifiers{}, key{};
  bool operator==(const KeyBinding&) const = default;
};
using KeyBindings = std::array<KeyBinding, 2>;
KeyBindings default_bindings();
std::wstring key_label(KeyBinding key);
std::wstring validate_bindings(const KeyBindings& bindings);
std::filesystem::path hotkey_config_path();

class Hotkeys {
 public:
  ~Hotkeys();
  void initialize(HWND owner, std::filesystem::path path, std::function<void()> changed = {}, bool activate = true);
  void reload();
  void shutdown();
  void show_editor(std::function<void()> closed = {});
  bool apply(const KeyBindings& candidate, std::wstring& error);
  const KeyBindings& bindings() const { return bindings_; }
  bool registered(int index) const { return registered_[index]; }
  std::wstring label(int index) const { return key_label(bindings_[index]); }
  std::wstring load_warning() const { return load_warning_; }
 private:
  static LRESULT CALLBACK editor_proc(HWND, UINT, WPARAM, LPARAM);
  void unbind();
  void bind();
  bool register_pair(const KeyBindings&, std::wstring& error);
  void populate(const KeyBindings&);
  KeyBindings edited() const;
  HWND owner_{}, editor_{};
  HFONT font_{};
  bool active_ = true, closing_{}, committed_{};
  std::array<bool, 2> registered_{};
  KeyBindings bindings_ = default_bindings();
  std::filesystem::path path_;
  std::wstring load_warning_;
  std::function<void()> changed_, closed_;
};
int run_shortcut_editor(const std::filesystem::path& config);
}
