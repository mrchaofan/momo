#pragma once

// WinVsyncDwm: 基于 DwmFlush 的 platform::VsyncSource(docs/03 §4)。仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
// 按需运行: 专用线程 "momo.vsync"(momo::Thread, 自带 EventLoop)。RequestFrameCallback 时若未在运行,
// 向该线程投递一个 Tick 任务;Tick 阻塞在 DwmFlush()(等到下一次 DWM 合成 ≈ vblank), 返回后若有待处理
// 请求, 就把帧时间 PostToObject 到 platform 线程, 由那里触发一次性 callback, 再投递下一个 Tick。某个 Tick
// 发现没有待处理请求时停止(不再投递)——无动画时该线程阻塞在 EventLoop 的等待中, 零唤醒。
//   - 时间戳: DwmGetCompositionTimingInfo 的 qpcVBlank / qpcRefreshPeriod(QPC)换算到 steady_clock;
//     失败时 frame_start = 现在, frame_target = 现在 + 刷新周期。
//   - 保护: DwmFlush 失败或"过早返回"(DWM 空闲时可能不阻塞)时, 补足睡眠到下一个刷新周期, 避免空转。
//   - 多显示器: DWM 以主合成节奏(通常为主显示器)为准, 不按窗口所在显示器切换。
//
// 线程与生命周期(vsync_source.h 契约):
//   - momo::Object, 归属 platform 线程(创建线程);RequestFrameCallback、callback 的调用与析构只在该线程
//     (回调在调用方线程执行)。vsync 线程从不调用 callback。
//   - 析构先在锁内置 destroyed, 再 Join vsync 线程(最多等待一个刷新周期);已投递未执行的通知因 WeakPtr
//     失效而作废——析构返回后绝不再回调。
//   - 不依赖窗口存活: DwmFlush/DwmGetCompositionTimingInfo 都是全局的, 不持有 HWND。

#include <memory>

#include "base/lock.h"
#include "base/status.h"
#include "base/thread.h"
#include "embedder/common/engine_platform.h"
#include "engine/platform/vsync_source.h"

namespace momo::embedder::windows {

class WinVsyncDwm final : public platform::VsyncSource {
 public:
  // 须在 platform 线程(有当前 EventLoop)调用。
  static StatusOr<ObjectPtr<platform::VsyncSource>> Create();

  ~WinVsyncDwm() override;

  void RequestFrameCallback(Callback callback) override;

  // 供 MakeObject 使用;请用 Create。
  WinVsyncDwm();

 private:
  // platform 线程与 vsync 线程共享的状态(字段受 lock 保护, last_tick 仅 vsync 线程)。
  struct Shared {
    Mutex lock;
    bool wanted = false;    // platform 线程有待触发的请求
    bool running = false;   // 已有 Tick 在 vsync 线程排队/执行
    bool destroyed = false; // 析构已开始
    WeakPtr<WinVsyncDwm> owner;  // 构造后不变
    TimePoint last_tick{};  // 仅 vsync 线程
  };

  // vsync 线程。shared 与 runner 的生命周期: 析构 Join 线程之后才释放 shared_。
  static void Tick(Shared* shared, TaskRunner* runner);
  // platform 线程: 触发待处理的 callback。
  void OnVsync(TimePoint frame_start, TimePoint frame_target);

  std::unique_ptr<Shared> shared_;
  Callback callback_;  // 仅 platform 线程
  std::unique_ptr<Thread> thread_;
};

}  // namespace momo::embedder::windows
