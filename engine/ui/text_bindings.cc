// momo:ui 文本类: ParagraphBuilder、Paragraph、loadFontFromBytes(docs/02 §4、M3)。
// 样式以普通 JS 对象传入(字段见 momo-ui.d.ts 的 TextStyle / ParagraphStyle), 未知字段忽略。

#include <cmath>
#include <string>
#include <utility>

#include "engine/runtime/class_builder.h"
#include "engine/runtime/js_args.h"
#include "engine/ui/ui_classes.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ArgNumber;
using runtime::ClassBuilder;
using runtime::NewWrapper;
using runtime::ThrowError;
using runtime::ThrowTypeError;
using runtime::ToUtf8;
using runtime::V8String;

const WrapperTypeInfo JsParagraph::kTypeInfo = {"Paragraph"};
const WrapperTypeInfo JsParagraphBuilder::kTypeInfo = {"ParagraphBuilder"};

namespace {

std::u16string ToUtf16(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::Local<v8::String> s;
  if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&s)) {
    return {};
  }
  std::u16string out(static_cast<size_t>(s->Length()), u'\0');
  s->WriteV2(isolate, 0, s->Length(), reinterpret_cast<uint16_t*>(out.data()));
  return out;
}

// 读取 obj[key]: 存在且非 undefined/null 时返回 true。
bool Field(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* key,
           v8::Local<v8::Value>* out) {
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  return obj->Get(context, V8String(isolate, key)).ToLocal(out) && !(*out)->IsNullOrUndefined();
}

template <typename T>
bool NumberField(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* key,
                 std::optional<T>* out) {
  v8::Local<v8::Value> v;
  if (!Field(isolate, obj, key, &v)) {
    return true;
  }
  if (!v->IsNumber() || !std::isfinite(v.As<v8::Number>()->Value())) {
    ThrowTypeError(isolate, std::string(key) + " must be a finite number");
    return false;
  }
  const double d = v.As<v8::Number>()->Value();
  if constexpr (std::is_same_v<T, uint32_t>) {
    *out = static_cast<uint32_t>(static_cast<int64_t>(d));
  } else {
    *out = static_cast<T>(d);
  }
  return true;
}

template <typename E>
bool EnumField(v8::Isolate* isolate, v8::Local<v8::Object> obj, const char* key,
               std::initializer_list<std::pair<const char*, E>> options, std::optional<E>* out) {
  v8::Local<v8::Value> v;
  if (!Field(isolate, obj, key, &v)) {
    return true;
  }
  const std::string s = ToUtf8(isolate, v);
  for (const auto& [name, e] : options) {
    if (s == name) {
      *out = e;
      return true;
    }
  }
  ThrowTypeError(isolate, std::string("invalid ") + key + ": '" + s + "'");
  return false;
}

bool ParseTextStyle(v8::Isolate* isolate, v8::Local<v8::Value> value, text::TextStyle* style) {
  if (value->IsNullOrUndefined()) {
    return true;
  }
  if (!value->IsObject()) {
    ThrowTypeError(isolate, "TextStyle must be an object");
    return false;
  }
  v8::Local<v8::Object> o = value.As<v8::Object>();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Value> v;
  if (Field(isolate, o, "fontFamilies", &v)) {
    if (!v->IsArray()) {
      ThrowTypeError(isolate, "fontFamilies must be an array of strings");
      return false;
    }
    std::vector<std::string> families;
    v8::Local<v8::Array> array = v.As<v8::Array>();
    for (uint32_t i = 0; i < array->Length(); ++i) {
      families.push_back(ToUtf8(isolate, array->Get(context, i).ToLocalChecked()));
    }
    style->font_families = std::move(families);
  }
  if (Field(isolate, o, "fontStyle", &v)) {
    style->italic = ToUtf8(isolate, v) == "italic";
  }
  if (Field(isolate, o, "locale", &v)) {
    style->locale = ToUtf8(isolate, v);
  }
  std::optional<double> weight;
  if (!NumberField(isolate, o, "color", &style->color) ||
      !NumberField(isolate, o, "fontSize", &style->font_size) ||
      !NumberField(isolate, o, "fontWeight", &weight) ||
      !NumberField(isolate, o, "letterSpacing", &style->letter_spacing) ||
      !NumberField(isolate, o, "wordSpacing", &style->word_spacing) ||
      !NumberField(isolate, o, "height", &style->height) ||
      !NumberField(isolate, o, "decorationColor", &style->decoration_color) ||
      !NumberField(isolate, o, "decorationThickness", &style->decoration_thickness) ||
      !NumberField(isolate, o, "backgroundColor", &style->background_color)) {
    return false;
  }
  if (weight) {
    style->font_weight = static_cast<int>(*weight);
  }
  using D = text::TextDecoration;
  using DS = text::TextDecorationStyle;
  using B = text::TextBaseline;
  return EnumField<D>(isolate, o, "decoration",
                      {{"none", D::kNone}, {"underline", D::kUnderline},
                       {"overline", D::kOverline}, {"lineThrough", D::kLineThrough}},
                      &style->decoration) &&
         EnumField<DS>(isolate, o, "decorationStyle",
                       {{"solid", DS::kSolid}, {"double", DS::kDouble}, {"dotted", DS::kDotted},
                        {"dashed", DS::kDashed}, {"wavy", DS::kWavy}},
                       &style->decoration_style) &&
         EnumField<B>(isolate, o, "textBaseline",
                      {{"alphabetic", B::kAlphabetic}, {"ideographic", B::kIdeographic}},
                      &style->baseline);
}

bool ParseParagraphStyle(v8::Isolate* isolate, v8::Local<v8::Value> value,
                         text::ParagraphStyle* style) {
  if (value->IsNullOrUndefined()) {
    return true;
  }
  if (!value->IsObject()) {
    ThrowTypeError(isolate, "ParagraphStyle must be an object");
    return false;
  }
  v8::Local<v8::Object> o = value.As<v8::Object>();
  using A = text::TextAlign;
  using Dir = text::TextDirection;
  std::optional<A> align;
  std::optional<Dir> direction;
  std::optional<double> max_lines;
  if (!EnumField<A>(isolate, o, "textAlign",
                    {{"left", A::kLeft}, {"right", A::kRight}, {"center", A::kCenter},
                     {"justify", A::kJustify}, {"start", A::kStart}, {"end", A::kEnd}},
                    &align) ||
      !EnumField<Dir>(isolate, o, "textDirection", {{"ltr", Dir::kLtr}, {"rtl", Dir::kRtl}},
                      &direction) ||
      !NumberField(isolate, o, "maxLines", &max_lines) ||
      !NumberField(isolate, o, "height", &style->height)) {
    return false;
  }
  if (align) style->align = *align;
  if (direction) style->direction = *direction;
  if (max_lines && *max_lines >= 1) style->max_lines = static_cast<size_t>(*max_lines);
  v8::Local<v8::Value> v;
  if (Field(isolate, o, "ellipsis", &v)) {
    style->ellipsis = ToUtf16(isolate, v);
  }
  if (Field(isolate, o, "textStyle", &v) && !ParseTextStyle(isolate, v, &style->text_style)) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ParagraphBuilder

std::unique_ptr<JsParagraphBuilder> ConstructBuilder(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  text::ParagraphStyle style;
  if (a.Length() > 0 && !ParseParagraphStyle(isolate, a[0], &style)) {
    return nullptr;
  }
  auto builder = std::make_unique<JsParagraphBuilder>();
  builder->builder = std::make_unique<text::ParagraphBuilder>(style, UiFonts(isolate));
  return builder;
}

text::ParagraphBuilder* LiveBuilder(JsParagraphBuilder& b, const Args& a) {
  if (b.builder == nullptr) {
    ThrowError(a.GetIsolate(), "ParagraphBuilder has already been built");
  }
  return b.builder.get();
}

void BuilderPushStyle(JsParagraphBuilder& b, const Args& a) {
  text::TextStyle style;
  if (auto* builder = LiveBuilder(b, a); builder && ParseTextStyle(a.GetIsolate(), a[0], &style)) {
    builder->PushStyle(style);
  }
}
void BuilderPop(JsParagraphBuilder& b, const Args& a) {
  if (auto* builder = LiveBuilder(b, a)) builder->Pop();
}
void BuilderAddText(JsParagraphBuilder& b, const Args& a) {
  if (auto* builder = LiveBuilder(b, a)) builder->AddText(ToUtf16(a.GetIsolate(), a[0]));
}
void BuilderBuild(JsParagraphBuilder& b, const Args& a) {
  auto* builder = LiveBuilder(b, a);
  if (builder == nullptr) {
    return;
  }
  auto paragraph = std::make_unique<JsParagraph>();
  paragraph->paragraph = builder->Build();
  b.builder.reset();
  a.GetReturnValue().Set(NewWrapper(a.GetIsolate(), std::move(paragraph)));
}

// ---------------------------------------------------------------------------
// Paragraph

text::Paragraph* Live(JsParagraph& p, const Args& a) {
  if (p.paragraph == nullptr) {
    ThrowError(a.GetIsolate(), "Paragraph has been disposed");
  }
  return p.paragraph.get();
}

// layout(width): 允许 +Infinity(不限宽, 用于测量固有宽度, 与 Flutter 一致);NaN/负数抛错。
void ParagraphLayout(JsParagraph& p, const Args& a) {
  text::Paragraph* para = Live(p, a);
  if (para == nullptr) return;
  if (a.Length() < 1 || !a[0]->IsNumber() || std::isnan(a[0].As<v8::Number>()->Value()) ||
      a[0].As<v8::Number>()->Value() < 0) {
    ThrowTypeError(a.GetIsolate(), "Paragraph.layout(width): width must be a non-negative number");
    return;
  }
  const double width = a[0].As<v8::Number>()->Value();
  para->Layout(std::isinf(width) ? 1e9 : width);  // SkParagraph 以极大有限值表示不限宽
}

#define MOMO_PARAGRAPH_GETTER(Name, expr)              \
  void Name(JsParagraph& p, const Args& a) {           \
    if (text::Paragraph* para = Live(p, a)) {          \
      a.GetReturnValue().Set(static_cast<double>(expr)); \
    }                                                  \
  }
MOMO_PARAGRAPH_GETTER(ParagraphWidth, para->width())
MOMO_PARAGRAPH_GETTER(ParagraphHeight, para->height())
MOMO_PARAGRAPH_GETTER(ParagraphLongestLine, para->longest_line())
MOMO_PARAGRAPH_GETTER(ParagraphMinIntrinsic, para->min_intrinsic_width())
MOMO_PARAGRAPH_GETTER(ParagraphMaxIntrinsic, para->max_intrinsic_width())
MOMO_PARAGRAPH_GETTER(ParagraphAlphabetic, para->alphabetic_baseline())
MOMO_PARAGRAPH_GETTER(ParagraphIdeographic, para->ideographic_baseline())
MOMO_PARAGRAPH_GETTER(ParagraphLineCount, para->line_count())
#undef MOMO_PARAGRAPH_GETTER

void ParagraphExceeded(JsParagraph& p, const Args& a) {
  if (text::Paragraph* para = Live(p, a)) a.GetReturnValue().Set(para->did_exceed_max_lines());
}

void ParagraphBoxes(JsParagraph& p, const Args& a) {
  double start = 0, end = 0;
  text::Paragraph* para = Live(p, a);
  if (!para || !ArgNumber(a, 0, &start) || !ArgNumber(a, 1, &end)) {
    return;
  }
  const std::vector<text::TextBox> boxes = para->GetBoxesForRange(
      static_cast<size_t>(std::max(0.0, start)), static_cast<size_t>(std::max(0.0, end)));
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, boxes.size() * 5 * 8);
  auto* out = static_cast<double*>(buffer->Data());
  for (const text::TextBox& box : boxes) {
    *out++ = box.rect.x;
    *out++ = box.rect.y;
    *out++ = box.rect.x + box.rect.width;
    *out++ = box.rect.y + box.rect.height;
    *out++ = box.direction == text::TextDirection::kLtr ? 1 : 0;
  }
  a.GetReturnValue().Set(v8::Float64Array::New(buffer, 0, boxes.size() * 5));
}

void ParagraphPositionForOffset(JsParagraph& p, const Args& a) {
  double x = 0, y = 0;
  text::Paragraph* para = Live(p, a);
  if (!para || !ArgNumber(a, 0, &x) || !ArgNumber(a, 1, &y)) {
    return;
  }
  const auto [offset, downstream] = para->GetPositionForOffset(x, y);
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(context, V8String(isolate, "offset"), v8::Number::New(isolate, static_cast<double>(offset)))
      .Check();
  o->Set(context, V8String(isolate, "affinity"),
         V8String(isolate, downstream ? "downstream" : "upstream"))
      .Check();
  a.GetReturnValue().Set(o);
}

void ParagraphWordBoundary(JsParagraph& p, const Args& a) {
  double offset = 0;
  text::Paragraph* para = Live(p, a);
  if (!para || !ArgNumber(a, 0, &offset)) {
    return;
  }
  const auto [start, end] = para->GetWordBoundary(static_cast<size_t>(std::max(0.0, offset)));
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  o->Set(context, V8String(isolate, "start"), v8::Number::New(isolate, static_cast<double>(start)))
      .Check();
  o->Set(context, V8String(isolate, "end"), v8::Number::New(isolate, static_cast<double>(end)))
      .Check();
  a.GetReturnValue().Set(o);
}

void ParagraphLineMetrics(JsParagraph& p, const Args& a) {
  text::Paragraph* para = Live(p, a);
  if (!para) {
    return;
  }
  v8::Isolate* isolate = a.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  const std::vector<text::LineMetrics> lines = para->GetLineMetrics();
  v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int>(lines.size()));
  for (size_t i = 0; i < lines.size(); ++i) {
    const text::LineMetrics& m = lines[i];
    v8::Local<v8::Object> o = v8::Object::New(isolate);
    auto set = [&](const char* k, double v) {
      o->Set(context, V8String(isolate, k), v8::Number::New(isolate, v)).Check();
    };
    set("startIndex", static_cast<double>(m.start_index));
    set("endIndex", static_cast<double>(m.end_index));
    o->Set(context, V8String(isolate, "hardBreak"), v8::Boolean::New(isolate, m.hard_break))
        .Check();
    set("ascent", m.ascent);
    set("descent", m.descent);
    set("height", m.height);
    set("width", m.width);
    set("left", m.left);
    set("baseline", m.baseline);
    set("lineNumber", static_cast<double>(m.line_number));
    array->Set(context, static_cast<uint32_t>(i), o).Check();
  }
  a.GetReturnValue().Set(array);
}

void ParagraphDispose(JsParagraph& p, const Args&) { p.paragraph.reset(); }

// loadFontFromBytes(bytes, family?) → 族名
void LoadFontFromBytes(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  std::vector<uint8_t> bytes;
  if (a.Length() > 0 && a[0]->IsArrayBuffer()) {
    auto buffer = a[0].As<v8::ArrayBuffer>();
    const auto* d = static_cast<const uint8_t*>(buffer->Data());
    bytes.assign(d, d + buffer->ByteLength());
  } else if (a.Length() > 0 && a[0]->IsArrayBufferView()) {
    auto view = a[0].As<v8::ArrayBufferView>();
    bytes.resize(view->ByteLength());
    view->CopyContents(bytes.data(), bytes.size());
  } else {
    ThrowTypeError(isolate, "loadFontFromBytes: bytes must be an ArrayBuffer or typed array");
    return;
  }
  const std::string family =
      a.Length() > 1 && !a[1]->IsNullOrUndefined() ? ToUtf8(isolate, a[1]) : std::string();
  StatusOr<std::string> name = UiFonts(isolate).RegisterFont(bytes.data(), bytes.size(), family);
  if (!name.ok()) {
    ThrowError(isolate, std::string(name.status().message()));
    return;
  }
  a.GetReturnValue().Set(V8String(isolate, *name));
}

}  // namespace

void InstallTextClasses(v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Object> exports) {
  ClassBuilder<JsParagraphBuilder>(isolate, "ParagraphBuilder", &ConstructBuilder)
      .Method("pushStyle", &BuilderPushStyle)
      .Method("pop", &BuilderPop)
      .Method("addText", &BuilderAddText)
      .Method("build", &BuilderBuild)
      .Export(context, exports);

  ClassBuilder<JsParagraph>(isolate, "Paragraph", nullptr)
      .Method("layout", &ParagraphLayout)
      .Property("width", &ParagraphWidth)
      .Property("height", &ParagraphHeight)
      .Property("longestLine", &ParagraphLongestLine)
      .Property("minIntrinsicWidth", &ParagraphMinIntrinsic)
      .Property("maxIntrinsicWidth", &ParagraphMaxIntrinsic)
      .Property("alphabeticBaseline", &ParagraphAlphabetic)
      .Property("ideographicBaseline", &ParagraphIdeographic)
      .Property("didExceedMaxLines", &ParagraphExceeded)
      .Property("lineCount", &ParagraphLineCount)
      .Method("getBoxesForRange", &ParagraphBoxes)
      .Method("getPositionForOffset", &ParagraphPositionForOffset)
      .Method("getWordBoundary", &ParagraphWordBoundary)
      .Method("getLineMetrics", &ParagraphLineMetrics)
      .Method("dispose", &ParagraphDispose)
      .Export(context, exports);

  exports->Set(context, V8String(isolate, "loadFontFromBytes"),
               v8::Function::New(context, &LoadFontFromBytes).ToLocalChecked())
      .Check();
}

}  // namespace momo::engine::ui
