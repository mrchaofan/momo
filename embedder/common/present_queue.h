#pragma once

// PresentQueue: 一个窗口里, raster 线程产出的帧与 platform 线程之间的同步点。由窗口创建, 交给它的 surface;
// 四种 surface(Metal / Direct3D / 两个平台的软件 surface)都经它上报呈现, 窗口不必知道当前是哪一种。
//
// 帧的呈现分两种:
//   - 必须在 platform 线程完成的(软件帧设为窗口内容;要与窗口尺寸、平台视图几何同一事务提交的 GPU 帧):
//     raster 线程 Post(size, present), platform 线程按入队顺序执行。平常由 wake 触发(构造时给出: 让
//     platform 线程稍后调用 Drain, 如 GCD 主队列 / PostMessage);已有一次唤醒在途时不重复唤醒, 所以
//     raster 比 platform 线程快时唤醒不会堆积。
//   - 在 raster 线程就完成的(交换链 Present、命令缓冲 presentDrawable): 呈现后 NotePresented(size)。
//
// 拖拽缩放同步(同 Flutter 的 ResizeSynchronizer): 窗口尺寸在 platform 线程的系统回调里变化, 帧却是异步到的
// ——不同步的话, 新尺寸的窗口里是被拉伸的旧帧, 直到松手。窗口在尺寸变化的回调里把新尺寸交给引擎之后调用
// WaitForFrame: platform 线程就地等到这个尺寸的帧呈现(入队的呈现在等待中直接执行, 不必回到事件循环),
// 回调返回后系统才显示新尺寸, 窗口与内容同时变化。等待有上限: 应用没及时出帧就暂时拉伸旧内容。
//
// 线程约定: Post / NotePresented 在 raster 线程;Drain / WaitForFrame 在 platform 线程;查询任意线程。
// 生命周期: 以 std::shared_ptr 持有(Create)。窗口、surface、在途的呈现闭包与唤醒各持一份, 窗口先销毁也安全。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "base/blocking_queue.h"
#include "base/geometry.h"
#include "base/macros.h"
#include "base/time.h"

namespace momo::embedder {

// 拖拽缩放时等一帧的上限: 应用在此之内出帧则缩放无拉伸, 否则暂时拉伸旧内容(窗口拖动保持跟手)。
inline constexpr TimeDelta kLiveResizeFrameWait = std::chrono::milliseconds(100);

class PresentQueue final : public std::enable_shared_from_this<PresentQueue> {
 public:
  // 任意线程调用: 让 platform 线程稍后调用 queue->Drain()。
  using Wake = std::function<void(std::shared_ptr<PresentQueue> queue)>;

  static std::shared_ptr<PresentQueue> Create(Wake wake);

  // raster 线程: 一帧 size 尺寸的帧要在 platform 线程呈现。
  void Post(Size size, std::function<void()> present);
  // raster 线程: 一帧 size 尺寸的帧已在本线程呈现。
  void NotePresented(Size size);

  // platform 线程: 按序执行已入队的呈现。
  void Drain();
  // platform 线程: 等到一帧 size 尺寸的帧呈现(见文件头"拖拽缩放同步")。等待期间反复调用 pump 执行平台线程
  // 任务——引擎出帧要经过它们(vsync 请求与回调、平台视图呈现), 不执行会互等。超时返回 false。
  // pump 返回 false 表示调用方已在其中被销毁: 立即返回 false。
  bool WaitForFrame(Size size, TimeDelta timeout, const std::function<bool()>& pump);

  // platform 线程: 窗口尺寸在系统回调里变成了 size 时调用(拖拽缩放同步的全部步骤, 两个平台的窗口共用):
  // 调用 notify_metrics 把新尺寸交给引擎, 再就地等这个尺寸的帧呈现, 上限 kLiveResizeFrameWait;等待期间
  // 执行本线程 EventLoop 的到期任务(只执行任务, 不析构对象)。还没出过帧(等待只会超时)、或正处在另一次等待
  // 里(等待期间执行的任务又改了窗口尺寸, 由外层的等待覆盖)时只通知, 不等待。
  void SynchronizeResize(Size size, const std::function<void()>& notify_metrics);

  // 已入队、尚未执行完的帧数(raster 线程据此保持两种呈现方式之间的帧顺序)。
  int pending() const { return pending_.load(std::memory_order_acquire); }
  // 是否已呈现过帧(还没有时等待只会超时)。
  bool has_presented() const { return presented_size_.load(std::memory_order_acquire) != 0; }

 private:
  struct Entry {
    uint64_t size;  // PackSize
    std::function<void()> present;
  };

  explicit PresentQueue(Wake wake);
  // 物理像素尺寸(取整)压成一个可原子读写的值;0 = 没有。
  static uint64_t PackSize(Size size);
  void Run(Entry& entry);
  // platform 线程: 执行已入队的帧;返回其中是否有 target 尺寸的帧。
  bool RunQueued(uint64_t target);

  const Wake wake_;
  BlockingQueue<Entry> queue_;
  std::atomic<bool> wake_pending_{false};
  std::atomic<int> pending_{0};
  std::atomic<uint64_t> presented_size_{0};  // 最近一次呈现的帧尺寸
  bool resize_waiting_ = false;              // SynchronizeResize 正在等待(仅 platform 线程)

  MOMO_DISALLOW_COPY_AND_ASSIGN(PresentQueue);
};

}  // namespace momo::embedder
