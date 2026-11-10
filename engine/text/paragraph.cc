#include "engine/text/paragraph.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/graphics/skia/paint_context.h"
#include "engine/text/text_internal.h"
#include "include/core/SkPictureRecorder.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"

namespace momo::engine::text {

namespace tl = skia::textlayout;

namespace {

tl::TextAlign ToSk(TextAlign a) {
  switch (a) {
    case TextAlign::kLeft: return tl::TextAlign::kLeft;
    case TextAlign::kRight: return tl::TextAlign::kRight;
    case TextAlign::kCenter: return tl::TextAlign::kCenter;
    case TextAlign::kJustify: return tl::TextAlign::kJustify;
    case TextAlign::kStart: return tl::TextAlign::kStart;
    case TextAlign::kEnd: return tl::TextAlign::kEnd;
  }
  return tl::TextAlign::kStart;
}

tl::TextDirection ToSk(TextDirection d) {
  return d == TextDirection::kRtl ? tl::TextDirection::kRtl : tl::TextDirection::kLtr;
}

tl::TextDecoration ToSk(TextDecoration d) {
  switch (d) {
    case TextDecoration::kNone: return tl::TextDecoration::kNoDecoration;
    case TextDecoration::kUnderline: return tl::TextDecoration::kUnderline;
    case TextDecoration::kOverline: return tl::TextDecoration::kOverline;
    case TextDecoration::kLineThrough: return tl::TextDecoration::kLineThrough;
  }
  return tl::TextDecoration::kNoDecoration;
}

tl::TextDecorationStyle ToSk(TextDecorationStyle s) {
  switch (s) {
    case TextDecorationStyle::kSolid: return tl::TextDecorationStyle::kSolid;
    case TextDecorationStyle::kDouble: return tl::TextDecorationStyle::kDouble;
    case TextDecorationStyle::kDotted: return tl::TextDecorationStyle::kDotted;
    case TextDecorationStyle::kDashed: return tl::TextDecorationStyle::kDashed;
    case TextDecorationStyle::kWavy: return tl::TextDecorationStyle::kWavy;
  }
  return tl::TextDecorationStyle::kSolid;
}

// 把 style 中设置了的字段叠加到 out 上。
void Apply(const TextStyle& style, tl::TextStyle* out) {
  if (style.color) out->setColor(*style.color);
  if (style.font_families) {
    std::vector<SkString> families;
    for (const std::string& f : *style.font_families) {
      families.emplace_back(f.c_str());
    }
    out->setFontFamilies(std::move(families));
  }
  if (style.font_size) out->setFontSize(static_cast<SkScalar>(*style.font_size));
  if (style.font_weight || style.italic) {
    const SkFontStyle current = out->getFontStyle();
    const int weight = style.font_weight ? std::clamp(*style.font_weight, 100, 1000)
                                         : current.weight();
    const SkFontStyle::Slant slant =
        style.italic ? (*style.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant)
                     : current.slant();
    out->setFontStyle(SkFontStyle(weight, current.width(), slant));
  }
  if (style.letter_spacing) out->setLetterSpacing(static_cast<SkScalar>(*style.letter_spacing));
  if (style.word_spacing) out->setWordSpacing(static_cast<SkScalar>(*style.word_spacing));
  if (style.height) {
    out->setHeight(static_cast<SkScalar>(*style.height));
    out->setHeightOverride(true);
  }
  if (style.decoration) out->setDecoration(ToSk(*style.decoration));
  if (style.decoration_color) out->setDecorationColor(*style.decoration_color);
  if (style.decoration_style) out->setDecorationStyle(ToSk(*style.decoration_style));
  if (style.decoration_thickness) {
    out->setDecorationThicknessMultiplier(static_cast<SkScalar>(*style.decoration_thickness));
  }
  if (style.background_color) {
    SkPaint paint;
    paint.setColor(*style.background_color);
    out->setBackgroundPaint(paint);
  }
  if (style.baseline) {
    out->setTextBaseline(*style.baseline == TextBaseline::kIdeographic
                             ? tl::TextBaseline::kIdeographic
                             : tl::TextBaseline::kAlphabetic);
  }
  if (style.locale) out->setLocale(SkString(style.locale->c_str()));
}

// UTF-8 下标 → UTF-16 下标(SkParagraph 行度量以 UTF-8 计)。
std::vector<size_t> Utf8ToUtf16Map(const std::u16string& text) {
  std::vector<size_t> map;
  map.reserve(text.size() * 3 + 1);
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = text[i];
    size_t bytes;
    if (c < 0x80) {
      bytes = 1;
    } else if (c < 0x800) {
      bytes = 2;
    } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
      bytes = 4;  // 代理对: 4 个 UTF-8 字节对应 2 个 UTF-16 码元
      for (size_t b = 0; b < bytes; ++b) {
        map.push_back(i);
      }
      ++i;
      continue;
    } else {
      bytes = 3;
    }
    for (size_t b = 0; b < bytes; ++b) {
      map.push_back(i);
    }
  }
  map.push_back(text.size());
  return map;
}

}  // namespace

struct Paragraph::Impl {
  std::unique_ptr<tl::Paragraph> paragraph;
  std::u16string text;
  double layout_width = 0;
  bool laid_out = false;
};

struct ParagraphBuilder::Impl {
  std::unique_ptr<tl::ParagraphBuilder> builder;
  std::vector<tl::TextStyle> stack;
  std::u16string text;
};

// ---------------------------------------------------------------------------

ParagraphBuilder::ParagraphBuilder(const ParagraphStyle& style, FontCollection& fonts)
    : impl_(std::make_unique<Impl>()) {
  tl::ParagraphStyle ps;
  tl::TextStyle base;
  base.setFontSize(14);
  base.setColor(SK_ColorBLACK);
  Apply(style.text_style, &base);
  ps.setTextStyle(base);
  ps.setTextAlign(ToSk(style.align));
  ps.setTextDirection(ToSk(style.direction));
  if (style.max_lines && *style.max_lines > 0) {
    ps.setMaxLines(*style.max_lines);
  }
  if (!style.ellipsis.empty()) {
    ps.setEllipsis(style.ellipsis);
  }
  if (style.height) {
    tl::StrutStyle strut;
    strut.setStrutEnabled(true);
    strut.setHeight(static_cast<SkScalar>(*style.height));
    strut.setHeightOverride(true);
    if (style.text_style.font_size) {
      strut.setFontSize(static_cast<SkScalar>(*style.text_style.font_size));
    }
    ps.setStrutStyle(strut);
  }
  FontCollection::Impl& f = fonts.impl();
  impl_->builder = tl::ParagraphBuilder::make(ps, f.collection, f.unicode);
  impl_->stack.push_back(base);
}

ParagraphBuilder::~ParagraphBuilder() = default;

void ParagraphBuilder::PushStyle(const TextStyle& style) {
  tl::TextStyle next = impl_->stack.back();
  Apply(style, &next);
  impl_->stack.push_back(next);
  impl_->builder->pushStyle(next);
}

void ParagraphBuilder::Pop() {
  if (impl_->stack.size() > 1) {
    impl_->stack.pop_back();
    impl_->builder->pop();
  }
}

void ParagraphBuilder::AddText(const std::u16string& text) {
  impl_->text += text;
  impl_->builder->addText(text);
}

std::unique_ptr<Paragraph> ParagraphBuilder::Build() {
  auto impl = std::make_unique<Paragraph::Impl>();
  impl->paragraph = impl_->builder->Build();
  impl->text = std::move(impl_->text);
  return std::make_unique<Paragraph>(std::move(impl));
}

// ---------------------------------------------------------------------------

Paragraph::Paragraph(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Paragraph::~Paragraph() = default;

void Paragraph::Layout(double width) {
  if (impl_->laid_out && width == impl_->layout_width) {
    return;
  }
  impl_->paragraph->layout(static_cast<SkScalar>(width));
  impl_->layout_width = width;
  impl_->laid_out = true;
  recorded_.reset();
}

double Paragraph::width() const { return impl_->paragraph->getMaxWidth(); }
double Paragraph::height() const { return impl_->paragraph->getHeight(); }
double Paragraph::longest_line() const { return impl_->paragraph->getLongestLine(); }
double Paragraph::min_intrinsic_width() const { return impl_->paragraph->getMinIntrinsicWidth(); }
double Paragraph::max_intrinsic_width() const { return impl_->paragraph->getMaxIntrinsicWidth(); }
double Paragraph::alphabetic_baseline() const { return impl_->paragraph->getAlphabeticBaseline(); }
double Paragraph::ideographic_baseline() const {
  return impl_->paragraph->getIdeographicBaseline();
}
bool Paragraph::did_exceed_max_lines() const { return impl_->paragraph->didExceedMaxLines(); }
size_t Paragraph::line_count() const { return impl_->paragraph->lineNumber(); }

std::vector<TextBox> Paragraph::GetBoxesForRange(size_t start, size_t end) const {
  std::vector<TextBox> out;
  if (end <= start) {
    return out;
  }
  for (const tl::TextBox& box : impl_->paragraph->getRectsForRange(
           static_cast<unsigned>(start), static_cast<unsigned>(end),
           tl::RectHeightStyle::kMax, tl::RectWidthStyle::kTight)) {
    out.push_back({{box.rect.left(), box.rect.top(), box.rect.width(), box.rect.height()},
                   box.direction == tl::TextDirection::kRtl ? TextDirection::kRtl
                                                            : TextDirection::kLtr});
  }
  return out;
}

std::pair<size_t, bool> Paragraph::GetPositionForOffset(double x, double y) const {
  const tl::PositionWithAffinity p = impl_->paragraph->getGlyphPositionAtCoordinate(
      static_cast<SkScalar>(x), static_cast<SkScalar>(y));
  const size_t pos = static_cast<size_t>(std::max<int32_t>(0, p.position));
  return {std::min(pos, impl_->text.size()), p.affinity == tl::Affinity::kDownstream};
}

std::pair<size_t, size_t> Paragraph::GetWordBoundary(size_t offset) const {
  const tl::SkRange<size_t> r = impl_->paragraph->getWordBoundary(static_cast<unsigned>(offset));
  return {std::min(r.start, impl_->text.size()), std::min(r.end, impl_->text.size())};
}

std::vector<LineMetrics> Paragraph::GetLineMetrics() const {
  std::vector<tl::LineMetrics> sk;
  impl_->paragraph->getLineMetrics(sk);
  const std::vector<size_t> map = Utf8ToUtf16Map(impl_->text);
  auto to16 = [&map](size_t i) { return map[std::min(i, map.size() - 1)]; };
  std::vector<LineMetrics> out;
  out.reserve(sk.size());
  for (const tl::LineMetrics& m : sk) {
    LineMetrics lm;
    lm.start_index = to16(m.fStartIndex);
    lm.end_index = to16(m.fEndIndex);
    lm.hard_break = m.fHardBreak;
    lm.ascent = m.fAscent;
    lm.descent = m.fDescent;
    lm.height = m.fHeight;
    lm.width = m.fWidth;
    lm.left = m.fLeft;
    lm.baseline = m.fBaseline;
    lm.line_number = m.fLineNumber;
    out.push_back(lm);
  }
  return out;
}

Rect Paragraph::paint_bounds() const {
  // 斜体/装饰线/阴影可能略超出排版盒: 外扩一个保守余量。
  const double w = std::max(impl_->paragraph->getMaxWidth(), impl_->paragraph->getLongestLine());
  const double h = impl_->paragraph->getHeight();
  const double pad = 4 + 0.25 * (h / std::max<size_t>(1, impl_->paragraph->lineNumber()));
  return Rect{0, 0, w, h}.Outset(pad);
}

std::shared_ptr<const graphics::ExternalPicture> Paragraph::Record() const {
  if (recorded_ == nullptr) {
    SkPictureRecorder recorder;
    const Rect b = paint_bounds();
    SkCanvas* canvas = recorder.beginRecording(SkRect::MakeXYWH(
        static_cast<SkScalar>(b.x), static_cast<SkScalar>(b.y), static_cast<SkScalar>(b.width),
        static_cast<SkScalar>(b.height)));
    impl_->paragraph->paint(canvas, 0, 0);
    auto picture = std::make_shared<graphics::ExternalPicture>();
    picture->picture = recorder.finishRecordingAsPicture();
    recorded_ = std::move(picture);
  }
  return recorded_;
}

}  // namespace momo::engine::text
