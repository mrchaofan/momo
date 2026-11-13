// momo:ui 绘制类: Paint、Gradient、Path、PictureRecorder、Canvas、Picture。

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "engine/runtime/class_builder.h"
#include "engine/runtime/js_args.h"
#include "engine/ui/ui_classes.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ArgNumber;
using runtime::ArgUint32;
using runtime::ArgWrapped;
using runtime::ClassBuilder;
using runtime::NewWrapper;
using runtime::ThrowError;
using runtime::ThrowTypeError;
using runtime::ToUtf8;
using runtime::V8String;

const WrapperTypeInfo JsPaint::kTypeInfo = {"Paint"};
const WrapperTypeInfo JsGradient::kTypeInfo = {"Gradient"};
const WrapperTypeInfo JsPath::kTypeInfo = {"Path"};
const WrapperTypeInfo JsPicture::kTypeInfo = {"Picture"};
const WrapperTypeInfo JsPictureRecorder::kTypeInfo = {"PictureRecorder"};
const WrapperTypeInfo JsCanvas::kTypeInfo = {"Canvas"};

namespace {

// 读取 n 个数字参数(从 start 起)。
bool Numbers(const Args& args, int start, int n, double* out) {
  for (int i = 0; i < n; ++i) {
    if (!ArgNumber(args, start + i, &out[i])) {
      return false;
    }
  }
  return true;
}

Rect RectOf(const double* v) { return {v[0], v[1], v[2], v[3]}; }

// 字符串枚举参数/属性值。
template <typename E>
bool ParseEnum(v8::Isolate* isolate, v8::Local<v8::Value> value, const char* what,
               std::initializer_list<std::pair<const char*, E>> options, E* out) {
  const std::string s = value->IsString() ? ToUtf8(isolate, value) : std::string();
  for (const auto& [name, e] : options) {
    if (s == name) {
      *out = e;
      return true;
    }
  }
  std::string expected;
  for (const auto& [name, e] : options) {
    expected += (expected.empty() ? "'" : ", '") + std::string(name) + "'";
  }
  ThrowTypeError(isolate, std::string(what) + " must be one of " + expected);
  return false;
}

template <typename E>
const char* EnumName(std::initializer_list<std::pair<const char*, E>> options, E value) {
  for (const auto& [name, e] : options) {
    if (e == value) {
      return name;
    }
  }
  return "";
}

using Style = graphics::Paint::Style;
using Cap = graphics::Paint::StrokeCap;
using Join = graphics::Paint::StrokeJoin;
const std::initializer_list<std::pair<const char*, Style>> kStyles = {{"fill", Style::kFill},
                                                                      {"stroke", Style::kStroke}};
const std::initializer_list<std::pair<const char*, Cap>> kCaps = {
    {"butt", Cap::kButt}, {"round", Cap::kRound}, {"square", Cap::kSquare}};
const std::initializer_list<std::pair<const char*, Join>> kJoins = {
    {"miter", Join::kMiter}, {"round", Join::kRound}, {"bevel", Join::kBevel}};
using Tile = graphics::Gradient::TileMode;
const std::initializer_list<std::pair<const char*, Tile>> kTiles = {
    {"clamp", Tile::kClamp}, {"repeat", Tile::kRepeat}, {"mirror", Tile::kMirror}};
using Quality = graphics::Paint::FilterQuality;
const std::initializer_list<std::pair<const char*, Quality>> kQualities = {
    {"none", Quality::kNone}, {"low", Quality::kLow}, {"medium", Quality::kMedium},
    {"high", Quality::kHigh}};
using Fill = graphics::Path::FillType;
const std::initializer_list<std::pair<const char*, Fill>> kFills = {{"nonzero", Fill::kNonZero},
                                                                    {"evenodd", Fill::kEvenOdd}};

// ---------------------------------------------------------------------------
// Paint

void PaintGetColor(JsPaint& p, const Args& a) { a.GetReturnValue().Set(p.paint.color); }
void PaintSetColor(JsPaint& p, const Args& a) { ArgUint32(a, 0, &p.paint.color); }
void PaintGetStyle(JsPaint& p, const Args& a) {
  a.GetReturnValue().Set(V8String(a.GetIsolate(), EnumName(kStyles, p.paint.style)));
}
void PaintSetStyle(JsPaint& p, const Args& a) {
  ParseEnum(a.GetIsolate(), a[0], "Paint.style", kStyles, &p.paint.style);
}
void PaintGetStrokeWidth(JsPaint& p, const Args& a) { a.GetReturnValue().Set(p.paint.stroke_width); }
void PaintSetStrokeWidth(JsPaint& p, const Args& a) {
  double w = 0;
  if (ArgNumber(a, 0, &w)) {
    p.paint.stroke_width = std::max(0.0, w);
  }
}
void PaintGetStrokeCap(JsPaint& p, const Args& a) {
  a.GetReturnValue().Set(V8String(a.GetIsolate(), EnumName(kCaps, p.paint.stroke_cap)));
}
void PaintSetStrokeCap(JsPaint& p, const Args& a) {
  ParseEnum(a.GetIsolate(), a[0], "Paint.strokeCap", kCaps, &p.paint.stroke_cap);
}
void PaintGetStrokeJoin(JsPaint& p, const Args& a) {
  a.GetReturnValue().Set(V8String(a.GetIsolate(), EnumName(kJoins, p.paint.stroke_join)));
}
void PaintSetStrokeJoin(JsPaint& p, const Args& a) {
  ParseEnum(a.GetIsolate(), a[0], "Paint.strokeJoin", kJoins, &p.paint.stroke_join);
}
void PaintGetAntiAlias(JsPaint& p, const Args& a) { a.GetReturnValue().Set(p.paint.anti_alias); }
void PaintSetAntiAlias(JsPaint& p, const Args& a) {
  p.paint.anti_alias = a[0]->BooleanValue(a.GetIsolate());
}
void PaintGetFilterQuality(JsPaint& p, const Args& a) {
  a.GetReturnValue().Set(V8String(a.GetIsolate(), EnumName(kQualities, p.paint.filter_quality)));
}
void PaintSetFilterQuality(JsPaint& p, const Args& a) {
  ParseEnum(a.GetIsolate(), a[0], "Paint.filterQuality", kQualities, &p.paint.filter_quality);
}
void PaintGetMaskBlur(JsPaint& p, const Args& a) { a.GetReturnValue().Set(p.paint.mask_blur_sigma); }
void PaintSetMaskBlur(JsPaint& p, const Args& a) {
  double sigma = 0;
  if (ArgNumber(a, 0, &sigma)) {
    p.paint.mask_blur_sigma = std::max(0.0, sigma);
  }
}
void PaintGetShader(JsPaint& p, const Args& a) {
  if (p.shader.IsEmpty()) {
    a.GetReturnValue().SetNull();
  } else {
    a.GetReturnValue().Set(p.shader.Get(a.GetIsolate()));
  }
}
void PaintSetShader(JsPaint& p, const Args& a) {
  if (a[0]->IsNullOrUndefined()) {
    p.paint.gradient.reset();
    p.shader.Reset();
    return;
  }
  JsGradient* g = ArgWrapped<JsGradient>(a, 0);
  if (g == nullptr) {
    return;
  }
  p.paint.gradient = g->gradient;
  p.shader.Reset(a.GetIsolate(), a[0].As<v8::Object>());
}

// ---------------------------------------------------------------------------
// Gradient(静态工厂)

bool ParseGradientTail(const Args& a, int colors_index, graphics::Gradient* g) {
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (colors_index >= a.Length() || !a[colors_index]->IsArray()) {
    ThrowTypeError(isolate, "Gradient colors must be an array of ARGB numbers");
    return false;
  }
  v8::Local<v8::Array> colors = a[colors_index].As<v8::Array>();
  if (colors->Length() < 2) {
    ThrowTypeError(isolate, "Gradient needs at least 2 colors");
    return false;
  }
  for (uint32_t i = 0; i < colors->Length(); ++i) {
    v8::Local<v8::Value> c = colors->Get(context, i).ToLocalChecked();
    if (!c->IsNumber()) {
      ThrowTypeError(isolate, "Gradient colors must be numbers");
      return false;
    }
    g->colors.push_back(static_cast<uint32_t>(static_cast<int64_t>(c.As<v8::Number>()->Value())));
  }
  const int stops_index = colors_index + 1;
  if (stops_index < a.Length() && !a[stops_index]->IsNullOrUndefined()) {
    if (!a[stops_index]->IsArray() || a[stops_index].As<v8::Array>()->Length() != colors->Length()) {
      ThrowTypeError(isolate, "Gradient stops must be an array with one stop per color");
      return false;
    }
    v8::Local<v8::Array> stops = a[stops_index].As<v8::Array>();
    for (uint32_t i = 0; i < stops->Length(); ++i) {
      v8::Local<v8::Value> s = stops->Get(context, i).ToLocalChecked();
      g->stops.push_back(static_cast<float>(s->NumberValue(context).FromMaybe(0)));
    }
  }
  const int tile_index = colors_index + 2;
  if (tile_index < a.Length() && !a[tile_index]->IsUndefined()) {
    return ParseEnum(isolate, a[tile_index], "Gradient tileMode", kTiles, &g->tile_mode);
  }
  return true;
}

void ReturnGradient(const Args& a, std::shared_ptr<graphics::Gradient> g) {
  auto js = std::make_unique<JsGradient>();
  js->gradient = std::move(g);
  a.GetReturnValue().Set(NewWrapper(a.GetIsolate(), std::move(js)));
}

// Gradient.linear(x0, y0, x1, y1, colors, stops?, tileMode?)
void GradientLinear(const Args& a) {
  double v[4];
  auto g = std::make_shared<graphics::Gradient>();
  if (!Numbers(a, 0, 4, v) || !ParseGradientTail(a, 4, g.get())) {
    return;
  }
  g->type = graphics::Gradient::Type::kLinear;
  g->start = {v[0], v[1]};
  g->end = {v[2], v[3]};
  ReturnGradient(a, std::move(g));
}

// Gradient.radial(cx, cy, radius, colors, stops?, tileMode?)
void GradientRadial(const Args& a) {
  double v[3];
  auto g = std::make_shared<graphics::Gradient>();
  if (!Numbers(a, 0, 3, v) || !ParseGradientTail(a, 3, g.get())) {
    return;
  }
  g->type = graphics::Gradient::Type::kRadial;
  g->start = {v[0], v[1]};
  g->radius = v[2];
  ReturnGradient(a, std::move(g));
}

// ---------------------------------------------------------------------------
// Path(变更方法返回 this, 便于链式调用)

void ReturnThis(const Args& a) { a.GetReturnValue().Set(a.This()); }

void PathMoveTo(JsPath& p, const Args& a) {
  double v[2];
  if (Numbers(a, 0, 2, v)) { p.mutable_path().MoveTo(v[0], v[1]); ReturnThis(a); }
}
void PathLineTo(JsPath& p, const Args& a) {
  double v[2];
  if (Numbers(a, 0, 2, v)) { p.mutable_path().LineTo(v[0], v[1]); ReturnThis(a); }
}
void PathQuadTo(JsPath& p, const Args& a) {
  double v[4];
  if (Numbers(a, 0, 4, v)) { p.mutable_path().QuadTo(v[0], v[1], v[2], v[3]); ReturnThis(a); }
}
void PathCubicTo(JsPath& p, const Args& a) {
  double v[6];
  if (Numbers(a, 0, 6, v)) { p.mutable_path().CubicTo(v[0], v[1], v[2], v[3], v[4], v[5]); ReturnThis(a); }
}
void PathClose(JsPath& p, const Args& a) { p.mutable_path().Close(); ReturnThis(a); }
void PathReset(JsPath& p, const Args& a) { p.mutable_path().Reset(); ReturnThis(a); }
void PathAddRect(JsPath& p, const Args& a) {
  double v[4];
  if (Numbers(a, 0, 4, v)) { p.mutable_path().AddRect(RectOf(v)); ReturnThis(a); }
}
void PathAddOval(JsPath& p, const Args& a) {
  double v[4];
  if (Numbers(a, 0, 4, v)) { p.mutable_path().AddOval(RectOf(v)); ReturnThis(a); }
}
void PathAddRRect(JsPath& p, const Args& a) {
  double v[5];
  if (Numbers(a, 0, 5, v)) { p.mutable_path().AddRRect(RectOf(v), v[4]); ReturnThis(a); }
}
void PathGetFillType(JsPath& p, const Args& a) {
  a.GetReturnValue().Set(V8String(a.GetIsolate(), EnumName(kFills, p.path().fill_type())));
}
void PathSetFillType(JsPath& p, const Args& a) {
  Fill f;
  if (ParseEnum(a.GetIsolate(), a[0], "Path.fillType", kFills, &f)) {
    p.mutable_path().set_fill_type(f);
  }
}
void PathGetBounds(JsPath& p, const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const Rect b = p.path().bounds();
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(context, V8String(isolate, "x"), v8::Number::New(isolate, b.x)).Check();
  o->Set(context, V8String(isolate, "y"), v8::Number::New(isolate, b.y)).Check();
  o->Set(context, V8String(isolate, "width"), v8::Number::New(isolate, b.width)).Check();
  o->Set(context, V8String(isolate, "height"), v8::Number::New(isolate, b.height)).Check();
  a.GetReturnValue().Set(o);
}

// ---------------------------------------------------------------------------
// PictureRecorder / Canvas / Picture

void RecorderBegin(JsPictureRecorder& r, const Args& a) {
  if (r.recording && r.recording->active) {
    ThrowError(a.GetIsolate(), "PictureRecorder is already recording");
    return;
  }
  std::optional<Rect> cull;
  if (a.Length() >= 4) {
    double v[4];
    if (!Numbers(a, 0, 4, v)) {
      return;
    }
    cull = RectOf(v);
  }
  r.recording = std::make_shared<Recording>(cull);
  auto canvas = std::make_unique<JsCanvas>();
  canvas->recording = r.recording;
  a.GetReturnValue().Set(NewWrapper(a.GetIsolate(), std::move(canvas)));
}

void RecorderEnd(JsPictureRecorder& r, const Args& a) {
  if (!r.recording || !r.recording->active) {
    ThrowError(a.GetIsolate(), "PictureRecorder is not recording");
    return;
  }
  auto picture = std::make_unique<JsPicture>();
  picture->display_list = r.recording->builder.Build();
  r.recording->active = false;
  r.recording.reset();
  a.GetReturnValue().Set(NewWrapper(a.GetIsolate(), std::move(picture)));
}

void RecorderIsRecording(JsPictureRecorder& r, const Args& a) {
  a.GetReturnValue().Set(r.recording != nullptr && r.recording->active);
}

// Canvas 方法的公共前置: 记录已结束则抛错。
graphics::DisplayListBuilder* Builder(JsCanvas& c, const Args& a) {
  if (!c.recording || !c.recording->active) {
    ThrowError(a.GetIsolate(), "Canvas is no longer recording (endRecording was called)");
    return nullptr;
  }
  return &c.recording->builder;
}

void CanvasSave(JsCanvas& c, const Args& a) {
  if (auto* b = Builder(c, a)) b->Save();
}
void CanvasRestore(JsCanvas& c, const Args& a) {
  if (auto* b = Builder(c, a)) b->Restore();
}
void CanvasGetSaveCount(JsCanvas& c, const Args& a) {
  if (auto* b = Builder(c, a)) a.GetReturnValue().Set(b->save_count() + 1);
}
void CanvasTranslate(JsCanvas& c, const Args& a) {
  double v[2];
  if (auto* b = Builder(c, a); b && Numbers(a, 0, 2, v)) b->Translate(v[0], v[1]);
}
void CanvasScale(JsCanvas& c, const Args& a) {
  double sx = 0, sy = 0;
  auto* b = Builder(c, a);
  if (b && ArgNumber(a, 0, &sx) && runtime::OptNumber(a, 1, sx, &sy)) b->Scale(sx, sy);
}
void CanvasRotate(JsCanvas& c, const Args& a) {
  double r = 0;
  if (auto* b = Builder(c, a); b && ArgNumber(a, 0, &r)) b->Rotate(r);
}
void CanvasTransform(JsCanvas& c, const Args& a) {
  Matrix m;
  if (auto* b = Builder(c, a); b && ParseMatrix(a.GetIsolate(), a[0], &m)) b->Transform(m);
}
void CanvasClipRect(JsCanvas& c, const Args& a) {
  double v[4];
  if (auto* b = Builder(c, a); b && Numbers(a, 0, 4, v)) b->ClipRect(RectOf(v));
}
// addHitRegion(x, y, w, h): 记录可命中区域(不绘制), 平台层据此判断平台视图是否被 momo 内容盖住。
void CanvasAddHitRegion(JsCanvas& c, const Args& a) {
  double v[4];
  if (auto* b = Builder(c, a); b && Numbers(a, 0, 4, v)) b->AddHitRegion(RectOf(v));
}
void CanvasClipRRect(JsCanvas& c, const Args& a) {
  double v[5];
  if (auto* b = Builder(c, a); b && Numbers(a, 0, 5, v)) b->ClipRRect(RectOf(v), v[4]);
}
void CanvasClipPath(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  if (JsPath* path = b ? ArgWrapped<JsPath>(a, 0) : nullptr) b->ClipPath(path->snapshot());
}
// saveLayer(x|null, y|null, w|null, h|null, paint|null)
void CanvasSaveLayer(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  if (b == nullptr) return;
  std::optional<Rect> bounds;
  if (a.Length() >= 4 && !a[0]->IsNullOrUndefined()) {
    double v[4];
    if (!Numbers(a, 0, 4, v)) return;
    bounds = RectOf(v);
  }
  const graphics::Paint* paint = nullptr;
  if (a.Length() >= 5 && !a[4]->IsNullOrUndefined()) {
    JsPaint* p = ArgWrapped<JsPaint>(a, 4);
    if (p == nullptr) return;
    paint = &p->paint;
  }
  b->SaveLayer(bounds, paint);
}
// 可选 paint 参数: 缺省/null → nullptr;类型错误 → 抛错并返回 false。
bool OptPaint(const Args& a, int index, const graphics::Paint** out) {
  *out = nullptr;
  if (index >= a.Length() || a[index]->IsNullOrUndefined()) return true;
  JsPaint* p = ArgWrapped<JsPaint>(a, index);
  if (p == nullptr) return false;
  *out = &p->paint;
  return true;
}
void CanvasDrawDRRect(JsCanvas& c, const Args& a) {
  double v[10];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 10, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 10)) {
    b->DrawDRRect(RectOf(v), v[4], RectOf(v + 5), v[9], p->paint);
  }
}
void CanvasDrawImage(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  JsImage* image = b ? ArgWrapped<JsImage>(a, 0) : nullptr;
  double v[2];
  const graphics::Paint* paint = nullptr;
  if (!image || !Numbers(a, 1, 2, v) || !OptPaint(a, 3, &paint)) return;
  b->DrawImage(image->image, {v[0], v[1]}, paint);
}
void CanvasDrawImageRect(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  JsImage* image = b ? ArgWrapped<JsImage>(a, 0) : nullptr;
  double v[8];
  const graphics::Paint* paint = nullptr;
  if (!image || !Numbers(a, 1, 8, v) || !OptPaint(a, 9, &paint)) return;
  b->DrawImageRect(image->image, RectOf(v), RectOf(v + 4), paint);
}
void CanvasDrawParagraph(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  JsParagraph* para = b ? ArgWrapped<JsParagraph>(a, 0) : nullptr;
  double v[2];
  if (!para || !Numbers(a, 1, 2, v)) return;
  if (para->paragraph == nullptr) {
    ThrowError(a.GetIsolate(), "drawParagraph: paragraph has been disposed");
    return;
  }
  b->DrawExternal(para->paragraph->Record(), {v[0], v[1]}, para->paragraph->paint_bounds());
}
void CanvasDrawTexture(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  double v[5];
  if (!b || !Numbers(a, 0, 5, v)) return;
  std::shared_ptr<graphics::TextureRegistry> registry = UiTextures(a.GetIsolate());
  std::shared_ptr<graphics::Texture> texture =
      registry ? registry->Get(static_cast<int64_t>(v[0])) : nullptr;
  if (texture == nullptr) return;  // 未注册/已注销: 空操作(与 Flutter Texture 一致)
  b->DrawTexture(std::move(texture), RectOf(v + 1));
}
void CanvasDrawColor(JsCanvas& c, const Args& a) {
  uint32_t color = 0;
  if (auto* b = Builder(c, a); b && ArgUint32(a, 0, &color)) b->DrawColor(color);
}
void CanvasDrawPaint(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  if (JsPaint* p = b ? ArgWrapped<JsPaint>(a, 0) : nullptr) b->DrawPaint(p->paint);
}
void CanvasDrawRect(JsCanvas& c, const Args& a) {
  double v[4];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 4, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 4)) b->DrawRect(RectOf(v), p->paint);
}
void CanvasDrawRRect(JsCanvas& c, const Args& a) {
  double v[5];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 5, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 5)) b->DrawRRect(RectOf(v), v[4], p->paint);
}
void CanvasDrawOval(JsCanvas& c, const Args& a) {
  double v[4];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 4, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 4)) b->DrawOval(RectOf(v), p->paint);
}
void CanvasDrawCircle(JsCanvas& c, const Args& a) {
  double v[3];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 3, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 3)) b->DrawCircle({v[0], v[1]}, v[2], p->paint);
}
void CanvasDrawLine(JsCanvas& c, const Args& a) {
  double v[4];
  auto* b = Builder(c, a);
  if (!b || !Numbers(a, 0, 4, v)) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 4)) b->DrawLine({v[0], v[1]}, {v[2], v[3]}, p->paint);
}
void CanvasDrawPath(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  JsPath* path = b ? ArgWrapped<JsPath>(a, 0) : nullptr;
  if (path == nullptr) return;
  if (JsPaint* p = ArgWrapped<JsPaint>(a, 1)) b->DrawPath(path->snapshot(), p->paint);
}
void CanvasDrawPicture(JsCanvas& c, const Args& a) {
  auto* b = Builder(c, a);
  if (JsPicture* pic = b ? ArgWrapped<JsPicture>(a, 0) : nullptr) b->DrawDisplayList(pic->display_list);
}

void PictureOpCount(JsPicture& p, const Args& a) {
  a.GetReturnValue().Set(static_cast<double>(p.display_list ? p.display_list->op_count() : 0));
}

}  // namespace

bool ParseMatrix(v8::Isolate* isolate, v8::Local<v8::Value> value, Matrix* out) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  if (!value->IsObject() || (!value->IsArray() && !value->IsTypedArray())) {
    ThrowTypeError(isolate, "matrix must be an array of 16 numbers (column-major)");
    return false;
  }
  v8::Local<v8::Object> obj = value.As<v8::Object>();
  double v[16];
  for (uint32_t i = 0; i < 16; ++i) {
    v8::Local<v8::Value> e;
    if (!obj->Get(context, i).ToLocal(&e) || !e->IsNumber()) {
      ThrowTypeError(isolate, "matrix must be an array of 16 numbers (column-major)");
      return false;
    }
    v[i] = e.As<v8::Number>()->Value();
  }
  // 列主序(v[col*4 + row]) → Matrix 行主序。
  std::array<double, 16> rows;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      rows[r * 4 + c] = v[c * 4 + r];
    }
  }
  *out = Matrix::FromRows(rows);
  return true;
}

void InstallPaintingClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                            v8::Local<v8::Object> exports) {
  ClassBuilder<JsPaint>(isolate, "Paint", [](const Args&) { return std::make_unique<JsPaint>(); })
      .Property("color", &PaintGetColor, &PaintSetColor)
      .Property("style", &PaintGetStyle, &PaintSetStyle)
      .Property("strokeWidth", &PaintGetStrokeWidth, &PaintSetStrokeWidth)
      .Property("strokeCap", &PaintGetStrokeCap, &PaintSetStrokeCap)
      .Property("strokeJoin", &PaintGetStrokeJoin, &PaintSetStrokeJoin)
      .Property("antiAlias", &PaintGetAntiAlias, &PaintSetAntiAlias)
      .Property("shader", &PaintGetShader, &PaintSetShader)
      .Property("filterQuality", &PaintGetFilterQuality, &PaintSetFilterQuality)
      .Property("maskBlurSigma", &PaintGetMaskBlur, &PaintSetMaskBlur)
      .Export(context, exports);

  ClassBuilder<JsGradient>(isolate, "Gradient", nullptr)
      .StaticMethod("linear", &GradientLinear)
      .StaticMethod("radial", &GradientRadial)
      .Export(context, exports);

  ClassBuilder<JsPath>(isolate, "Path", [](const Args&) { return std::make_unique<JsPath>(); })
      .Method("moveTo", &PathMoveTo)
      .Method("lineTo", &PathLineTo)
      .Method("quadTo", &PathQuadTo)
      .Method("cubicTo", &PathCubicTo)
      .Method("close", &PathClose)
      .Method("reset", &PathReset)
      .Method("addRect", &PathAddRect)
      .Method("addOval", &PathAddOval)
      .Method("addRRect", &PathAddRRect)
      .Property("fillType", &PathGetFillType, &PathSetFillType)
      .Property("bounds", &PathGetBounds)
      .Export(context, exports);

  ClassBuilder<JsPictureRecorder>(isolate, "PictureRecorder",
                                  [](const Args&) { return std::make_unique<JsPictureRecorder>(); })
      .Method("beginRecording", &RecorderBegin)
      .Method("endRecording", &RecorderEnd)
      .Property("isRecording", &RecorderIsRecording)
      .Export(context, exports);

  ClassBuilder<JsCanvas>(isolate, "Canvas", nullptr)
      .Method("save", &CanvasSave)
      .Method("restore", &CanvasRestore)
      .Method("getSaveCount", &CanvasGetSaveCount)
      .Method("translate", &CanvasTranslate)
      .Method("scale", &CanvasScale)
      .Method("rotate", &CanvasRotate)
      .Method("transform", &CanvasTransform)
      .Method("clipRect", &CanvasClipRect)
      .Method("clipRRect", &CanvasClipRRect)
      .Method("addHitRegion", &CanvasAddHitRegion)
      .Method("clipPath", &CanvasClipPath)
      .Method("saveLayer", &CanvasSaveLayer)
      .Method("drawDRRect", &CanvasDrawDRRect)
      .Method("drawImage", &CanvasDrawImage)
      .Method("drawImageRect", &CanvasDrawImageRect)
      .Method("drawParagraph", &CanvasDrawParagraph)
      .Method("drawTexture", &CanvasDrawTexture)
      .Method("drawColor", &CanvasDrawColor)
      .Method("drawPaint", &CanvasDrawPaint)
      .Method("drawRect", &CanvasDrawRect)
      .Method("drawRRect", &CanvasDrawRRect)
      .Method("drawOval", &CanvasDrawOval)
      .Method("drawCircle", &CanvasDrawCircle)
      .Method("drawLine", &CanvasDrawLine)
      .Method("drawPath", &CanvasDrawPath)
      .Method("drawPicture", &CanvasDrawPicture)
      .Export(context, exports);

  ClassBuilder<JsPicture>(isolate, "Picture", nullptr)
      .Property("opCount", &PictureOpCount)
      .Export(context, exports);
}

}  // namespace momo::engine::ui
