#pragma once

// MacTextInput: TextInputConnection 的 macOS 实现(docs/03 §3, text_input_connection.h)。仅被 .mm 包含。
//
// 分工: MomoContentView 实现 NSTextInputClient 协议并把每个调用转给本对象;本对象维护 IME 需要同步
// 查询的文本状态(文本 / 选区 / marked 区间, 均为 UTF-16 下标, 与 NSString 一致), 并把输入法动作翻译为
// TextInputEvent 经 WindowDelegate::OnTextInput 上报。
//   - Show 之后视图的 keyDown 才走 interpretKeyEvents(输入法);Hide 后不走, 并丢弃未完成的 marked text。
//   - JS 是编辑模型的权威: SetEditingState 覆盖本地状态;两次同步之间, 输入法事件先在本地乐观应用,
//     保证 hasMarkedText/markedRange/selectedRange 等同步查询立即反映最新的预编辑。
//   - SetCaretRect(物理像素, 相对内容区左上角)用于 firstRectForCharacterRange(屏幕坐标, 候选窗定位)。
// 线程约定: 只在 platform 线程(主线程)使用。生命周期: 由 MacWindow 拥有, 只弱引用视图。

#include <string>

#import <Foundation/Foundation.h>

#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/text_input.h"

@class NSView;

namespace momo::embedder::macos {

class MacTextInput final : public TextInputConnection {
 public:
  MacTextInput(NSView* view, WindowDelegate* delegate);
  ~MacTextInput() override;

  // TextInputConnection
  void Show() override;
  void Hide() override;
  void SetCaretRect(Rect physical_rect) override;
  void SetEditingState(const TextEditingState& state) override;

  bool active() const { return active_; }

  // NSTextInputClient(由视图转发)。
  void InsertText(id string, NSRange replacement_range);
  void SetMarkedText(id string, NSRange selected_range, NSRange replacement_range);
  void UnmarkText();
  bool HasMarkedText() const;
  NSRange MarkedRange() const;
  NSRange SelectedRange() const;
  NSAttributedString* AttributedSubstring(NSRange range, NSRangePointer actual_range) const;
  NSRect FirstRectForCharacterRange(NSRange range, NSRangePointer actual_range) const;
  void DoCommand(SEL selector);

 private:
  NSRange ClampRange(NSRange range) const;
  NSRange TargetRange(NSRange replacement_range) const;
  void Emit(const TextInputEvent& event);

  NSView* __weak view_;
  WindowDelegate* delegate_;
  bool active_ = false;
  NSMutableString* text_;
  NSRange selection_ = NSMakeRange(0, 0);
  NSRange marked_ = NSMakeRange(NSNotFound, 0);
  Rect caret_rect_;  // 物理像素, 相对内容区左上角
};

}  // namespace momo::embedder::macos
