#pragma once

// WinTextInput: TextInputConnection 的 IMM32 实现(docs/03 §3, embedder/common/text_input.h)。
// 仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
// 分工: WinWindow 的 WndProc 把 IME 消息、WM_CHAR、WM_KEYDOWN 转给本对象;本对象把输入法动作翻译为
// TextInputEvent 经 WindowDelegate::OnTextInput 上报(只在 Show 之后, 与 mac 一致)。
//   - Show / Hide: ImmAssociateContextEx(hwnd, NULL, IACE_DEFAULT) 恢复默认输入上下文 / (NULL, 0) 解除关联
//     (= 禁用输入法)。窗口创建时即解除关联: 非文本焦点时按键不会被输入法吃掉。Hide 时取消未完成的预编辑。
//   - WM_IME_STARTCOMPOSITION / COMPOSITION / ENDCOMPOSITION:
//       GCS_RESULTSTR → kCommit;GCS_COMPSTR → kComposing(cursor = GCS_CURSORPOS, UTF-16);
//       预编辑被清空 / 结束且未提交 → kComposingEnd。这些消息不交给 DefWindowProc(否则系统画默认
//       组合窗并生成 WM_IME_CHAR → WM_CHAR, 造成重复提交);WM_IME_SETCONTEXT 去掉
//       ISC_SHOWUICOMPOSITIONWINDOW(预编辑由 JS 绘制), 候选窗仍由输入法绘制。
//   - 候选窗定位: SetCaretRect(物理像素, 相对客户区;Per-Monitor-V2 下客户区坐标即物理像素)→
//     ImmSetCompositionWindow(CFS_POINT)+ ImmSetCandidateWindow(CFS_CANDIDATEPOS 与 CFS_EXCLUDE)+
//     不可见的系统 caret(CreateCaret/SetCaretPos, 新版微软拼音/日文输入法与辅助工具跟随它)+
//     WM_IME_REQUEST/IMR_QUERYCHARPOSITION 应答(屏幕坐标)。
//   - 非 IME 输入: Show 期间 WM_CHAR 的可打印字符(含代理对拼接)→ kCommit;控制字符(退格/回车/Tab/
//     Ctrl+字母等产生的 WM_CHAR)忽略。
//   - 编辑键(退格/Delete/方向键/Home/End/回车/Tab/全选等)只以 KeyData 送达框架, 本对象**不**为它们发
//     kCommand(embedder/common/text_input.h 契约: 否则框架执行两次)。Windows 端目前没有发 kCommand 的来源。
//   - SetEditingState: 记录(供日后的重新转换 IMR_RECONVERTSTRING 使用;当前未实现)。
// 线程约定: 只在 platform 线程使用。生命周期: 由 WinWindow 拥有, HWND 比本对象活得久。

#include <string>

#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/windows/win_util.h"
#include "embedder/common/text_input.h"

namespace momo::embedder::windows {

class WinTextInput final : public TextInputConnection {
 public:
  WinTextInput(HWND hwnd, WindowDelegate* delegate);
  ~WinTextInput() override;

  // TextInputConnection
  void Show() override;
  void Hide() override;
  void SetCaretRect(Rect physical_rect) override;
  void SetEditingState(const TextEditingState& state) override;

  bool active() const { return active_; }
  bool composing() const { return composing_; }

  // WndProc 转发的 WM_IME_* 消息。返回 true 表示已处理(*result 为 WndProc 返回值),
  // false 表示交给 DefWindowProc。
  bool HandleImeMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result);
  // WM_CHAR(UTF-16 码元)。
  void OnChar(wchar_t code_unit);
  // 窗口焦点变化(系统 caret 只能属于有焦点的窗口)。
  void OnFocusChanged(bool focused);

 private:
  void Emit(const TextInputEvent& event);
  void UpdateImePosition();
  void DestroySystemCaret();

  HWND hwnd_;
  WindowDelegate* delegate_;
  bool active_ = false;
  bool composing_ = false;     // 已发出非空 kComposing 且尚未提交/结束
  bool caret_created_ = false;
  wchar_t pending_high_surrogate_ = 0;
  Rect caret_rect_;            // 物理像素, 相对客户区左上角
  TextEditingState state_;
};

}  // namespace momo::embedder::windows
