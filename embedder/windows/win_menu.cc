#include "embedder/windows/win_menu.h"

#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <utility>

#include "base/logging.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_displays.h"
#include "embedder/windows/win_window.h"
#include "engine/platform/input_types.h"

namespace momo::embedder::windows {

namespace {

constexpr wchar_t kHostClass[] = L"MomoMenuHost";
// 宿主窗口的消息: 弹出已请求的上下文菜单;托盘图标的回调(NOTIFYICON_VERSION_4: LOWORD(lparam) = 事件,
// HIWORD(lparam) = 图标 id)。
constexpr UINT kShowPopupMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;

constexpr uint32_t kAcceleratorModifiers =
    platform::kModifierShift | platform::kModifierControl | platform::kModifierAlt | platform::kModifierMeta;

// Windows 支持的 role: 标准标题、标准快捷键。edit: 快捷键只显示, 按键归获得焦点的内容(见 win_menu.h)。
struct RoleInfo {
  MenuRole role;
  const wchar_t* title;
  const char* key;  // 空 = 没有标准快捷键
  bool edit;
};

const RoleInfo* FindRole(MenuRole role) {
  static constexpr RoleInfo kRoles[] = {
      {MenuRole::kQuit, L"Exit", "", false},
      {MenuRole::kCut, L"Cut", "x", true},
      {MenuRole::kCopy, L"Copy", "c", true},
      {MenuRole::kPaste, L"Paste", "v", true},
      {MenuRole::kSelectAll, L"Select All", "a", true},
      {MenuRole::kMinimize, L"Minimize", "", false},
      {MenuRole::kZoom, L"Maximize", "", false},
      {MenuRole::kClose, L"Close", "", false},
      {MenuRole::kEditMenu, L"Edit", "", false},
      {MenuRole::kWindowMenu, L"Window", "", false},
  };
  for (const RoleInfo& info : kRoles) {
    if (info.role == role) return &info;
  }
  return nullptr;
}

struct NamedKey {
  std::string_view name;
  UINT virtual_key;
};
constexpr NamedKey kNamedKeys[] = {
    {"Enter", VK_RETURN}, {"Tab", VK_TAB},     {"Space", VK_SPACE},   {"Backspace", VK_BACK},
    {"Delete", VK_DELETE}, {"Insert", VK_INSERT}, {"Escape", VK_ESCAPE}, {"Up", VK_UP},
    {"Down", VK_DOWN},    {"Left", VK_LEFT},   {"Right", VK_RIGHT},   {"Home", VK_HOME},
    {"End", VK_END},      {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
};

// MenuAccelerator.key → 虚拟键;不认识时为 0。
UINT VirtualKeyFor(const std::string& key) {
  for (const NamedKey& named : kNamedKeys) {
    if (named.name == key) return named.virtual_key;
  }
  if (key.size() >= 2 && key[0] == 'F') {  // F1–F24
    const int n = std::atoi(key.c_str() + 1);
    return n >= 1 && n <= 24 ? static_cast<UINT>(VK_F1 + n - 1) : 0;
  }
  const std::wstring wide = Utf8ToWide(key);
  if (wide.size() != 1) return 0;
  const wchar_t c = wide[0];
  if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
    return static_cast<UINT>(std::towupper(c));  // 字母与数字的虚拟键即其大写 ASCII
  }
  const SHORT scan = ::VkKeyScanW(c);  // 其余字符按当前键盘布局
  return scan == -1 ? 0 : static_cast<UINT>(scan & 0xFF);
}

// 菜单项右侧显示的快捷键文字("Ctrl+Shift+N")。
std::wstring AcceleratorText(const MenuAccelerator& accelerator) {
  std::wstring text;
  if ((accelerator.modifiers & platform::kModifierControl) != 0) text += L"Ctrl+";
  if ((accelerator.modifiers & platform::kModifierAlt) != 0) text += L"Alt+";
  if ((accelerator.modifiers & platform::kModifierShift) != 0) text += L"Shift+";
  if ((accelerator.modifiers & platform::kModifierMeta) != 0) text += L"Win+";
  std::wstring key = Utf8ToWide(accelerator.key);
  if (key.size() == 1) key[0] = static_cast<wchar_t>(std::towupper(key[0]));
  return text + key;
}

}  // namespace

// ---------------------------------------------------------------------------
// 托盘图标

class WinMenus::Tray final : public PlatformTray {
 public:
  Tray(WinMenus& menus, int64_t tray_id, TrayDelegate* delegate)
      : menus_(menus), tray_id_(tray_id), icon_id_(static_cast<UINT>(tray_id)), delegate_(delegate) {
    menus_.trays_[icon_id_] = this;
  }
  ~Tray() override {
    menus_.trays_.erase(icon_id_);
    if (added_) {
      NOTIFYICONDATAW data = Data(0);
      ::Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (icon_ != nullptr) ::DestroyIcon(icon_);
  }

  Status SetImage(const std::string& path) override {
    const std::wstring wide = Utf8ToWide(path);
    HICON icon = static_cast<HICON>(::LoadImageW(nullptr, wide.c_str(), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                                                 ::GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
    if (icon == nullptr) {
      return InvalidArgumentError("cannot load the tray image '" + path + "' (Windows needs an .ico file)");
    }
    if (icon_ != nullptr) ::DestroyIcon(icon_);
    icon_ = icon;
    Update(NIF_ICON);
    return Status::Ok();
  }

  void SetToolTip(std::string_view text) override {
    tooltip_ = Utf8ToWide(text);
    Update(NIF_TIP | NIF_SHOWTIP);
  }

  void SetContextMenu(const MenuModel* menu) override {
    menu_ = menu != nullptr ? std::optional<MenuModel>(*menu) : std::nullopt;
  }

  std::optional<Rect> bounds() const override {
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = menus_.host_;
    identifier.uID = icon_id_;
    RECT rect{};
    if (FAILED(::Shell_NotifyIconGetRect(&identifier, &rect))) {
      return std::nullopt;
    }
    return menus_.ToLogicalRect(rect);
  }

  // 把图标加进通知区域(创建时, 以及资源管理器重启之后)。
  void Add() {
    NOTIFYICONDATAW data = Data(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    added_ = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (!added_) {
      MOMO_LOG(WARNING) << "Shell_NotifyIcon(NIM_ADD) failed for tray " << tray_id_;
      return;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);
  }

  // 图标的回调消息。注意: 打开菜单期间(系统的菜单循环里)JS 可能销毁本图标, 弹出菜单之后不再访问 this。
  void OnNotify(UINT event) {
    switch (event) {
      case WM_LBUTTONUP:
      case NIN_KEYSELECT:
        delegate_->OnTrayClicked(tray_id_, TrayClick::kClick, bounds());
        return;
      case WM_LBUTTONDBLCLK:
        delegate_->OnTrayClicked(tray_id_, TrayClick::kDoubleClick, bounds());
        return;
      case WM_CONTEXTMENU: {
        if (!menu_.has_value()) {
          delegate_->OnTrayClicked(tray_id_, TrayClick::kRightClick, bounds());
          return;
        }
        POINT at{};
        ::GetCursorPos(&at);
        menus_.TrackPopup(BuildPopupMenu(*menu_), menus_.host_, at);
        return;
      }
      default:
        return;
    }
  }

 private:
  NOTIFYICONDATAW Data(UINT flags) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = menus_.host_;
    data.uID = icon_id_;
    data.uFlags = flags;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon_;
    tooltip_.copy(data.szTip, ARRAYSIZE(data.szTip) - 1);  // data 已清零: 截断后仍以 NUL 结尾
    return data;
  }
  void Update(UINT flags) {
    if (!added_) return;  // 尚未加入: Add 时带上全部属性
    NOTIFYICONDATAW data = Data(flags);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
  }

  WinMenus& menus_;
  const int64_t tray_id_;
  const UINT icon_id_;
  TrayDelegate* const delegate_;
  HICON icon_ = nullptr;
  std::wstring tooltip_;
  std::optional<MenuModel> menu_;
  bool added_ = false;
};

// ---------------------------------------------------------------------------
// WinMenus

WinMenus::WinMenus(WinDisplays& displays) : displays_(displays) {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &WinMenus::WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kHostClass;
  ::RegisterClassExW(&wc);  // 已注册(ERROR_CLASS_ALREADY_EXISTS)也可以继续
  host_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                            nullptr);
  if (host_ == nullptr) {
    MOMO_LOG(ERROR) << "WinMenus: CreateWindowExW failed: " << ::GetLastError();
    return;
  }
  ::SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  taskbar_created_message_ = ::RegisterWindowMessageW(L"TaskbarCreated");
}

WinMenus::~WinMenus() {
  ClosePopup();
  if (host_ != nullptr) {
    ::SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
    ::DestroyWindow(host_);
  }
}

LRESULT CALLBACK WinMenus::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<WinMenus*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return self != nullptr ? self->HandleMessage(message, wparam, lparam)
                         : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT WinMenus::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kShowPopupMessage) {
    ShowPendingPopup();
    return 0;
  }
  if (message == kTrayMessage) {
    auto it = trays_.find(HIWORD(lparam));
    if (it != trays_.end()) it->second->OnNotify(LOWORD(lparam));
    return 0;
  }
  if (message == taskbar_created_message_ && message != 0) {
    for (const auto& [id, tray] : trays_) tray->Add();  // 资源管理器重启: 图标都没了
    return 0;
  }
  return ::DefWindowProcW(host_, message, wparam, lparam);
}

void WinMenus::AppendItems(HMENU menu, const MenuModel& model, std::vector<Command>& commands,
                           std::vector<Accelerator>* accelerators) {
  for (const MenuItem& item : model) {
    if (item.type == MenuItemType::kSeparator) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    const RoleInfo* role = FindRole(item.role);
    if (item.role != MenuRole::kNone && role == nullptr) {
      continue;  // Windows 没有的 role: 不显示
    }
    std::wstring text = !item.label.empty() ? Utf8ToWide(item.label) : role != nullptr ? role->title : L"";
    const UINT state = item.enabled ? MF_ENABLED : MF_GRAYED;
    if (item.type == MenuItemType::kSubmenu) {
      HMENU submenu = ::CreatePopupMenu();
      AppendItems(submenu, item.submenu, commands, accelerators);
      ::AppendMenuW(menu, MF_POPUP | state, reinterpret_cast<UINT_PTR>(submenu), text.c_str());
      continue;
    }
    std::optional<MenuAccelerator> accelerator = item.accelerator;
    if (!accelerator.has_value() && role != nullptr && role->key[0] != '\0') {
      accelerator = MenuAccelerator{role->key, platform::kModifierControl};
    }
    if (accelerator.has_value()) {
      text += L"\t" + AcceleratorText(*accelerator);
    }
    commands.push_back({item.id, item.role});
    const UINT command = static_cast<UINT>(commands.size());
    ::AppendMenuW(menu, MF_STRING | state | (item.checked ? MF_CHECKED : MF_UNCHECKED), command, text.c_str());
    if (item.type == MenuItemType::kRadio) {  // 选中标记画成圆点
      MENUITEMINFOW info{};
      info.cbSize = sizeof(info);
      info.fMask = MIIM_FTYPE;
      info.fType = MFT_STRING | MFT_RADIOCHECK;
      ::SetMenuItemInfoW(menu, command, FALSE, &info);
    }
    const bool intercept = item.enabled && (role == nullptr || !role->edit);
    if (accelerators != nullptr && accelerator.has_value() && intercept) {
      if (const UINT virtual_key = VirtualKeyFor(accelerator->key); virtual_key != 0) {
        accelerators->push_back({virtual_key, accelerator->modifiers & kAcceleratorModifiers, commands.back()});
      }
    }
  }
}

WinMenus::BuiltMenu WinMenus::BuildPopupMenu(const MenuModel& model) {
  BuiltMenu built;
  built.menu = ::CreatePopupMenu();
  AppendItems(built.menu, model, built.commands, nullptr);
  return built;
}

void WinMenus::Activate(const Command& command) {
  if (delegate_ == nullptr) return;
  if (command.role != MenuRole::kNone) {
    delegate_->OnMenuRoleActivated(command.role);
  } else {
    delegate_->OnMenuItemActivated(command.id);
  }
}

// ---- 应用菜单 ----

void WinMenus::SetApplicationMenu(const MenuModel* menu) {
  application_menu_ = menu != nullptr && !menu->empty() ? std::optional<MenuModel>(*menu) : std::nullopt;
  // 命令表与快捷键表: 与每个窗口的菜单栏按同一份描述、同样的顺序构建, 命令号一致。
  application_commands_.clear();
  accelerators_.clear();
  if (application_menu_.has_value()) {
    HMENU scratch = ::CreateMenu();
    AppendItems(scratch, *application_menu_, application_commands_, &accelerators_);
    ::DestroyMenu(scratch);
  }
  // 本线程的每个 momo 窗口换上新的菜单栏。
  ::EnumThreadWindows(
      ::GetCurrentThreadId(),
      [](HWND hwnd, LPARAM) -> BOOL {
        if (WinWindow* window = WinWindow::FromHwnd(hwnd)) window->UpdateMenuBar();
        return TRUE;
      },
      0);
}

HMENU WinMenus::CreateMenuBar() const {
  if (!application_menu_.has_value()) {
    return nullptr;
  }
  HMENU bar = ::CreateMenu();
  std::vector<Command> commands;  // 与 application_commands_ 相同
  AppendItems(bar, *application_menu_, commands, nullptr);
  return bar;
}

void WinMenus::OnMenuBarCommand(UINT command) {
  if (command >= 1 && command <= application_commands_.size()) {
    Activate(application_commands_[command - 1]);
  }
}

bool WinMenus::HandleKeyDown(UINT virtual_key, uint32_t modifiers, bool repeat) {
  for (const Accelerator& accelerator : accelerators_) {
    if (accelerator.virtual_key == virtual_key && accelerator.modifiers == (modifiers & kAcceleratorModifiers)) {
      if (!repeat) {
        const Command command = accelerator.command;  // 激活可能改掉菜单(accelerators_ 随之重建)
        Activate(command);
      }
      return true;
    }
  }
  return false;
}

// ---- 上下文菜单 ----

void WinMenus::Popup(const MenuModel& menu, PlatformWindow* window, std::optional<Point> position,
                     MoveOnlyFunction<void()> on_closed) {
  ClosePopup();
  PendingPopup popup;
  popup.built = BuildPopupMenu(menu);
  popup.owner = window != nullptr ? static_cast<HWND>(window->native_handle()) : host_;
  if (window != nullptr && position.has_value()) {
    const double dpr = window->device_pixel_ratio();
    popup.at = {static_cast<LONG>(std::lround(position->x * dpr)), static_cast<LONG>(std::lround(position->y * dpr))};
    ::ClientToScreen(popup.owner, &popup.at);
  } else {
    ::GetCursorPos(&popup.at);
  }
  popup.on_closed = std::move(on_closed);
  pending_popup_ = std::move(popup);
  // 菜单循环是系统的模态循环: 从窗口消息里进入, 不在 platform EventLoop 的任务里。
  ::PostMessageW(host_, kShowPopupMessage, 0, 0);
}

void WinMenus::ShowPendingPopup() {
  if (!pending_popup_.has_value()) {
    return;  // 弹出之前已被 ClosePopup 取消
  }
  PendingPopup popup = std::move(*pending_popup_);
  pending_popup_.reset();
  popup_open_ = true;
  TrackPopup(std::move(popup.built), ::IsWindow(popup.owner) ? popup.owner : host_, popup.at);
  popup_open_ = false;
  popup.on_closed();
}

void WinMenus::ClosePopup() {
  if (pending_popup_.has_value()) {  // 尚未弹出: 直接作废
    PendingPopup popup = std::move(*pending_popup_);
    pending_popup_.reset();
    ::DestroyMenu(popup.built.menu);
    popup.on_closed();
  }
  if (popup_open_) {
    ::EndMenu();  // TrackPopup 随后返回, ShowPendingPopup 报告关闭
  }
}

void WinMenus::TrackPopup(BuiltMenu built, HWND owner, POINT at) {
  // 拥有者是前台窗口时, 点击菜单之外才会关闭菜单;菜单关闭后补一条消息让前台切换生效(MSDN 的惯例)。
  ::SetForegroundWindow(owner);
  const UINT command = static_cast<UINT>(::TrackPopupMenuEx(
      built.menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, owner, nullptr));
  ::PostMessageW(owner, WM_NULL, 0, 0);
  ::DestroyMenu(built.menu);
  if (command >= 1 && command <= built.commands.size()) {
    Activate(built.commands[command - 1]);
  }
}

// ---- 托盘 ----

StatusOr<std::unique_ptr<PlatformTray>> WinMenus::CreateTray(int64_t tray_id, const std::string& image_path,
                                                             TrayDelegate* delegate) {
  auto tray = std::make_unique<Tray>(*this, tray_id, delegate);
  MOMO_RETURN_IF_ERROR(tray->SetImage(image_path));
  tray->Add();
  return std::unique_ptr<PlatformTray>(std::move(tray));
}

Rect WinMenus::ToLogicalRect(const RECT& physical) const {
  const Point top_left = displays_.ToLogical(POINT{physical.left, physical.top});
  const Point bottom_right = displays_.ToLogical(POINT{physical.right, physical.bottom});
  return {top_left.x, top_left.y, bottom_right.x - top_left.x, bottom_right.y - top_left.y};
}

}  // namespace momo::embedder::windows
