#pragma once

// WinPlatformViews: 平台视图在 Windows 窗口中的宿主(ARCHITECTURE.md §4.10;对应 macOS 的 MomoPlatformViews)。
// 仅被 embedder/windows 的 .cc 包含。
//
// 视图: 扩展创建的 MomoWinPlatformView(momo_host.h), 内容是 DirectComposition visual(WebView2 的 visual hosting)。
// 每个视图在每个窗口里有一个容器 visual(位置 + 圆角裁剪 + 透明度)和其下的挂载点 visual(交给视图挂内容)。
//
// 两条线程, 分工同 macOS 的"画面与几何同一事务生效":
//   - raster 线程(Compose, 在 WinCompositor::Update 内): 按合成布局排列 visual——主帧、视图容器、overlay 的 z 序、
//     容器的位置/裁剪/透明度——与交换链的 Present 同一次提交, 原生视图与 momo 画面同帧。
//   - platform 线程(OnPresented, 由呈现队列在帧提交后执行): 视图的挂入(哪个窗口的合成帧里出现它, 就由那个窗口
//     接管: attach 到本窗口)、内容尺寸、键盘焦点规则、命中几何。视图的内容尺寸因此比位置晚一次线程跳转生效。
// 输入: 这里只提供几何与投递。ViewAt 回答"用户此刻看到的指针下是什么"(最近一次呈现的帧), 窗口据此作为
//   PointerWindow 回答 PointerDispatcher 的命中;派发器决定交给某个视图后, 窗口经 SendMouse 交给它。
// 焦点(规则同 macOS): 框架的焦点所在的平台视图只要显示在某个窗口里, 就持有键盘焦点。
// 线程约定: 除 Compose / Disable 外都在 platform 线程。

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_compositor.h"
#include "embedder/windows/win_util.h"
#include "engine/platform/platform_view.h"
#include "momo/momo_host.h"

namespace momo::embedder::windows {

class WinPlatformViews final {
 public:
  // 窗口一侧(platform 线程回调)。
  class Delegate {
   public:
    // 用户直接操作使视图取得了键盘焦点。
    virtual void OnPlatformViewFocused(int64_t view_id) = 0;
    virtual void OnPlatformViewCursorChanged(int64_t view_id) = 0;
    // 视图持有键盘焦点时的按下: 返回是否已由应用菜单的快捷键处理。
    virtual bool OnPlatformViewKeyDown(UINT message, WPARAM wparam, LPARAM lparam) = 0;
    // 已呈现的几何变了: 重新判定指针下的接收方(指针不动而画面变化)。
    virtual void OnPlatformViewGeometryChanged() = 0;

   protected:
    ~Delegate() = default;
  };

  WinPlatformViews(HWND hwnd, std::shared_ptr<WinCompositor> compositor, Delegate* delegate);
  ~WinPlatformViews();

  // ---- platform 线程 ----
  // 视图登记(初始不显示, 也不挂入: 由合成帧决定它出现在哪个窗口)/ 移出。
  void Add(int64_t view_id, const MomoWinPlatformView* view);
  void Remove(int64_t view_id);
  void Focus(int64_t view_id);
  void Blur(int64_t view_id);
  // 一帧合成画面已提交: 挂入、尺寸、焦点、命中几何随之生效。
  void OnPresented(const platform::CompositionLayout& layout);
  // client(客户区物理像素)处接收指针的平台视图;0 = momo 内容。
  int64_t ViewAt(POINT client) const;
  // 鼠标消息交给视图(坐标换算为视图内容坐标)。
  void SendMouse(int64_t view_id, UINT message, WPARAM wparam, POINT client);
  // 指针所在的视图变了(0 = 不在视图上): 离开的视图收到 WM_MOUSELEAVE。
  void SetHovered(int64_t view_id);
  // 视图的光标;视图不在本窗口时为空。
  HCURSOR Cursor(int64_t view_id) const;

  // ---- raster 线程 ----
  // GPU 呈现已停用(软件回退): 平台视图不再显示, 也不再接收指针。
  void Disable() { disabled_.store(true, std::memory_order_release); }
  // 在 WinCompositor::Update 内: root 之下依次排列 base、视图容器与 overlay(overlays 与 layout.overlays 等长,
  // 元素可为空), 并设置容器的几何。
  void Compose(const platform::CompositionLayout& layout, IDCompositionVisual2* base,
               const std::vector<IDCompositionVisual2*>& overlays);

 private:
  struct Entry;

  bool Hosts(int64_t view_id) const;
  Entry* Find(int64_t view_id) const;
  void Attach(Entry& entry);
  // view_id 持有系统键盘焦点时交还给窗口。
  void ResignFocus(int64_t view_id);

  static void SiteCommit(void* context);
  static void SiteFocused(void* context);
  static void SiteCursorChanged(void* context);
  static bool SiteKeyDown(void* context, uint32_t message, uintptr_t wparam, intptr_t lparam);

  const HWND hwnd_;
  const std::shared_ptr<WinCompositor> compositor_;
  Delegate* const delegate_;

  // 增删在 compositor 的锁内(raster 的 Compose 读它们的 visual);其余字段只在 platform 线程。
  std::map<int64_t, std::unique_ptr<Entry>> entries_;
  // raster 线程(compositor 的锁内): root 当前的子 visual, 自下而上。
  std::vector<IDCompositionVisual2*> order_;

  // platform 线程
  platform::CompositionLayout presented_;  // 最近一次呈现的帧
  int64_t hovered_ = 0;
  int64_t system_focus_ = 0;  // 本窗口里持有系统键盘焦点的视图(经 focus 交给它, 或用户点击它)
  std::atomic<bool> disabled_{false};

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinPlatformViews);
};

}  // namespace momo::embedder::windows
