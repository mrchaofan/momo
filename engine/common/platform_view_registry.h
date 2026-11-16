#pragma once

// PlatformViewRegistry: 平台视图的类型(工厂)与实例表(docs/05 §9、docs/07 §3)。
//   - 工厂: 扩展经宿主表的 platform_view_register_factory 注册(任意线程), 按视图类型名("acme.webview")索引;
//     一经注册到 Clear 为止都有效。
//   - 实例: JS 经 momo/platform_views 通道请求 create/dispose, Shell 在 platform 线程调用本表。
// 原生视图(NSView* / HWND)由工厂创建并归扩展所有;本表只借用指针, 直到 dispose 回调为止。
// 线程约定: RegisterFactory 线程安全;其余方法只在 platform 线程调用, 工厂回调也在该线程执行
// (原生视图只能在主线程创建/销毁)。
// 回调不在内部状态修改的中途执行: 先更新表, 再调用工厂(工厂可重入地查询本表)。

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/lock.h"
#include "base/macros.h"
#include "base/status.h"

namespace momo::engine {

class PlatformViewRegistry {
 public:
  struct Factory {
    // 创建原生视图;返回空表示失败。
    std::function<void*(int64_t view_id, const std::vector<uint8_t>& params)> create;
    // 销毁(释放)create 返回的原生视图。
    std::function<void(int64_t view_id, void* native_view)> dispose;
  };

  struct View {
    int64_t id = 0;
    std::string type;
    void* native = nullptr;
  };

  // 在原生视图交给工厂 dispose 之前调用(embedder 先把它移出窗口)。
  using BeforeDispose = std::function<void(int64_t view_id)>;

  PlatformViewRegistry() = default;

  // 类型名非空且未被注册。任意线程。
  Status RegisterFactory(const std::string& type, Factory factory);

  // 创建视图:id 由 JS 侧分配, 必须未被占用;成功返回原生视图。
  StatusOr<void*> Create(int64_t view_id, const std::string& type,
                         const std::vector<uint8_t>& params);
  // 销毁视图: 先从表中移除, 再 before(id), 最后工厂 dispose。未知 id 返回 NotFound。
  Status Dispose(int64_t view_id, const BeforeDispose& before = nullptr);
  // 引擎关闭: 销毁全部视图并清空工厂(此后不再调用任何工厂回调, 也不能再创建视图)。
  void Clear(const BeforeDispose& before = nullptr);

  // 未知 id 返回空。
  void* Get(int64_t view_id) const;
  std::vector<View> views() const;
  size_t size() const { return views_.size(); }

 private:
  struct Entry {
    View view;
    std::function<void(int64_t view_id, void* native_view)> dispose;
  };
  void DisposeEntry(Entry entry, const BeforeDispose& before);

  mutable Mutex factories_mutex_;
  std::map<std::string, Factory> factories_;  // factories_mutex_ 保护
  std::map<int64_t, Entry> views_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(PlatformViewRegistry);
};

}  // namespace momo::engine
