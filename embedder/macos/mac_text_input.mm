#include "embedder/macos/mac_text_input.h"

#import <Cocoa/Cocoa.h>

#include <algorithm>

namespace momo::embedder::macos {

namespace {

NSString* PlainString(id string) {
  if ([string isKindOfClass:[NSAttributedString class]]) {
    return [(NSAttributedString*)string string];
  }
  return [string isKindOfClass:[NSString class]] ? (NSString*)string : @"";
}

std::string ToUtf8(NSString* string) {
  const char* utf8 = string.UTF8String;
  return utf8 ? std::string(utf8) : std::string();
}

bool IsValidRange(NSRange range) { return range.location != NSNotFound; }

}  // namespace

MacTextInput::MacTextInput(NSView* view, WindowDelegate* delegate)
    : view_(view), delegate_(delegate), text_([NSMutableString string]) {}

MacTextInput::~MacTextInput() = default;

void MacTextInput::Show() {
  active_ = true;
  [view_.inputContext activate];
}

void MacTextInput::Hide() {
  if (!active_) {
    return;
  }
  // 先失活: discardMarkedText 期间输入法可能回调 unmarkText/setMarkedText, 此时不再上报也不改本地状态。
  active_ = false;
  if (IsValidRange(marked_)) {
    marked_ = NSMakeRange(NSNotFound, 0);
    [view_.inputContext discardMarkedText];  // 让输入法放弃未完成的预编辑
  }
}

void MacTextInput::SetCaretRect(Rect physical_rect) {
  caret_rect_ = physical_rect;
  if (active_) {
    [view_.inputContext invalidateCharacterCoordinates];
  }
}

void MacTextInput::SetEditingState(const platform::TextEditingState& state) {
  NSString* text = [[NSString alloc] initWithBytes:state.text.data()
                                            length:state.text.size()
                                          encoding:NSUTF8StringEncoding];
  [text_ setString:text ?: @""];
  const NSUInteger length = text_.length;
  auto clamp = [length](int32_t index) {
    return static_cast<NSUInteger>(std::clamp<int64_t>(index, 0, static_cast<int64_t>(length)));
  };
  const NSUInteger base = clamp(state.selection_base);
  const NSUInteger extent = clamp(state.selection_extent);
  selection_ = NSMakeRange(std::min(base, extent), std::max(base, extent) - std::min(base, extent));
  if (state.composing_start >= 0 && state.composing_end >= state.composing_start) {
    const NSUInteger start = clamp(state.composing_start);
    marked_ = NSMakeRange(start, clamp(state.composing_end) - start);
  } else {
    marked_ = NSMakeRange(NSNotFound, 0);
  }
  if (active_) {
    [view_.inputContext invalidateCharacterCoordinates];
  }
}

NSRange MacTextInput::ClampRange(NSRange range) const {
  const NSUInteger length = text_.length;
  if (!IsValidRange(range)) {
    return NSMakeRange(NSNotFound, 0);
  }
  const NSUInteger start = std::min(range.location, length);
  const NSUInteger end = std::min(start + range.length, length);
  return NSMakeRange(start, end - start);
}

// 输入法要替换的区间: 显式 replacement_range > 当前 marked 区间 > 当前选区。
NSRange MacTextInput::TargetRange(NSRange replacement_range) const {
  if (IsValidRange(replacement_range)) {
    return ClampRange(replacement_range);
  }
  if (IsValidRange(marked_)) {
    return ClampRange(marked_);
  }
  return ClampRange(selection_);
}

void MacTextInput::Emit(const platform::TextInputEvent& event) {
  if (active_ && delegate_ != nullptr) {
    delegate_->OnTextInput(event);
  }
}

void MacTextInput::InsertText(id string, NSRange replacement_range) {
  if (!active_) {
    return;
  }
  NSString* inserted = PlainString(string);
  const NSRange target = TargetRange(replacement_range);
  [text_ replaceCharactersInRange:target withString:inserted];
  selection_ = NSMakeRange(target.location + inserted.length, 0);
  marked_ = NSMakeRange(NSNotFound, 0);
  platform::TextInputEvent event;
  event.type = platform::TextInputEvent::Type::kCommit;
  event.text = ToUtf8(inserted);
  if (IsValidRange(replacement_range)) {  // 显式区间(重音字符/重新转换)原样告知 JS
    event.replace_start = static_cast<int32_t>(target.location);
    event.replace_end = static_cast<int32_t>(NSMaxRange(target));
  }
  Emit(event);
}

void MacTextInput::SetMarkedText(id string, NSRange selected_range, NSRange replacement_range) {
  if (!active_) {
    return;
  }
  NSString* marked = PlainString(string);
  const NSRange target = TargetRange(replacement_range);
  [text_ replaceCharactersInRange:target withString:marked];
  platform::TextInputEvent event;
  if (marked.length == 0) {
    // 输入法清空了预编辑(如退格删光拼音): 结束 composing。
    marked_ = NSMakeRange(NSNotFound, 0);
    selection_ = NSMakeRange(target.location, 0);
    event.type = platform::TextInputEvent::Type::kComposingEnd;
  } else {
    marked_ = NSMakeRange(target.location, marked.length);
    const NSUInteger cursor = std::min<NSUInteger>(
        IsValidRange(selected_range) ? selected_range.location : marked.length, marked.length);
    const NSUInteger sel_len =
        IsValidRange(selected_range) ? std::min(selected_range.length, marked.length - cursor) : 0;
    selection_ = NSMakeRange(target.location + cursor, sel_len);
    event.type = platform::TextInputEvent::Type::kComposing;
    event.text = ToUtf8(marked);
    event.cursor = static_cast<int32_t>(cursor);
    if (IsValidRange(replacement_range)) {
      event.replace_start = static_cast<int32_t>(target.location);
      event.replace_end = static_cast<int32_t>(NSMaxRange(target));
    }
  }
  Emit(event);
}

void MacTextInput::UnmarkText() {
  if (!active_ || !IsValidRange(marked_)) {
    return;
  }
  // Cocoa 语义: 按原样接受当前预编辑文本。
  const NSRange marked = ClampRange(marked_);
  NSString* text = [text_ substringWithRange:marked];
  marked_ = NSMakeRange(NSNotFound, 0);
  selection_ = NSMakeRange(NSMaxRange(marked), 0);
  platform::TextInputEvent event;
  event.type = platform::TextInputEvent::Type::kCommit;
  event.text = ToUtf8(text);
  Emit(event);
}

bool MacTextInput::HasMarkedText() const { return IsValidRange(marked_) && marked_.length > 0; }

NSRange MacTextInput::MarkedRange() const {
  return HasMarkedText() ? ClampRange(marked_) : NSMakeRange(NSNotFound, 0);
}

NSRange MacTextInput::SelectedRange() const { return ClampRange(selection_); }

NSAttributedString* MacTextInput::AttributedSubstring(NSRange range,
                                                      NSRangePointer actual_range) const {
  const NSRange clamped = ClampRange(range);
  if (!IsValidRange(clamped)) {
    return nil;
  }
  if (actual_range != nullptr) {
    *actual_range = clamped;
  }
  return [[NSAttributedString alloc] initWithString:[text_ substringWithRange:clamped]];
}

NSRect MacTextInput::FirstRectForCharacterRange(NSRange range, NSRangePointer actual_range) const {
  if (actual_range != nullptr) {
    *actual_range = ClampRange(range);
  }
  NSView* view = view_;
  NSWindow* window = view.window;
  if (view == nil || window == nil) {
    return NSZeroRect;
  }
  // 物理像素(左上原点)→ 视图点坐标(左下原点)→ 窗口 → 屏幕。
  const CGFloat scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1.0;
  const NSRect bounds = view.bounds;
  const CGFloat x = caret_rect_.x / scale;
  const CGFloat w = caret_rect_.width / scale;
  const CGFloat h = caret_rect_.height / scale;
  const CGFloat y = bounds.size.height - caret_rect_.y / scale - h;
  const NSRect in_view = NSMakeRect(x, y, w, h);
  const NSRect in_window = [view convertRect:in_view toView:nil];
  return [window convertRectToScreen:in_window];
}

void MacTextInput::DoCommand(SEL selector) {
  if (!active_) {
    return;
  }
  NSString* name = NSStringFromSelector(selector);
  if ([name hasSuffix:@":"]) {
    name = [name substringToIndex:name.length - 1];
  }
  if ([name isEqualToString:@"noop"]) {
    return;  // 未绑定任何命令的按键(如 Cmd+字母): 快捷键由 platform::KeyData 通道处理
  }
  platform::TextInputEvent event;
  event.type = platform::TextInputEvent::Type::kCommand;
  event.command = ToUtf8(name);
  Emit(event);
}

}  // namespace momo::embedder::macos
