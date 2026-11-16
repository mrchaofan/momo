#pragma once

// NativeHost: 宿主能力表 MomoHost(extensions/sdk/include/momo/momo_host.h)的实现(docs/07)。
// 原生扩展是 Node 插件, 由 JS 加载;它们经 momo:services 的 nativeHost 拿到 table(), 用它投递平台任务、
// 创建外部纹理、注册平台视图类型。本类不知道有哪些插件, 也不加载任何东西。
//
// 关闭: Close()(platform 线程, 先于销毁平台视图与 JS 运行时)之后, 表里的每个函数返回 kMomoErrClosed,
// 已投递而未执行的平台任务被丢弃。表本身有意保留到进程结束(几十字节): 插件可能在任意线程上还握着指针。
//
// 线程约定: 创建与 Close 在 platform 线程;table() 的函数按 momo_host.h 的标注。

#include <memory>

#include "base/macros.h"
#include "base/task_runner.h"
#include "engine/common/platform_view_registry.h"
#include "engine/graphics/texture.h"
#include "momo/momo_host.h"

namespace momo::engine {

class NativeHost {
 public:
  NativeHost(std::shared_ptr<TaskRunner> platform_runner,
                std::shared_ptr<graphics::TextureRegistry> textures,
                std::shared_ptr<PlatformViewRegistry> platform_views);
  ~NativeHost();  // 隐式 Close

  const MomoHost* table() const;
  // 幂等。
  void Close();

  struct Table;  // 实现细节(native_host.cc)

 private:
  Table* const table_;  // 不释放, 见上

  MOMO_DISALLOW_COPY_AND_ASSIGN(NativeHost);
};

}  // namespace momo::engine
