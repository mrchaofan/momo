#include "embedder/macos/mac_platform_views.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "base/logging.h"

using momo::engine::platform::CompositionLayout;
using momo::engine::platform::PlatformViewGeometry;

namespace {

// 物理像素矩形 → 宿主视图(flipped, 左上原点)的点矩形。
NSRect ToPoints(const momo::Rect& r, CGFloat scale) {
  return NSMakeRect(r.x / scale, r.y / scale, r.width / scale, r.height / scale);
}

std::optional<momo::Rect> VisibleRect(const PlatformViewGeometry& g) {
  std::optional<momo::Rect> visible = g.frame;
  for (const auto& clip : g.clips) {
    if (!visible) {
      break;
    }
    visible = visible->Intersect(clip.rect);
  }
  if (visible && (visible->width <= 0 || visible->height <= 0)) {
    return std::nullopt;
  }
  return visible;
}

// 作用在可见区域上的圆角: 取与可见区域重合(误差 1px 内)的祖先圆角裁剪中最大的半径。
// 多层不同圆角或圆角裁剪不贴合可见区域时只能近似(容器只有一个 cornerRadius)。
double CornerRadius(const PlatformViewGeometry& g, const momo::Rect& visible) {
  double radius = 0;
  for (const auto& clip : g.clips) {
    if (clip.radius <= 0) {
      continue;
    }
    if (std::abs(clip.rect.x - visible.x) <= 1 && std::abs(clip.rect.y - visible.y) <= 1 &&
        std::abs(clip.rect.right() - visible.right()) <= 1 &&
        std::abs(clip.rect.bottom() - visible.bottom()) <= 1) {
      radius = std::max(radius, clip.radius);
    }
  }
  return radius;
}

// sortSubviewsUsingFunction 的比较函数: context 为 view → 合成顺序(越大越靠上)。
NSComparisonResult CompareByRank(__kindof NSView* a, __kindof NSView* b, void* context) {
  const auto& rank = *static_cast<const std::map<NSView*, int>*>(context);
  auto ra = rank.find(a), rb = rank.find(b);
  const int x = ra != rank.end() ? ra->second : -1;
  const int y = rb != rank.end() ? rb->second : -1;
  return x < y ? NSOrderedAscending : (x > y ? NSOrderedDescending : NSOrderedSame);
}

}  // namespace

// ---------------------------------------------------------------------------

// 吞掉沿响应链冒泡上来的鼠标事件(NSView 的默认实现会转给 nextResponder, 最终漏到 momo 内容视图)。
#define MOMO_SWALLOW_MOUSE_EVENTS               \
  -(void)mouseDown : (NSEvent*)e {}             \
  -(void)mouseUp : (NSEvent*)e {}               \
  -(void)mouseDragged : (NSEvent*)e {}          \
  -(void)rightMouseDown : (NSEvent*)e {}        \
  -(void)rightMouseUp : (NSEvent*)e {}          \
  -(void)rightMouseDragged : (NSEvent*)e {}     \
  -(void)otherMouseDown : (NSEvent*)e {}        \
  -(void)otherMouseUp : (NSEvent*)e {}          \
  -(void)otherMouseDragged : (NSEvent*)e {}     \
  -(void)mouseMoved : (NSEvent*)e {}            \
  -(void)scrollWheel : (NSEvent*)e {}

@interface MomoPlatformViewContainer : NSView <MomoMouseEventTarget>
@property(nonatomic) int64_t viewId;
@property(nonatomic, strong) NSView* native;
- (void)setOccluders:(std::vector<NSRect>)occluders;
// point 为宿主视图坐标: 原生视图中被命中的子视图(派发器已决定由本容器接收)。
- (NSView*)nativeHitTest:(NSPoint)point;
@end

@interface MomoPlatformViewsHostView : NSView
@property(nonatomic, weak) MomoMouseEventDispatcher* dispatcher;
@end

@implementation MomoPlatformViewsHostView
- (BOOL)isFlipped {
  return YES;
}
// AppKit 投递派发器选中的事件时: 只有被选中的平台视图可命中(其他平台视图、宿主自身都不可命中,
// 事件落到内容视图)。非派发期间(拖放、光标等 AppKit 内部命中测试)按几何: 宿主自身不可命中。
- (NSView*)hitTest:(NSPoint)point {
  MomoMouseEventDispatcher* dispatcher = self.dispatcher;
  if (dispatcher != nil && dispatcher.routing) {
    id<MomoMouseEventTarget> target = dispatcher.routedTarget;
    if ([(NSObject*)target isKindOfClass:[MomoPlatformViewContainer class]] &&
        ((MomoPlatformViewContainer*)target).superview == self) {
      return [(MomoPlatformViewContainer*)target nativeHitTest:point];
    }
    return nil;
  }
  NSView* hit = [super hitTest:point];
  return hit == self ? nil : hit;
}
MOMO_SWALLOW_MOUSE_EVENTS
@end

@implementation MomoPlatformViewContainer {
  std::vector<NSRect> _occluders;  // 宿主视图坐标(点)
}
- (BOOL)isFlipped {
  return YES;
}
- (void)setOccluders:(std::vector<NSRect>)occluders {
  _occluders = std::move(occluders);
}
// 非派发期间的几何命中(拖放等): 被上层 momo 内容遮挡的位置不命中。point 为宿主坐标。
- (NSView*)hitTest:(NSPoint)point {
  if (self.hidden) {
    return nil;
  }
  for (const NSRect& r : _occluders) {
    if (NSPointInRect(point, r)) {
      return nil;
    }
  }
  return [super hitTest:point];
}
- (NSView*)nativeHitTest:(NSPoint)point {
  NSView* hit = [super hitTest:point];
  return (hit == nil || hit == self) ? self.native : hit;
}

// ---- MomoMouseEventTarget ----
// 认领: 指针位于可见区域且未被上层 momo 内容遮挡。投递交给 AppKit(原生视图自己处理)。
- (BOOL)momoHandleMouseEvent:(NSEvent*)event captured:(BOOL)captured {
  if (captured) {
    return YES;
  }
  NSView* host = self.superview;
  // 原生视图已被另一个窗口接管时, 本容器只是空壳。
  if (self.hidden || host == nil || self.native == nil || self.native.superview != self) {
    return NO;
  }
  const NSPoint p = [host convertPoint:event.locationInWindow fromView:nil];
  if (!NSPointInRect(p, self.frame)) {
    return NO;
  }
  for (const NSRect& r : _occluders) {
    if (NSPointInRect(p, r)) {
      return NO;
    }
  }
  return YES;
}
- (BOOL)momoDeliversViaAppKit {
  return YES;
}

MOMO_SWALLOW_MOUSE_EVENTS
@end

// ---------------------------------------------------------------------------

// overlay 视图以 CAMetalLayer 直接作为 backing layer(与 MTKView 相同): AppKit 不会往里画内容、
// 也不会翻转或替换它;图层的位置与尺寸随视图 frame 同步。不翻转坐标系(AppKit 对翻转视图的 backing
// layer 设置 geometryFlipped, 会影响 Metal 内容)。
@interface MomoOverlayView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
- (instancetype)initWithMetalLayer:(CAMetalLayer*)layer;
@end

@implementation MomoOverlayView
- (instancetype)initWithMetalLayer:(CAMetalLayer*)layer {
  if ((self = [super initWithFrame:NSZeroRect])) {
    _metalLayer = layer;
    self.wantsLayer = YES;  // 触发 makeBackingLayer
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
  }
  return self;
}
- (CALayer*)makeBackingLayer {
  return _metalLayer;
}
- (BOOL)wantsUpdateLayer {
  return YES;
}
- (void)updateLayer {}  // 内容只来自 Metal drawable
- (NSView*)hitTest:(NSPoint)point {
  return nil;  // 纯显示: overlay 上的 momo 内容由派发器经 occluders 判定归 momo
}
@end

// ---------------------------------------------------------------------------

@implementation MomoPlatformViews {
  NSView* __weak _content;
  MomoPlatformViewsHostView* _host;
  std::map<int64_t, MomoPlatformViewContainer*> _containers;
  std::vector<MomoOverlayView*> _overlays;  // 下标与 layout.overlays 对应, 复用
  std::vector<int64_t> _zOrder;             // 最近一次布局中可见视图的 id, 自下而上
  BOOL _warnedTransform;
}

- (MomoMouseEventDispatcher*)dispatcher {
  return _host.dispatcher;
}
- (void)setDispatcher:(MomoMouseEventDispatcher*)dispatcher {
  _host.dispatcher = dispatcher;
}

- (NSArray<id<MomoMouseEventTarget>>*)mouseTargetsTopDown {
  NSMutableArray<id<MomoMouseEventTarget>>* targets = [NSMutableArray array];
  for (auto it = _zOrder.rbegin(); it != _zOrder.rend(); ++it) {
    auto found = _containers.find(*it);
    if (found != _containers.end() && !found->second.hidden &&
        found->second.native.superview == found->second) {
      [targets addObject:found->second];
    }
  }
  return targets;
}

- (instancetype)initWithContentView:(NSView*)contentView {
  if ((self = [super init])) {
    _content = contentView;
    _host = [[MomoPlatformViewsHostView alloc] initWithFrame:contentView.bounds];
    _host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _host.wantsLayer = YES;
    _host.layer.zPosition = 1;  // 在主帧 CAMetalLayer 之上
    [contentView addSubview:_host];
  }
  return self;
}

- (void)dealloc {
  [_host removeFromSuperview];
}

- (BOOL)hasViews {
  return !_containers.empty();
}

- (CGFloat)scale {
  NSView* content = _content;
  const CGFloat scale = content.window ? content.window.backingScaleFactor : 1.0;
  return scale > 0 ? scale : 1.0;
}

- (void)addView:(NSView*)native viewId:(int64_t)viewId {
  if (native == nil || _containers.count(viewId) != 0) {
    return;
  }
  MomoPlatformViewContainer* container = [[MomoPlatformViewContainer alloc] initWithFrame:NSZeroRect];
  container.viewId = viewId;
  container.native = native;
  container.wantsLayer = YES;
  container.layer.masksToBounds = YES;
  container.hidden = YES;  // 由下一个合成帧决定几何与可见性
  // 原生视图此时不挂入: 多窗口下同一个平台视图对所有窗口登记, 哪个窗口的合成帧里出现它,
  // 就由那个窗口在 applyLayout 中接管(addSubview 会把它从原窗口移过来)。
  [_host addSubview:container];
  _containers[viewId] = container;
}

- (void)removeViewId:(int64_t)viewId {
  auto it = _containers.find(viewId);
  if (it == _containers.end()) {
    return;
  }
  MomoPlatformViewContainer* container = it->second;
  _containers.erase(it);
  [_host.dispatcher forgetTarget:container];
  // 焦点在被移除的视图里: 交还给内容视图(否则键盘事件无人接收)。
  [self resignFocusIfInside:container];
  if (container.native.superview == container) {
    [container.native removeFromSuperview];  // 只摘掉挂在本窗口的;已被其他窗口接管的不动
  }
  container.native = nil;
  [container removeFromSuperview];
}

// 被隐藏的平台视图不能继续持有键盘焦点(否则按键发给看不见的视图): 交还给内容视图。
- (void)resignFocusIfInside:(NSView*)container {
  NSView* content = _content;
  NSWindow* window = content.window;
  if (window != nil && [window.firstResponder isKindOfClass:[NSView class]] &&
      [(NSView*)window.firstResponder isDescendantOf:container]) {
    [window makeFirstResponder:content];
  }
}

- (void)focusViewId:(int64_t)viewId {
  auto it = _containers.find(viewId);
  NSView* content = _content;
  // 只有当前承载该原生视图的窗口响应(请求被广播给所有窗口)。
  if (it == _containers.end() || content.window == nil || it->second.native.superview != it->second) {
    return;
  }
  [content.window makeFirstResponder:it->second.native];
}

- (MomoOverlayView*)overlayAt:(size_t)index layer:(CAMetalLayer*)layer {
  if (_overlays.size() <= index) {
    _overlays.resize(index + 1, nil);
  }
  MomoOverlayView* view = _overlays[index];
  if (view == nil || view.metalLayer != layer) {
    // 每个 overlay 槽位的 CAMetalLayer 由 surface 复用, 通常只在首次使用时创建视图。
    [view removeFromSuperview];
    view = [[MomoOverlayView alloc] initWithMetalLayer:layer];
    view.hidden = YES;
    [_host addSubview:view];
    _overlays[index] = view;
  }
  return view;
}

- (void)applyLayout:(const CompositionLayout&)layout
      overlayLayers:(const std::vector<CAMetalLayer*>&)overlayLayers {
  const CGFloat scale = [self scale];
  CGFloat z = 1;  // 合成顺序(base → view0 → overlay0 → view1 …), 也用于子视图排序
  std::map<NSView*, int> rank;
  std::map<int64_t, bool> shown;
  std::vector<bool> overlay_shown(_overlays.size(), false);
  _zOrder.clear();

  for (size_t i = 0; i < layout.views.size(); ++i) {
    const PlatformViewGeometry& g = layout.views[i];
    auto it = _containers.find(g.view_id);
    if (it != _containers.end()) {
      MomoPlatformViewContainer* container = it->second;
      const std::optional<momo::Rect> visible = VisibleRect(g);
      if (visible && container.native.superview != container) {
        // 接管原生视图(可能正挂在另一个窗口里): 若它持有键盘焦点, 先交还给原窗口的内容视图。
        NSView* native = container.native;
        NSWindow* old_window = native.window;
        if (old_window != nil && [old_window.firstResponder isKindOfClass:[NSView class]] &&
            [(NSView*)old_window.firstResponder isDescendantOf:native]) {
          [old_window makeFirstResponder:old_window.contentView];
        }
        native.autoresizingMask = NSViewNotSizable;
        [container addSubview:native];
        MOMO_LOG(VERBOSE) << "Platform view " << g.view_id << " attached to window (frame "
                          << g.frame.x << "," << g.frame.y << " " << g.frame.width << "x"
                          << g.frame.height << " px).";
      }
      if (visible) {
        if (!g.axis_aligned && !_warnedTransform) {
          _warnedTransform = YES;
          MOMO_LOG(WARNING) << "Platform view " << g.view_id
                            << ": rotation/skew is not supported; using the bounding box.";
        }
        container.frame = ToPoints(*visible, scale);
        container.native.frame = ToPoints(g.frame.Offset(-visible->x, -visible->y), scale);
        container.alphaValue = static_cast<CGFloat>(std::clamp(g.opacity, 0.0, 1.0));
        container.layer.cornerRadius = static_cast<CGFloat>(CornerRadius(g, *visible) / scale);
        rank[container] = static_cast<int>(z);
        container.layer.zPosition = z++;
        std::vector<NSRect> occluders;
        occluders.reserve(g.occluders.size());
        for (const momo::Rect& r : g.occluders) {
          occluders.push_back(ToPoints(r, scale));
        }
        [container setOccluders:std::move(occluders)];
        container.hidden = NO;
        shown[g.view_id] = true;
        _zOrder.push_back(g.view_id);
      }
    }
    // overlays[i] 位于 views[i] 之上(即使视图本身不可见, overlay 仍承载其后的内容)。
    CAMetalLayer* layer = i < overlayLayers.size() ? overlayLayers[i] : nil;
    if (i < layout.overlays.size() && layout.overlays[i] && layer != nil) {
      MomoOverlayView* overlay = [self overlayAt:i layer:layer];
      overlay.frame = ToPoints(*layout.overlays[i], scale);
      layer.contentsScale = scale;
      rank[overlay] = static_cast<int>(z);
      overlay.layer.zPosition = z++;
      overlay.hidden = NO;
      if (i >= overlay_shown.size()) {
        overlay_shown.resize(i + 1, false);
      }
      overlay_shown[i] = true;
    }
  }
  // 层级以子视图顺序为准(AppKit 不按 zPosition 排 backing layer)。sortSubviews 原地重排,
  // 不移出/重新加入子视图, 原生视图的第一响应者不受影响。只在顺序变化时排序。
  if (!rank.empty()) {
    NSArray<NSView*>* subviews = _host.subviews;
    bool sorted = true;
    int last = -2;
    for (NSView* v : subviews) {
      auto it = rank.find(v);
      const int r = it != rank.end() ? it->second : -1;
      if (r < last) {
        sorted = false;
        break;
      }
      last = r;
    }
    if (!sorted) {
      [_host sortSubviewsUsingFunction:&CompareByRank context:&rank];
    }
  }
  for (auto& [id, container] : _containers) {
    if (!shown.count(id) && !container.hidden) {
      [self resignFocusIfInside:container];
      container.hidden = YES;
    }
  }
  for (size_t i = 0; i < _overlays.size(); ++i) {
    if (_overlays[i] != nil && (i >= overlay_shown.size() || !overlay_shown[i])) {
      _overlays[i].hidden = YES;
    }
  }
}

- (void)hideAll {
  _zOrder.clear();
  for (auto& [id, container] : _containers) {
    [self resignFocusIfInside:container];
    container.hidden = YES;
  }
  for (MomoOverlayView* overlay : _overlays) {
    overlay.hidden = YES;
  }
}

- (BOOL)platformViewAtContentPoint:(NSPoint)point {
  NSView* content = _content;
  if (_containers.empty() || content == nil) {
    return NO;
  }
  // hitTest: 的参数是接收者父视图(内容视图)的坐标。
  return [_host hitTest:point] != nil;
}

- (int64_t)viewIdForResponder:(NSResponder*)responder {
  if (![responder isKindOfClass:[NSView class]]) {
    return 0;
  }
  for (NSView* v = (NSView*)responder; v != nil; v = v.superview) {
    if ([v isKindOfClass:[MomoPlatformViewContainer class]]) {
      return ((MomoPlatformViewContainer*)v).viewId;
    }
  }
  return 0;
}

@end
