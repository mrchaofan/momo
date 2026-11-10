#pragma once

// Windows 平台实现公共小工具(仅被 embedder/windows 的 .cc 包含)。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
//  - 统一以 UNICODE + NOMINMAX + WIN32_LEAN_AND_MEAN 方式包含 <windows.h>(MSVC 构建已由
//    cmake/momo_options.cmake 定义, 这里再兜底一次, 便于 MinGW/clang 等工具链)。
//  - <windows.h> 把 CreateWindow 定义成宏(CreateWindowW), 会改写 PlatformIntegration::CreateWindow
//    的成员名: 包含后立即 #undef。本层一律显式调用 W 版 API(CreateWindowExW 等)。
//  - UTF-8 ↔ UTF-16 转换、单调时钟时间戳。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#ifdef CreateWindow
#undef CreateWindow
#endif

#include <cstdint>
#include <string>
#include <string_view>

namespace momo::embedder::windows {

// 非法 UTF-8 序列按 U+FFFD 替换(MultiByteToWideChar 默认行为)。
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

// 输入事件时间戳(微秒, steady_clock 纪元)。与 mac 的 NSEvent.timestamp 一样只用于相对比较。
int64_t NowMicros();

}  // namespace momo::embedder::windows
