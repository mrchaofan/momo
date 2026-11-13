#include "embedder/macos/mac_platform_views.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "base/logging.h"
#include "embedder/macos/mac_platform_view_input.h"

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

// 平台视图的几何包装(同 Flutter 的 FlutterMutatorView): 可见区域裁剪、圆角、透明度、z 序。
// 不处理输入——谁接收由框架决定, 投递由窗口的 MomoPlatformViewInputRouter 完成。
@interface MomoPlatformViewContainer : NSView
@property(nonatomic) int64_t viewId;
@property(nonatomic, strong) NSView* native;
- (void)setOccluders:(std::vector<NSRect>)occluders;
// point 为宿主视图坐标: 是否落在可见区域内、且之上没有 momo 内容(occluders)。
- (BOOL)receivesHostPoint:(NSPoint)point;
// 原生视图当前是否挂在本容器里(否则已被其他窗口接管, 本容器只是空壳)。
- (BOOL)hostsNative;
// point 为宿主视图坐标: 原生视图子树中被命中的视图(不考虑遮挡: 接收者已由框架决定)。
- (NSView*)nativeHitTest:(NSPoint)point;
// tracking area 是否属于本容器中的原生视图(子树中任一视图的跟踪区)。
- (BOOL)ownsTrackingArea:(NSTrackingArea*)area;
@end

@interface MomoPlatformViewsHostView : NSView
@property(nonatomic, weak) MomoPlatformViews* owner;
@end

@implementation MomoPlatformViewsHostView
- (BOOL)isFlipped {
  return YES;
}
// 窗口把框架决定交给某个平台视图的事件经 AppKit 投递时(MomoMouseRouting momoRoutedPlatformView): 命中测试
// 只落在那个平台视图的原生视图子树里(其他平台视图、overlay 与宿主都不可命中)。其他时候(拖放目标、光标等
// AppKit 内部命中测试)按几何: 宿主自身不可命中。原始鼠标事件不经 AppKit 投递(只交给 momo), 不会走到这里。
- (NSView*)hitTest:(NSPoint)point {
  id window = self.window;
  if ([window conformsToProtocol:@protocol(MomoMouseRouting)]) {
    const int64_t routed = [(id<MomoMouseRouting>)window momoRoutedPlatformView];
    if (routed != 0) {
      MomoPlatformViews* owner = self.owner;
      // point 为父视图(内容视图)坐标。
      return [owner nativeHitTestForPlatformView:routed
                                     windowPoint:[self.superview convertPoint:point toView:nil]];
    }
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
// 不经派发器投递时的 AppKit 几何命中(光标归属、拖放目标等"指针下是哪个视图"的查询)。point 为宿主坐标。
// 宿主掌握指针时以派发器的判定为准: 指针不归本平台视图(被 momo 内容盖住、已离开)就不可命中。
// WKWebView 据此丢弃迟到的光标设置——WebKit 在 [NSCursor set] 之前用内容视图的 hitTest: 确认指针下是
// 它自己的子树(PageClientImpl::setCursor), 光标因此也只有一个裁决者。拖放会话等期间宿主收不到鼠标事件,
// 退回按画面上的遮挡(occluders: 本视图之上 momo 内容的可命中区域)近似。
- (NSView*)hitTest:(NSPoint)point {
  if (self.hidden) {
    return nil;
  }
  MomoPlatformViewsHostView* host = (MomoPlatformViewsHostView*)self.superview;
  MomoPlatformViews* owner = [host isKindOfClass:[MomoPlatformViewsHostView class]] ? host.owner : nil;
  const int64_t pointerOwner = owner.pointerOwner != nil ? owner.pointerOwner() : -1;
  if (pointerOwner >= 0) {
    return pointerOwner == self.viewId ? [super hitTest:point] : nil;
  }
  for (const NSRect& r : _occluders) {
    if (NSPointInRect(point, r)) {
      return nil;
    }
  }
  return [super hitTest:point];
}
- (BOOL)hostsNative {
  return self.native != nil && self.native.superview == self;
}
// point 为宿主坐标: 在可见区域内且不在 occluders 里(之上没有 momo 内容)。
- (BOOL)receivesHostPoint:(NSPoint)point {
  if (self.hidden || ![self hostsNative] || !NSPointInRect(point, self.frame)) {
    return NO;
  }
  for (const NSRect& r : _occluders) {
    if (NSPointInRect(point, r)) {
      return NO;
    }
  }
  return YES;
}
- (NSView*)nativeHitTest:(NSPoint)point {
  NSView* hit = [super hitTest:point];
  return (hit == nil || hit == self) ? self.native : hit;
}
- (BOOL)ownsTrackingArea:(NSTrackingArea*)area {
  if (area == nil || self.native == nil) return NO;
  if ([area.owner isKindOfClass:[NSView class]]) {
    return [(NSView*)area.owner isDescendantOf:self];
  }
  // 跟踪区的 owner 可以不是视图(如 WebKit 的内部对象): 在子树里找安装了它的视图。
  NSMutableArray<NSView*>* pending = [NSMutableArray arrayWithObject:self];
  while (pending.count > 0) {
    NSView* view = pending.lastObject;
    [pending removeLastObject];
    if ([view.trackingAreas containsObject:area]) return YES;
    [pending addObjectsFromArray:view.subviews];
  }
  return NO;
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

- (instancetype)initWithContentView:(NSView*)contentView {
  if ((self = [super init])) {
    _content = contentView;
    _host = [[MomoPlatformViewsHostView alloc] initWithFrame:contentView.bounds];
    _host.owner = self;
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
  // 焦点在被移除的视图里: 交还给内容视图(否则键盘事件无人接收)。
  [self resignFocusIfInside:container];
  if (container.native.superview == container) {
    [container.native removeFromSuperview];  // 只摘掉挂在本窗口的;已被其他窗口接管的不动
  }
  container.native = nil;
  [container removeFromSuperview];
  std::erase(_zOrder, viewId);
  [self notifyGeometryChanged];
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
  [self notifyGeometryChanged];
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
  [self notifyGeometryChanged];
}

- (void)notifyGeometryChanged {
  if (self.geometryChanged != nil) {
    self.geometryChanged();
  }
}

- (int64_t)platformViewAtWindowPoint:(NSPoint)point {
  const NSPoint p = [_host convertPoint:point fromView:nil];
  for (auto it = _zOrder.rbegin(); it != _zOrder.rend(); ++it) {  // 自上而下
    auto c = _containers.find(*it);
    if (c != _containers.end() && [c->second receivesHostPoint:p]) {
      return *it;
    }
  }
  return 0;
}

- (NSView*)hostedNativeForPlatformView:(int64_t)viewId {
  auto it = _containers.find(viewId);
  if (it == _containers.end() || it->second.hidden || ![it->second hostsNative]) {
    return nil;
  }
  return it->second.native;
}

- (NSView*)nativeHitTestForPlatformView:(int64_t)viewId windowPoint:(NSPoint)point {
  auto it = _containers.find(viewId);
  if (it == _containers.end() || it->second.hidden || ![it->second hostsNative]) {
    return nil;
  }
  return [it->second nativeHitTest:[_host convertPoint:point fromView:nil]];
}

- (int64_t)platformViewOwningTrackingArea:(NSTrackingArea*)area {
  if (area == nil) return 0;
  for (auto& [view_id, container] : _containers) {
    if ([container ownsTrackingArea:area]) return view_id;
  }
  return 0;
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
