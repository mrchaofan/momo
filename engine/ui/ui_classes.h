#pragma once

// momo:ui 宿主类的内部声明(V8)。只被 engine/ui 的实现文件包含。

#include <memory>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/matrix.h"

#include "engine/assets/image_decoder.h"
#include "engine/graphics/display_list.h"
#include "engine/graphics/image.h"
#include "engine/graphics/layer.h"
#include "engine/graphics/paint.h"
#include "engine/graphics/path.h"
#include "engine/runtime/wrappable.h"
#include "engine/text/paragraph.h"
#include "engine/ui/ui_bindings.h"
#include "v8.h"

namespace momo::engine::ui {

using runtime::Wrappable;
using runtime::WrapperTypeInfo;

class JsPaint final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  graphics::Paint paint;
  v8::Global<v8::Object> shader;  // 当前 shader 的 JS 对象(getter 原样返回)
};

class JsGradient final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<const graphics::Gradient> gradient;
};

// 写时复制: drawPath 只共享当前快照;快照被 DisplayList 引用时, 之后的修改先复制一份。
class JsPath final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  const graphics::Path& path() const { return *path_; }
  graphics::Path& mutable_path() {
    if (path_.use_count() > 1) {  // 其他引用只会在 ui 线程新增, 读到 1 即独占
      path_ = std::make_shared<graphics::Path>(*path_);
    }
    return *path_;
  }
  std::shared_ptr<const graphics::Path> snapshot() const { return path_; }

 private:
  std::shared_ptr<graphics::Path> path_ = std::make_shared<graphics::Path>();
};

class JsPicture final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<const graphics::DisplayList> display_list;
};

// Recorder 与它产出的 Canvas 共享 builder;endRecording 后 Canvas 失效(调用即抛错)。
struct Recording {
  explicit Recording(std::optional<Rect> cull) : builder(cull) {}
  graphics::DisplayListBuilder builder;
  bool active = true;
};

class JsPictureRecorder final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<Recording> recording;
};

class JsCanvas final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<Recording> recording;
};

class JsSceneBuilder final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<graphics::ContainerLayer> root = std::make_shared<graphics::ContainerLayer>();
  std::vector<std::shared_ptr<graphics::ContainerLayer>> stack;
  bool has_platform_views = false;
};

class JsScene final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::shared_ptr<graphics::ContainerLayer> root;
  bool has_platform_views = false;
};

// 图片: dispose 后 image 为空(之后绘制为空操作)。向 V8 报告外部内存。
class JsImage final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  ~JsImage() override;
  void Set(v8::Isolate* isolate, std::shared_ptr<const graphics::Image> image);
  void Dispose();
  std::shared_ptr<const graphics::Image> image;
  int width = 0;
  int height = 0;

 private:
  v8::Isolate* isolate_ = nullptr;
  int64_t reported_bytes_ = 0;
};

// 多帧解码器: 解码状态归 io 线程使用(经 shared_ptr 在任务间传递, 任一时刻只有一个任务持有)。
class JsCodec final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  // 闭包只捕获 State(不捕获任何 V8 句柄: 运行时可能先于 io 任务结束而销毁)。
  struct State {
    std::shared_ptr<assets::MultiFrameCodec> codec;
    bool busy = false;  // 正在 io 线程解码(并发 getNextFrame 直接拒绝);只在 ui 线程读写
  };
  std::shared_ptr<State> state = std::make_shared<State>();
  int frame_count = 1;
  int repetition_count = 0;
};

class JsParagraph final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::unique_ptr<text::Paragraph> paragraph;
};

class JsParagraphBuilder final : public Wrappable {
 public:
  static const WrapperTypeInfo kTypeInfo;
  const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
  std::unique_ptr<text::ParagraphBuilder> builder;
};

// 当前 isolate 上的 momo:ui 共享状态(UiBindings::Impl, 见 window_binding.cc)。
UiBindings::Impl& UiImpl(v8::Isolate* isolate);
// UiBindings::Impl 中供各绑定文件使用的设施。
text::FontCollection& UiFonts(v8::Isolate* isolate);
std::shared_ptr<graphics::TextureRegistry> UiTextures(v8::Isolate* isolate);

// 在 exports 上安装各类。返回 Scene 的类模板已登记(window.render 需要解包 Scene)。
void InstallPaintingClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                            v8::Local<v8::Object> exports);
void InstallSceneClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                         v8::Local<v8::Object> exports);
void InstallTextClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Object> exports);
void InstallImageClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                         v8::Local<v8::Object> exports);
// 为 host.extensions 中注册的每个 ext:* 模块注册 synthetic module(extension_bindings.cc)。
void InstallExtensionModules(UiBindings::Impl& impl);
// momo:services 模块(消息通道)。
void InstallServicesModule(v8::Isolate* isolate, v8::Local<v8::Context> context,
                           v8::Local<v8::Object> exports);

// 解析 16 个数(数组或 TypedArray, 列主序, 与 Flutter Matrix4 一致)为 Matrix。失败时已抛错。
bool ParseMatrix(v8::Isolate* isolate, v8::Local<v8::Value> value, Matrix* out);

}  // namespace momo::engine::ui
