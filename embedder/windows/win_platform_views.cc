#include "embedder/windows/win_platform_views.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "base/logging.h"
#include "embedder/common/platform_view_geometry.h"

namespace momo::embedder::windows {

using Microsoft::WRL::ComPtr;
using platform::CompositionLayout;
using platform::PlatformViewGeometry;

namespace {

// 平台视图的 id 是全局的, 视图会换窗口(platform 线程, 各窗口共用):
//   ViewHosts: 视图此刻挂在哪个窗口(该窗口的合成帧最近一次显示了它)。
//   gFocused:  框架的焦点所在的平台视图(0 = 不在平台视图上)。规则: 这个视图只要显示在某个窗口里, 就持有
//              键盘焦点——所以焦点可以先于视图显示, 视图隐藏、换窗口之后再次显示时也照常取回。
std::map<int64_t, const WinPlatformViews*>& ViewHosts() {
  static std::map<int64_t, const WinPlatformViews*> hosts;
  return hosts;
}
int64_t gFocused = 0;

float ToFloat(double value) { return static_cast<float>(value); }

}  // namespace

struct WinPlatformViews::Entry {
  WinPlatformViews* owner = nullptr;
  int64_t id = 0;
  const MomoWinPlatformView* view = nullptr;
  // 创建后不变(raster 线程在 compositor 的锁内使用)。
  ComPtr<IDCompositionVisual2> container;  // 位置、裁剪、透明度
  ComPtr<IDCompositionVisual2> content;    // 挂载点: 视图的内容挂在它下面
  ComPtr<IDCompositionRectangleClip> clip;
  ComPtr<IDCompositionEffectGroup> effect;
  MomoWinPlatformViewSite site{};
  // platform 线程
  bool shown = false;
  Rect frame;  // 最近一次呈现时的 frame(物理像素)
  SIZE size{};
};

WinPlatformViews::WinPlatformViews(HWND hwnd, std::shared_ptr<WinCompositor> compositor, Delegate* delegate)
    : hwnd_(hwnd), compositor_(std::move(compositor)), delegate_(delegate) {}

WinPlatformViews::~WinPlatformViews() {
  // 正常情况下视图已由 Shell::DetachView 逐个移出;这里只是兜底。
  while (!entries_.empty()) {
    Remove(entries_.begin()->first);
  }
}

bool WinPlatformViews::Hosts(int64_t view_id) const {
  auto it = ViewHosts().find(view_id);
  return it != ViewHosts().end() && it->second == this;
}

WinPlatformViews::Entry* WinPlatformViews::Find(int64_t view_id) const {
  auto it = entries_.find(view_id);
  return it != entries_.end() ? it->second.get() : nullptr;
}

void WinPlatformViews::Add(int64_t view_id, const MomoWinPlatformView* view) {
  if (view == nullptr || view->struct_size < sizeof(MomoWinPlatformView) || entries_.count(view_id) != 0) {
    return;
  }
  auto entry = std::make_unique<Entry>();
  entry->owner = this;
  entry->id = view_id;
  entry->view = view;
  entry->site = {sizeof(MomoWinPlatformViewSite), entry.get(), &SiteCommit, &SiteFocused, &SiteCursorChanged,
                 &SiteKeyDown};
  IDCompositionDesktopDevice* device = compositor_->device();
  HRESULT hr = device->CreateVisual(&entry->container);
  if (SUCCEEDED(hr)) hr = device->CreateVisual(&entry->content);
  if (SUCCEEDED(hr)) hr = device->CreateRectangleClip(&entry->clip);
  if (SUCCEEDED(hr)) hr = device->CreateEffectGroup(&entry->effect);
  if (SUCCEEDED(hr)) hr = entry->container->AddVisual(entry->content.Get(), FALSE, nullptr);
  if (FAILED(hr)) {
    MOMO_LOG(ERROR) << "Platform view " << view_id << ": cannot create its composition visuals.";
    return;
  }
  entry->container->SetClip(entry->clip.Get());
  entry->container->SetEffect(entry->effect.Get());
  compositor_->Update([&] { entries_[view_id] = std::move(entry); });
}

void WinPlatformViews::Remove(int64_t view_id) {
  Entry* entry = Find(view_id);
  if (entry == nullptr) {
    return;
  }
  if (hovered_ == view_id) {
    hovered_ = 0;
  }
  if (Hosts(view_id)) {
    ResignFocus(view_id);
    entry->view->attach(entry->view->user_data, nullptr, nullptr, nullptr);
    ViewHosts().erase(view_id);
  }
  compositor_->Update([&] {
    compositor_->root()->RemoveVisual(entry->container.Get());  // 不在树上时失败, 无妨
    std::erase(order_, entry->container.Get());
    entries_.erase(view_id);
  });
  delegate_->OnPlatformViewGeometryChanged();
}

void WinPlatformViews::Attach(Entry& entry) {
  // 视图可能正挂在另一个窗口: 直接改挂到本窗口(那个窗口从此不再 Hosts 它);焦点按规则在显示时取回。
  entry.view->attach(entry.view->user_data, hwnd_, entry.content.Get(), &entry.site);
  ViewHosts()[entry.id] = this;
  entry.size = {};
}

void WinPlatformViews::ResignFocus(int64_t view_id) {
  if (system_focus_ != view_id) {
    return;
  }
  system_focus_ = 0;
  // 只交还本窗口之内的焦点(视图的焦点窗口是本窗口的子窗口);焦点已去了别的窗口时不抢回来。
  HWND focus = ::GetFocus();
  if (focus != nullptr && focus != hwnd_ && ::IsChild(hwnd_, focus)) {
    ::SetFocus(hwnd_);
  }
}

void WinPlatformViews::Focus(int64_t view_id) {
  gFocused = view_id;
  Entry* entry = Find(view_id);
  if (entry != nullptr && entry->shown && Hosts(view_id)) {
    system_focus_ = view_id;
    entry->view->focus(entry->view->user_data);
  }
}

void WinPlatformViews::Blur(int64_t view_id) {
  if (gFocused == view_id) {
    gFocused = 0;
  }
  if (Hosts(view_id)) {
    ResignFocus(view_id);
  }
}

void WinPlatformViews::OnPresented(const CompositionLayout& layout) {
  if (disabled_.load(std::memory_order_acquire)) {
    return;
  }
  presented_ = layout;
  std::set<int64_t> shown;
  for (const PlatformViewGeometry& g : layout.views) {
    Entry* entry = Find(g.view_id);
    if (entry == nullptr || !PlatformViewVisibleRect(g)) {
      continue;
    }
    shown.insert(g.view_id);
    const bool attaching = !Hosts(g.view_id);
    if (attaching) {
      Attach(*entry);
      MOMO_LOG(VERBOSE) << "Platform view " << g.view_id << " attached to window.";
    }
    entry->frame = g.frame;
    const SIZE size{std::lround(g.frame.width), std::lround(g.frame.height)};
    if (size.cx != entry->size.cx || size.cy != entry->size.cy) {
      entry->size = size;
      entry->view->set_size(entry->view->user_data, size.cx, size.cy);
    }
    const bool appeared = !entry->shown || attaching;
    entry->shown = true;
    if (appeared && gFocused == g.view_id) {
      Focus(g.view_id);
    }
  }
  for (auto& [id, entry] : entries_) {
    if (entry->shown && shown.count(id) == 0) {
      // 被隐藏的平台视图不能继续持有键盘焦点(否则按键发给看不见的视图)。
      entry->shown = false;
      if (Hosts(id)) {
        ResignFocus(id);
      }
    }
  }
  delegate_->OnPlatformViewGeometryChanged();
}

int64_t WinPlatformViews::ViewAt(POINT client) const {
  if (disabled_.load(std::memory_order_acquire)) {
    return 0;
  }
  return PlatformViewAt(presented_, Point{static_cast<double>(client.x), static_cast<double>(client.y)},
                        [this](int64_t id) { return Hosts(id); });
}

void WinPlatformViews::SendMouse(int64_t view_id, UINT message, WPARAM wparam, POINT client) {
  Entry* entry = Find(view_id);
  if (entry == nullptr || !Hosts(view_id)) {
    return;
  }
  entry->view->mouse(entry->view->user_data, message, wparam, client.x - std::lround(entry->frame.x),
                     client.y - std::lround(entry->frame.y));
}

void WinPlatformViews::SetHovered(int64_t view_id) {
  if (hovered_ == view_id) {
    return;
  }
  if (Entry* old = Find(hovered_); old != nullptr && Hosts(hovered_)) {
    old->view->mouse(old->view->user_data, WM_MOUSELEAVE, 0, 0, 0);
  }
  hovered_ = view_id;
}

HCURSOR WinPlatformViews::Cursor(int64_t view_id) const {
  Entry* entry = Find(view_id);
  if (entry == nullptr || !Hosts(view_id)) {
    return nullptr;
  }
  return static_cast<HCURSOR>(entry->view->cursor(entry->view->user_data));
}

void WinPlatformViews::Compose(const CompositionLayout& layout, IDCompositionVisual2* base,
                               const std::vector<IDCompositionVisual2*>& overlays) {
  std::vector<IDCompositionVisual2*> order = {base};
  for (size_t i = 0; i < layout.views.size(); ++i) {
    const PlatformViewGeometry& g = layout.views[i];
    const std::optional<Rect> visible = PlatformViewVisibleRect(g);
    if (Entry* entry = Find(g.view_id); entry != nullptr && visible) {
      const float radius = ToFloat(PlatformViewCornerRadius(g, *visible));
      entry->container->SetOffsetX(ToFloat(visible->x));
      entry->container->SetOffsetY(ToFloat(visible->y));
      entry->clip->SetLeft(0);
      entry->clip->SetTop(0);
      entry->clip->SetRight(ToFloat(visible->width));
      entry->clip->SetBottom(ToFloat(visible->height));
      IDCompositionRectangleClip* clip = entry->clip.Get();
      clip->SetTopLeftRadiusX(radius);
      clip->SetTopLeftRadiusY(radius);
      clip->SetTopRightRadiusX(radius);
      clip->SetTopRightRadiusY(radius);
      clip->SetBottomLeftRadiusX(radius);
      clip->SetBottomLeftRadiusY(radius);
      clip->SetBottomRightRadiusX(radius);
      clip->SetBottomRightRadiusY(radius);
      entry->effect->SetOpacity(ToFloat(std::clamp(g.opacity, 0.0, 1.0)));
      entry->content->SetOffsetX(ToFloat(g.frame.x - visible->x));
      entry->content->SetOffsetY(ToFloat(g.frame.y - visible->y));
      order.push_back(entry->container.Get());
    }
    // overlays[i] 位于 views[i] 之上(即使视图本身不可见, overlay 仍承载其后的内容)。
    if (i < overlays.size() && overlays[i] != nullptr && i < layout.overlays.size() && layout.overlays[i]) {
      overlays[i]->SetOffsetX(ToFloat(layout.overlays[i]->x));
      overlays[i]->SetOffsetY(ToFloat(layout.overlays[i]->y));
      order.push_back(overlays[i]);
    }
  }
  if (order == order_) {
    return;
  }
  // z 序只用 root 的子 visual 顺序表达;只在顺序变化时重排。
  IDCompositionVisual2* root = compositor_->root();
  root->RemoveAllVisuals();
  for (size_t i = 0; i < order.size(); ++i) {
    root->AddVisual(order[i], TRUE, i == 0 ? nullptr : order[i - 1]);
  }
  order_ = std::move(order);
}

// ---- 视图 → 宿主(MomoWinPlatformViewSite) ----

void WinPlatformViews::SiteCommit(void* context) {
  static_cast<Entry*>(context)->owner->compositor_->Update([] {});
}

void WinPlatformViews::SiteFocused(void* context) {
  auto* entry = static_cast<Entry*>(context);
  gFocused = entry->id;
  entry->owner->system_focus_ = entry->id;
  entry->owner->delegate_->OnPlatformViewFocused(entry->id);
}

void WinPlatformViews::SiteCursorChanged(void* context) {
  auto* entry = static_cast<Entry*>(context);
  entry->owner->delegate_->OnPlatformViewCursorChanged(entry->id);
}

bool WinPlatformViews::SiteKeyDown(void* context, uint32_t message, uintptr_t wparam, intptr_t lparam) {
  auto* entry = static_cast<Entry*>(context);
  return entry->owner->delegate_->OnPlatformViewKeyDown(message, wparam, lparam);
}

}  // namespace momo::embedder::windows
