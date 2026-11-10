// momo:ui 场景类: SceneBuilder、Scene(docs/02 §4)。
// SceneBuilder 维护一个 ContainerLayer 栈: push* 压入新容器层, addPicture 加叶子, pop 弹出, build 冻结。

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/runtime/class_builder.h"
#include "engine/runtime/js_args.h"
#include "engine/ui/ui_classes.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ArgNumber;
using runtime::ArgWrapped;
using runtime::ClassBuilder;
using runtime::NewWrapper;
using runtime::ThrowError;
using runtime::V8String;

const WrapperTypeInfo JsSceneBuilder::kTypeInfo = {"SceneBuilder"};
const WrapperTypeInfo JsScene::kTypeInfo = {"Scene"};

namespace {

graphics::ContainerLayer& Top(JsSceneBuilder& b) {
  return b.stack.empty() ? *b.root : *b.stack.back();
}

void Push(JsSceneBuilder& b, const Args& a, std::shared_ptr<graphics::ContainerLayer> layer) {
  Top(b).Add(layer);
  b.stack.push_back(std::move(layer));
  a.GetReturnValue().Set(a.This());
}

void PushOffset(JsSceneBuilder& b, const Args& a) {
  double dx = 0, dy = 0;
  if (ArgNumber(a, 0, &dx) && ArgNumber(a, 1, &dy)) {
    Push(b, a, std::make_shared<graphics::OffsetLayer>(Point{dx, dy}));
  }
}

void PushTransform(JsSceneBuilder& b, const Args& a) {
  Matrix m;
  if (ParseMatrix(a.GetIsolate(), a[0], &m)) {
    Push(b, a, std::make_shared<graphics::TransformLayer>(m));
  }
}

void PushClipRect(JsSceneBuilder& b, const Args& a) {
  double v[4];
  for (int i = 0; i < 4; ++i) {
    if (!ArgNumber(a, i, &v[i])) {
      return;
    }
  }
  Push(b, a, std::make_shared<graphics::ClipRectLayer>(Rect{v[0], v[1], v[2], v[3]}));
}

void PushClipRRect(JsSceneBuilder& b, const Args& a) {
  double v[5];
  for (int i = 0; i < 5; ++i) {
    if (!ArgNumber(a, i, &v[i])) {
      return;
    }
  }
  Push(b, a, std::make_shared<graphics::ClipRRectLayer>(Rect{v[0], v[1], v[2], v[3]}, v[4]));
}

// addTexture(textureId, x, y, w, h): 未注册的 id 为空操作。
void AddTexture(JsSceneBuilder& b, const Args& a) {
  double v[5];
  for (int i = 0; i < 5; ++i) {
    if (!ArgNumber(a, i, &v[i])) {
      return;
    }
  }
  a.GetReturnValue().Set(a.This());
  std::shared_ptr<graphics::TextureRegistry> registry = UiTextures(a.GetIsolate());
  std::shared_ptr<graphics::Texture> texture =
      registry ? registry->Get(static_cast<int64_t>(v[0])) : nullptr;
  if (texture != nullptr) {
    Top(b).Add(std::make_shared<graphics::TextureLayer>(Rect{v[1], v[2], v[3], v[4]},
                                                        std::move(texture)));
  }
}

// addPlatformView(viewId, x, y, w, h): 平台视图占位(docs/05 §9)。视图是否存在由合成时的宿主决定,
// 未创建/已销毁的 id 在合成时被忽略。
void AddPlatformView(JsSceneBuilder& b, const Args& a) {
  double v[5];
  for (int i = 0; i < 5; ++i) {
    if (!ArgNumber(a, i, &v[i])) {
      return;
    }
  }
  a.GetReturnValue().Set(a.This());
  if (v[0] <= 0 || v[3] <= 0 || v[4] <= 0) {
    return;
  }
  Top(b).Add(std::make_shared<graphics::PlatformViewLayer>(Rect{v[1], v[2], v[3], v[4]},
                                                           static_cast<int64_t>(v[0])));
  b.has_platform_views = true;
}

// alpha: 0..255(与 Flutter 一致)。
void PushOpacity(JsSceneBuilder& b, const Args& a) {
  double alpha = 0;
  if (ArgNumber(a, 0, &alpha)) {
    const auto clamped = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.0, 255.0)));
    Push(b, a, std::make_shared<graphics::OpacityLayer>(clamped));
  }
}

void Pop(JsSceneBuilder& b, const Args& a) {
  if (b.stack.empty()) {
    ThrowError(a.GetIsolate(), "SceneBuilder.pop: nothing to pop");
    return;
  }
  b.stack.pop_back();
  a.GetReturnValue().Set(a.This());
}

// addPicture(dx, dy, picture)
void AddPicture(JsSceneBuilder& b, const Args& a) {
  double dx = 0, dy = 0;
  if (!ArgNumber(a, 0, &dx) || !ArgNumber(a, 1, &dy)) {
    return;
  }
  JsPicture* picture = ArgWrapped<JsPicture>(a, 2);
  if (picture == nullptr) {
    return;
  }
  Top(b).Add(std::make_shared<graphics::PictureLayer>(Point{dx, dy}, picture->display_list));
  a.GetReturnValue().Set(a.This());
}

// 未 pop 的层自动闭合;builder 随后清空, 可继续构建下一个场景。
void Build(JsSceneBuilder& b, const Args& a) {
  auto scene = std::make_unique<JsScene>();
  scene->root = std::move(b.root);
  scene->has_platform_views = b.has_platform_views;
  b.root = std::make_shared<graphics::ContainerLayer>();
  b.stack.clear();
  b.has_platform_views = false;
  a.GetReturnValue().Set(NewWrapper(a.GetIsolate(), std::move(scene)));
}

}  // namespace

void InstallSceneClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                         v8::Local<v8::Object> exports) {
  ClassBuilder<JsSceneBuilder>(isolate, "SceneBuilder",
                               [](const Args&) { return std::make_unique<JsSceneBuilder>(); })
      .Method("pushOffset", &PushOffset)
      .Method("pushTransform", &PushTransform)
      .Method("pushClipRect", &PushClipRect)
      .Method("pushClipRRect", &PushClipRRect)
      .Method("pushOpacity", &PushOpacity)
      .Method("addTexture", &AddTexture)
      .Method("addPlatformView", &AddPlatformView)
      .Method("pop", &Pop)
      .Method("addPicture", &AddPicture)
      .Method("build", &Build)
      .Export(context, exports);
  ClassBuilder<JsScene>(isolate, "Scene", nullptr).Export(context, exports);
}

}  // namespace momo::engine::ui
