// momo:ui 图片: Image、Codec、decodeImage、instantiateCodec、imageFromPixels、loadAsset(M3)。
// 解码与资源读取在 io 线程进行(docs/08 §1), 结果经 Promise 回到 ui 线程;运行时在此期间被销毁
// (hot restart/关闭)时结果被丢弃。

#include <cstring>
#include <utility>

#include "base/task_util.h"
#include "engine/assets/image_decoder.h"
#include "engine/runtime/class_builder.h"
#include "engine/runtime/js_args.h"
#include "engine/ui/ui_classes.h"
#include "engine/ui/ui_impl.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ArgNumber;
using runtime::ClassBuilder;
using runtime::NewWrapper;
using runtime::OptNumber;
using runtime::ThrowError;
using runtime::ThrowTypeError;
using runtime::ToUtf8;
using runtime::V8String;

const WrapperTypeInfo JsImage::kTypeInfo = {"Image"};
const WrapperTypeInfo JsCodec::kTypeInfo = {"Codec"};

JsImage::~JsImage() { Dispose(); }

void JsImage::Set(v8::Isolate* isolate, std::shared_ptr<const graphics::Image> img) {
  Dispose();
  image = std::move(img);
  isolate_ = isolate;
  width = image ? image->width() : 0;
  height = image ? image->height() : 0;
  reported_bytes_ = image ? static_cast<int64_t>(image->byte_size()) : 0;
  if (reported_bytes_ > 0) {
    // 大对象向 isolate 报告外部内存压力(docs/04 §3), 促使 GC 及时回收不再引用的 Image。
    isolate_->AdjustAmountOfExternalAllocatedMemory(reported_bytes_);
  }
}

void JsImage::Dispose() {
  image.reset();
  if (reported_bytes_ > 0 && isolate_ != nullptr) {
    isolate_->AdjustAmountOfExternalAllocatedMemory(-reported_bytes_);
  }
  reported_bytes_ = 0;
}

namespace {

using ImageResult = StatusOr<std::shared_ptr<const graphics::Image>>;

bool BytesArg(const Args& a, int index, std::vector<uint8_t>* out) {
  if (index < a.Length() && a[index]->IsArrayBuffer()) {
    auto buffer = a[index].As<v8::ArrayBuffer>();
    const auto* d = static_cast<const uint8_t*>(buffer->Data());
    out->assign(d, d + buffer->ByteLength());
    return true;
  }
  if (index < a.Length() && a[index]->IsArrayBufferView()) {
    auto view = a[index].As<v8::ArrayBufferView>();
    out->resize(view->ByteLength());
    view->CopyContents(out->data(), out->size());
    return true;
  }
  ThrowTypeError(a.GetIsolate(), "bytes must be an ArrayBuffer or typed array");
  return false;
}

v8::Local<v8::Object> WrapImage(v8::Isolate* isolate, std::shared_ptr<const graphics::Image> img) {
  auto js = std::make_unique<JsImage>();
  js->Set(isolate, std::move(img));
  return NewWrapper(isolate, std::move(js));
}

// 在 io 线程执行 work, 结果回 ui 线程结算 promise。无 io runner(测试)时同步执行。
template <typename Result>
void RunOnIo(UiBindings::Impl& impl, uint64_t id, MoveOnlyFunction<Result()> work,
             MoveOnlyFunction<void(UiBindings::Impl&, uint64_t, Result)> done) {
  auto reply = [weak = impl.weak(), id, done = std::move(done)](Result result) mutable {
    if (UiBindings::Impl* self = weak.get()) {
      done(*self, id, std::move(result));
    }
  };
  if (!impl.host.io_runner) {
    impl.task_runner()->PostTask([work = std::move(work), reply = std::move(reply)]() mutable {
      reply(work());
    });
    return;
  }
  PostTaskAndReplyWithResult(*impl.host.io_runner, std::move(work), impl.task_runner(),
                             std::move(reply));
}

void ResolveImage(UiBindings::Impl& impl, uint64_t id, ImageResult result) {
  if (!result.ok()) {
    impl.Reject(id, std::string(result.status().message()));
    return;
  }
  impl.Settle(id, [&result](v8::Isolate* isolate, v8::Local<v8::Context> context,
                            v8::Local<v8::Promise::Resolver> resolver) {
    resolver->Resolve(context, WrapImage(isolate, *result)).Check();
  });
}

// decodeImage(bytes, targetWidth?, targetHeight?) → Promise<Image>
void DecodeImageFn(const Args& a) {
  std::vector<uint8_t> bytes;
  double tw = 0, th = 0;
  if (!BytesArg(a, 0, &bytes) || !OptNumber(a, 1, 0, &tw) || !OptNumber(a, 2, 0, &th)) {
    return;
  }
  UiBindings::Impl& impl = UiImpl(a.GetIsolate());
  v8::Local<v8::Promise> promise;
  const uint64_t id = impl.NewPromise(&promise);
  a.GetReturnValue().Set(promise);
  RunOnIo<ImageResult>(
      impl, id,
      [bytes = std::move(bytes), tw, th]() {
        return assets::DecodeImage(bytes, static_cast<int>(tw), static_cast<int>(th));
      },
      &ResolveImage);
}

// imageFromPixels(rgba: Uint8Array, width, height) → Image(同步)
void ImageFromPixels(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  double w = 0, h = 0;
  if (a.Length() < 1 || !a[0]->IsUint8Array()) {
    ThrowTypeError(isolate, "imageFromPixels: pixels must be a Uint8Array (RGBA8888)");
    return;
  }
  if (!ArgNumber(a, 1, &w) || !ArgNumber(a, 2, &h)) {
    return;
  }
  auto view = a[0].As<v8::Uint8Array>();
  const int width = static_cast<int>(w);
  const int height = static_cast<int>(h);
  if (width <= 0 || height <= 0 ||
      view->ByteLength() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
    ThrowTypeError(isolate, "imageFromPixels: pixel buffer too small for width*height*4");
    return;
  }
  const auto* data = static_cast<const uint8_t*>(view->Buffer()->Data()) + view->ByteOffset();
  ImageResult image = graphics::Image::FromRgba(data, width, height);
  if (!image.ok()) {
    ThrowError(isolate, std::string(image.status().message()));
    return;
  }
  a.GetReturnValue().Set(WrapImage(isolate, *image));
}

// loadAsset(path) → Promise<ArrayBuffer>
void LoadAsset(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  if (a.Length() < 1 || !a[0]->IsString()) {
    ThrowTypeError(isolate, "loadAsset(path): path must be a string");
    return;
  }
  UiBindings::Impl& impl = UiImpl(isolate);
  v8::Local<v8::Promise> promise;
  const uint64_t id = impl.NewPromise(&promise);
  a.GetReturnValue().Set(promise);
  using BytesResult = StatusOr<std::vector<uint8_t>>;
  RunOnIo<BytesResult>(
      impl, id,
      [assets = impl.assets, path = ToUtf8(isolate, a[0])]() { return assets.Load(path); },
      [](UiBindings::Impl& self, uint64_t req, BytesResult result) {
        if (!result.ok()) {
          self.Reject(req, std::string(result.status().message()));
          return;
        }
        self.Settle(req, [&result](v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                   v8::Local<v8::Promise::Resolver> resolver) {
          v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(iso, result->size());
          if (!result->empty()) {
            std::memcpy(buffer->Data(), result->data(), result->size());
          }
          resolver->Resolve(ctx, buffer).Check();
        });
      });
}

// ---------------------------------------------------------------------------
// Codec

using CodecResult = StatusOr<std::shared_ptr<assets::MultiFrameCodec>>;

void InstantiateCodec(const Args& a) {
  std::vector<uint8_t> bytes;
  if (!BytesArg(a, 0, &bytes)) {
    return;
  }
  UiBindings::Impl& impl = UiImpl(a.GetIsolate());
  v8::Local<v8::Promise> promise;
  const uint64_t id = impl.NewPromise(&promise);
  a.GetReturnValue().Set(promise);
  RunOnIo<CodecResult>(
      impl, id,
      [bytes = std::move(bytes)]() mutable -> CodecResult {
        MOMO_ASSIGN_OR_RETURN(std::unique_ptr<assets::MultiFrameCodec> codec,
                              assets::MultiFrameCodec::Create(std::move(bytes)));
        return std::shared_ptr<assets::MultiFrameCodec>(std::move(codec));
      },
      [](UiBindings::Impl& self, uint64_t req, CodecResult result) {
        if (!result.ok()) {
          self.Reject(req, std::string(result.status().message()));
          return;
        }
        self.Settle(req, [&result](v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                   v8::Local<v8::Promise::Resolver> resolver) {
          auto js = std::make_unique<JsCodec>();
          js->state->codec = *result;
          js->frame_count = (*result)->frame_count();
          js->repetition_count = (*result)->repetition_count();
          resolver->Resolve(ctx, NewWrapper(iso, std::move(js))).Check();
        });
      });
}

using FrameResult = StatusOr<assets::MultiFrameCodec::Frame>;

void CodecGetNextFrame(JsCodec& c, const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  UiBindings::Impl& impl = UiImpl(isolate);
  v8::Local<v8::Promise> promise;
  const uint64_t id = impl.NewPromise(&promise);
  a.GetReturnValue().Set(promise);
  if (c.state->codec == nullptr) {
    impl.Reject(id, "Codec has been disposed");
    return;
  }
  if (c.state->busy) {
    impl.Reject(id, "Codec.getNextFrame: previous frame is still decoding");
    return;
  }
  c.state->busy = true;
  // 解码状态只在 io 线程顺序访问(busy 保证同一时刻只有一个任务)。
  RunOnIo<FrameResult>(
      impl, id, [codec = c.state->codec]() { return codec->DecodeNextFrame(); },
      [state = c.state](UiBindings::Impl& self, uint64_t req, FrameResult result) {
        state->busy = false;
        if (!result.ok()) {
          self.Reject(req, std::string(result.status().message()));
          return;
        }
        self.Settle(req, [&result](v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                   v8::Local<v8::Promise::Resolver> resolver) {
          v8::Local<v8::Object> info = v8::Object::New(iso);
          info->Set(ctx, V8String(iso, "image"), WrapImage(iso, result->image)).Check();
          info->Set(ctx, V8String(iso, "durationMs"), v8::Number::New(iso, result->duration_ms))
              .Check();
          resolver->Resolve(ctx, info).Check();
        });
      });
}

void CodecFrameCount(JsCodec& c, const Args& a) { a.GetReturnValue().Set(c.frame_count); }
void CodecRepetition(JsCodec& c, const Args& a) { a.GetReturnValue().Set(c.repetition_count); }
void CodecDispose(JsCodec& c, const Args&) { c.state->codec.reset(); }

// ---------------------------------------------------------------------------
// Image

void ImageWidth(JsImage& i, const Args& a) { a.GetReturnValue().Set(i.width); }
void ImageHeight(JsImage& i, const Args& a) { a.GetReturnValue().Set(i.height); }
void ImageDispose(JsImage& i, const Args&) { i.Dispose(); }
void ImageDisposed(JsImage& i, const Args& a) { a.GetReturnValue().Set(i.image == nullptr); }

}  // namespace

void InstallImageClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                         v8::Local<v8::Object> exports) {
  ClassBuilder<JsImage>(isolate, "Image", nullptr)
      .Property("width", &ImageWidth)
      .Property("height", &ImageHeight)
      .Property("disposed", &ImageDisposed)
      .Method("dispose", &ImageDispose)
      .Export(context, exports);
  ClassBuilder<JsCodec>(isolate, "Codec", nullptr)
      .Property("frameCount", &CodecFrameCount)
      .Property("repetitionCount", &CodecRepetition)
      .Method("getNextFrame", &CodecGetNextFrame)
      .Method("dispose", &CodecDispose)
      .Export(context, exports);
  auto fn = [&](const char* name, v8::FunctionCallback cb) {
    exports->Set(context, V8String(isolate, name), v8::Function::New(context, cb).ToLocalChecked())
        .Check();
  };
  fn("decodeImage", &DecodeImageFn);
  fn("instantiateCodec", &InstantiateCodec);
  fn("imageFromPixels", &ImageFromPixels);
  fn("loadAsset", &LoadAsset);
}

}  // namespace momo::engine::ui
