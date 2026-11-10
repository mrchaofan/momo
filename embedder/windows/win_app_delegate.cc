// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_app_delegate.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <utility>

#include "base/logging.h"

namespace momo::embedder {

namespace {

// WM_COPYDATA 的 dwData 标记("MOMO"), 区分其他来源的 WM_COPYDATA。
constexpr ULONG_PTR kCopyDataMagic = 0x4D4F4D4F;
// 转发命令行的上限(字节), 防御畸形消息。
constexpr DWORD kMaxCopyDataBytes = 64 * 1024;
// 首个实例可能还在启动(已持有互斥量但接收窗口未建好): 后续实例重试查找的次数与间隔。
constexpr int kFindReceiverRetries = 20;
constexpr DWORD kFindReceiverIntervalMs = 50;
constexpr UINT kSendTimeoutMs = 3000;

std::string WideToUtf8(const wchar_t* wide) {
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) {
    return {};
  }
  std::string out(static_cast<size_t>(len - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
  return out;
}

// 完整命令行(含 argv[0])→ UTF-8 参数(不含 argv[0])。
std::vector<std::string> ParseCommandLine(const wchar_t* command_line) {
  std::vector<std::string> args;
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(command_line, &argc);
  if (argv == nullptr) {
    return args;
  }
  for (int i = 1; i < argc; ++i) {
    args.push_back(WideToUtf8(argv[i]));
  }
  ::LocalFree(argv);
  return args;
}

std::wstring ReceiverClassName(const std::wstring& app_id) {
  return L"MomoSingleInstance." + app_id;
}

}  // namespace

struct WinAppDelegate::Receiver {
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_COPYDATA) {
      auto* self = reinterpret_cast<WinAppDelegate*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      if (self == nullptr || data == nullptr || data->dwData != kCopyDataMagic ||
          data->lpData == nullptr || data->cbData < sizeof(wchar_t) ||
          data->cbData % sizeof(wchar_t) != 0 || data->cbData > kMaxCopyDataBytes) {
        return FALSE;
      }
      // 数据只在本次消息处理期间有效: 先拷贝, 且不信任结尾 NUL。
      std::wstring command_line(static_cast<const wchar_t*>(data->lpData),
                                data->cbData / sizeof(wchar_t));
      while (!command_line.empty() && command_line.back() == L'\0') {
        command_line.pop_back();
      }
      std::vector<std::string> args = ParseCommandLine(command_line.c_str());
      MOMO_LOG(INFO) << "Second instance forwarded " << args.size() << " argument(s)";
      if (self->second_instance_handler_) {
        self->second_instance_handler_(std::move(args));
      }
      return TRUE;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }
};

WinAppDelegate::~WinAppDelegate() {
  if (receiver_window_ != nullptr) {
    HWND hwnd = static_cast<HWND>(receiver_window_);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd);
    receiver_window_ = nullptr;
  }
  if (instance_mutex_ != nullptr) {
    ::ReleaseMutex(static_cast<HANDLE>(instance_mutex_));
    ::CloseHandle(static_cast<HANDLE>(instance_mutex_));
  }
}

void WinAppDelegate::EnablePerMonitorDpiAwareness() {
  if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    // ERROR_ACCESS_DENIED: 已由清单设置(正常)。其他失败(V2 不受支持的旧系统): 退回 V1。
    if (::GetLastError() != ERROR_ACCESS_DENIED) {
      ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    }
  }
}

bool WinAppDelegate::AcquireSingleInstance(const std::wstring& app_id) {
  const std::wstring name = L"Local\\momo." + app_id;
  const std::wstring class_name = ReceiverClassName(app_id);
  HANDLE mutex = ::CreateMutexW(nullptr, TRUE, name.c_str());
  if (mutex == nullptr) {
    return true;  // 无法判断时不阻止启动
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    ::CloseHandle(mutex);
    // 把命令行转发给已有实例(由它激活自己的窗口)。
    HWND target = nullptr;
    for (int i = 0; i < kFindReceiverRetries && target == nullptr; ++i) {
      target = ::FindWindowExW(HWND_MESSAGE, nullptr, class_name.c_str(), nullptr);
      if (target == nullptr) {
        ::Sleep(kFindReceiverIntervalMs);
      }
    }
    if (target == nullptr) {
      MOMO_LOG(WARNING) << "Another instance is running but its receiver window was not found";
      return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(target, &pid);
    if (pid != 0) {
      ::AllowSetForegroundWindow(pid);  // 本进程刚由用户启动, 拥有前台权限: 让渡给已有实例
    }
    const wchar_t* command_line = ::GetCommandLineW();
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataMagic;
    data.cbData = static_cast<DWORD>((std::wcslen(command_line) + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(command_line);
    DWORD_PTR result = 0;
    if (data.cbData <= kMaxCopyDataBytes &&
        ::SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs, &result) == 0) {
      MOMO_LOG(WARNING) << "Forwarding the command line to the running instance failed: "
                        << ::GetLastError();
    }
    return false;
  }
  instance_mutex_ = mutex;

  // 接收后续实例转发的命令行(消息窗口: 不可见、不参与枚举, 但可被 FindWindowEx(HWND_MESSAGE) 找到)。
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &Receiver::WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = class_name.c_str();
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    MOMO_LOG(WARNING) << "RegisterClassExW(single instance) failed: " << ::GetLastError();
    return true;
  }
  receiver_window_ = ::CreateWindowExW(0, class_name.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                       nullptr, instance, this);
  if (receiver_window_ == nullptr) {
    MOMO_LOG(WARNING) << "CreateWindowExW(single instance) failed: " << ::GetLastError();
  }
  return true;
}

void WinAppDelegate::ActivateWindow(void* handle) {
  HWND hwnd = static_cast<HWND>(handle);
  if (hwnd == nullptr || !::IsWindow(hwnd)) {
    return;
  }
  ::ShowWindow(hwnd, ::IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(hwnd);
}

std::vector<std::string> WinAppDelegate::CommandLineArgsUtf8() {
  return ParseCommandLine(::GetCommandLineW());
}

std::filesystem::path WinAppDelegate::ExecutableDir() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) {
      return {};
    }
    if (len < buffer.size()) {
      buffer.resize(len);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::filesystem::path(buffer).parent_path();
}

}  // namespace momo::embedder
