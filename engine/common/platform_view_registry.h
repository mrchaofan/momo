#pragma once

// PlatformViewRegistry: 平台视图的类型(工厂)与实例表(docs/05 §9、docs/07 §3)。
//   - 工厂: 扩展经 C ABI platform_view_register_factory 注册, 按视图类型名("acme.webview")索引。
//   - 实例: JS 经 momo/platform_views 通道请求 create/dispose, Shell 在 platform 线程调用本表。
// 原生视图(NSView* / HWND)由工厂创建并归扩展所有;本表只借用指针, 直到 dispose 回调为止。
// 线程约定: 全部方法只在 platform 线程调用, 工厂回调也在该线程执行(原生视图只能在主线程创建/销毁)。
// 回调不在内部状态修改的中途执行: 先更新表, 再调用工厂(工厂可重入地查询本表)。

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/status.h"

namespace momo::engine {

class PlatformViewRegistry {
 public:
  struct Factory {
    // 创建原生视图;返回空表示失败(error 写入失败原因, 可留空)。
    std::function<void*(int64_t view_id, const std::vector<uint8_t>& params, std::string* error)>
        create;
    // 销毁(释放)create 返回的原生视图。
    std::function<void(int64_t view_id, void* native_view)> dispose;
    std::string owner;  // 注册方(扩展 id), 诊断与按扩展清理用
  };

  struct View {
    int64_t id = 0;
    std::string type;
    void* native = nullptr;
  };

  // 在原生视图交给工厂 dispose 之前调用(embedder 先把它移出窗口)。
  using BeforeDispose = std::function<void(int64_t view_id)>;

  PlatformViewRegistry() = default;

  // 类型名非空且未被注册。
  Status RegisterFactory(const std::string& type, Factory factory);
  bool HasFactory(const std::string& type) const { return factories_.count(type) != 0; }
  // 注销某个注册方的全部工厂(其已创建的视图须先 dispose, 见 DisposeViewsOf)。
  void UnregisterFactoriesOf(const std::string& owner);

  // 创建视图:id 由 JS 侧分配, 必须未被占用;成功返回原生视图。
  StatusOr<void*> Create(int64_t view_id, const std::string& type,
                         const std::vector<uint8_t>& params);
  // 销毁视图: 先从表中移除, 再 before(id), 最后工厂 dispose。未知 id 返回 NotFound。
  Status Dispose(int64_t view_id, const BeforeDispose& before = nullptr);
  // 销毁全部视图(热重启、关闭)。
  void DisposeAll(const BeforeDispose& before = nullptr);
  // 销毁某个注册方的工厂创建的全部视图(扩展 detach 前)。
  void DisposeViewsOf(const std::string& owner, const BeforeDispose& before = nullptr);

  // 未知 id 返回空。
  void* Get(int64_t view_id) const;
  std::vector<View> views() const;
  size_t size() const { return views_.size(); }

 private:
  struct Entry {
    View view;
    Factory factory;  // 拷贝: 工厂注销后仍能 dispose 已创建的视图
  };
  void DisposeEntry(Entry entry, const BeforeDispose& before);

  std::map<std::string, Factory> factories_;
  std::map<int64_t, Entry> views_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(PlatformViewRegistry);
};

}  // namespace momo::engine
