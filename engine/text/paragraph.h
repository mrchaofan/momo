#pragma once

// 段落排版: SkParagraph 的薄封装(docs/02 §6、docs/05 §6)。公共头不含 Skia 类型。
// 文本以 UTF-16 传入, 所有下标为 UTF-16 码元下标(与 JS 字符串一致)。
// 绘制: Paragraph::Record() 把当前排版结果录制为不可变的 graphics::ExternalPicture(在 ui 线程),
// 记录进 DisplayList 后由 raster 线程重放——raster 线程从不触碰可变的段落对象。
// 线程约定: ParagraphBuilder / Paragraph 仅在 ui 线程使用。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "base/macros.h"
#include "engine/graphics/display_list.h"
#include "engine/text/font_collection.h"

namespace momo::engine::text {

enum class TextAlign : uint8_t { kLeft, kRight, kCenter, kJustify, kStart, kEnd };
enum class TextDirection : uint8_t { kRtl, kLtr };
enum class TextDecoration : uint8_t { kNone, kUnderline, kOverline, kLineThrough };
enum class TextDecorationStyle : uint8_t { kSolid, kDouble, kDotted, kDashed, kWavy };
enum class TextBaseline : uint8_t { kAlphabetic, kIdeographic };

// 未设置的字段继承父样式(pushStyle 叠加语义)。
struct TextStyle {
  std::optional<uint32_t> color;  // 0xAARRGGBB
  std::optional<std::vector<std::string>> font_families;
  std::optional<double> font_size;
  std::optional<int> font_weight;  // 100..900
  std::optional<bool> italic;
  std::optional<double> letter_spacing;
  std::optional<double> word_spacing;
  std::optional<double> height;  // 行高倍数
  std::optional<TextDecoration> decoration;
  std::optional<uint32_t> decoration_color;
  std::optional<TextDecorationStyle> decoration_style;
  std::optional<double> decoration_thickness;
  std::optional<uint32_t> background_color;
  std::optional<TextBaseline> baseline;
  std::optional<std::string> locale;
};

struct ParagraphStyle {
  TextAlign align = TextAlign::kStart;
  TextDirection direction = TextDirection::kLtr;
  std::optional<size_t> max_lines;
  std::u16string ellipsis;
  TextStyle text_style;
  std::optional<double> height;  // strut 行高倍数
};

struct TextBox {
  Rect rect;
  TextDirection direction;
};

struct LineMetrics {
  size_t start_index = 0;
  size_t end_index = 0;
  bool hard_break = false;
  double ascent = 0;
  double descent = 0;
  double height = 0;
  double width = 0;
  double left = 0;
  double baseline = 0;
  size_t line_number = 0;
};

class Paragraph {
 public:
  struct Impl;
  explicit Paragraph(std::unique_ptr<Impl> impl);
  ~Paragraph();

  void Layout(double width);
  double width() const;
  double height() const;
  double longest_line() const;
  double min_intrinsic_width() const;
  double max_intrinsic_width() const;
  double alphabetic_baseline() const;
  double ideographic_baseline() const;
  bool did_exceed_max_lines() const;
  size_t line_count() const;

  std::vector<TextBox> GetBoxesForRange(size_t start, size_t end) const;
  // 返回 (offset, downstream?)。
  std::pair<size_t, bool> GetPositionForOffset(double x, double y) const;
  std::pair<size_t, size_t> GetWordBoundary(size_t offset) const;
  std::vector<LineMetrics> GetLineMetrics() const;

  // 当前排版结果的不可变快照(同一次排版内缓存)。
  std::shared_ptr<const graphics::ExternalPicture> Record() const;
  // 绘制范围(本地坐标, 含少量外扩以容纳斜体/装饰线)。
  Rect paint_bounds() const;

 private:
  std::unique_ptr<Impl> impl_;
  mutable std::shared_ptr<const graphics::ExternalPicture> recorded_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Paragraph);
};

class ParagraphBuilder {
 public:
  struct Impl;
  // fonts 必须比 builder 及其产出的段落活得久。
  ParagraphBuilder(const ParagraphStyle& style, FontCollection& fonts);
  ~ParagraphBuilder();

  void PushStyle(const TextStyle& style);  // 在当前样式上叠加
  void Pop();
  void AddText(const std::u16string& text);
  std::unique_ptr<Paragraph> Build();

 private:
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(ParagraphBuilder);
};

}  // namespace momo::engine::text
