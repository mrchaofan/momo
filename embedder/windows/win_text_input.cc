// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_text_input.h"

#include <imm.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace momo::embedder::windows {

namespace {

std::wstring GetCompositionString(HIMC himc, DWORD index) {
  const LONG bytes = ::ImmGetCompositionStringW(himc, index, nullptr, 0);
  if (bytes <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
  ::ImmGetCompositionStringW(himc, index, out.data(), static_cast<DWORD>(bytes));
  return out;
}

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

LONG ToLong(double value) { return static_cast<LONG>(std::lround(value)); }

}  // namespace

WinTextInput::WinTextInput(HWND hwnd, WindowDelegate* delegate) : hwnd_(hwnd), delegate_(delegate) {
  // 默认不在文本输入会话中: 解除输入上下文关联(禁用输入法), Show 时再恢复。
  ::ImmAssociateContextEx(hwnd_, nullptr, 0);
}

WinTextInput::~WinTextInput() { DestroySystemCaret(); }

void WinTextInput::Show() {
  if (!active_) {
    active_ = true;
    pending_high_surrogate_ = 0;
    ::ImmAssociateContextEx(hwnd_, nullptr, IACE_DEFAULT);
  }
  UpdateImePosition();
}

void WinTextInput::Hide() {
  if (!active_) {
    return;
  }
  // 先失活: 取消预编辑期间输入法回发的 WM_IME_* 不再上报(与 mac 一致)。
  const bool was_composing = composing_;
  active_ = false;
  composing_ = false;
  pending_high_surrogate_ = 0;
  if (was_composing) {
    if (HIMC himc = ::ImmGetContext(hwnd_)) {
      ::ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
      ::ImmReleaseContext(hwnd_, himc);
    }
  }
  ::ImmAssociateContextEx(hwnd_, nullptr, 0);
  DestroySystemCaret();
}

void WinTextInput::SetCaretRect(Rect physical_rect) {
  caret_rect_ = physical_rect;
  UpdateImePosition();
}

void WinTextInput::SetEditingState(const platform::TextEditingState& state) { state_ = state; }

void WinTextInput::Emit(const platform::TextInputEvent& event) {
  if (active_ && delegate_ != nullptr) {
    delegate_->OnTextInput(event);
  }
}

void WinTextInput::UpdateImePosition() {
  if (!active_) {
    return;
  }
  const LONG x = ToLong(caret_rect_.x);
  LONG y = ToLong(caret_rect_.y);
  const LONG width = std::max<LONG>(1, ToLong(caret_rect_.width));
  const LONG height = std::max<LONG>(1, ToLong(caret_rect_.height));
  const WORD language = PRIMARYLANGID(LOWORD(reinterpret_cast<UINT_PTR>(::GetKeyboardLayout(0))));

  // 系统 caret(不显示): 新版输入法与辅助工具按它定位。caret 属于线程, 只在本窗口有焦点时持有。
  if (::GetFocus() == hwnd_) {
    if (!caret_created_) {
      caret_created_ = ::CreateCaret(hwnd_, nullptr, 1, static_cast<int>(height)) != FALSE;
    }
    if (caret_created_) {
      ::SetCaretPos(x, language == LANG_JAPANESE ? y + height : y);
    }
  }

  HIMC himc = ::ImmGetContext(hwnd_);
  if (himc == nullptr) {
    return;
  }
  COMPOSITIONFORM composition{};
  composition.dwStyle = CFS_POINT;
  composition.ptCurrentPos = {x, y};
  ::ImmSetCompositionWindow(himc, &composition);

  // 以下顺序与取舍参考 Chromium ui/base/ime/win/imm32_manager.cc(MoveImeWindow)。
  if (language == LANG_CHINESE) {
    // 中文输入法(TSF/CUAS 开启时)使用 CFS_CANDIDATEPOS。
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = {x, y};
    ::ImmSetCandidateWindow(himc, &candidate);
  }
  if (language == LANG_KOREAN) {
    y += 1;  // 韩文输入法以 caret 左下角定位
  }
  // 日文/韩文输入法(TSF/CUAS 关闭时)使用 CFS_EXCLUDE 矩形。
  CANDIDATEFORM exclude{};
  exclude.dwIndex = 0;
  exclude.dwStyle = CFS_EXCLUDE;
  exclude.ptCurrentPos = {x, y};
  exclude.rcArea = {x, y, x + width, y + height};
  ::ImmSetCandidateWindow(himc, &exclude);
  ::ImmReleaseContext(hwnd_, himc);
}

void WinTextInput::DestroySystemCaret() {
  if (caret_created_) {
    ::DestroyCaret();
    caret_created_ = false;
  }
}

void WinTextInput::OnFocusChanged(bool focused) {
  if (focused) {
    UpdateImePosition();  // 重新创建系统 caret
  } else {
    DestroySystemCaret();
  }
}

bool WinTextInput::HandleImeMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) {
  switch (message) {
    case WM_IME_SETCONTEXT:
      if (!active_) {
        return false;
      }
      // 预编辑文本由 JS 绘制: 不显示系统默认组合窗(候选窗仍显示)。
      *result = ::DefWindowProcW(hwnd_, message, wparam,
                                 lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
      return true;

    case WM_IME_STARTCOMPOSITION:
      if (!active_) {
        return false;
      }
      UpdateImePosition();
      *result = 0;  // 不交给 DefWindowProc: 否则显示默认组合窗
      return true;

    case WM_IME_COMPOSITION: {
      if (!active_) {
        return false;
      }
      HIMC himc = ::ImmGetContext(hwnd_);
      if (himc == nullptr) {
        return false;
      }
      // 同一条消息可能同时带结果串与新的预编辑串(先提交一段, 继续输入): 先 commit 再 composing。
      if (lparam & GCS_RESULTSTR) {
        const std::wstring text = GetCompositionString(himc, GCS_RESULTSTR);
        if (!text.empty()) {
          composing_ = false;
          platform::TextInputEvent event;
          event.type = platform::TextInputEvent::Type::kCommit;
          event.text = WideToUtf8(text);
          Emit(event);
        }
      }
      if (lparam & GCS_COMPSTR) {
        const std::wstring text = GetCompositionString(himc, GCS_COMPSTR);
        if (!text.empty()) {
          int32_t cursor = static_cast<int32_t>(text.size());
          if (lparam & GCS_CURSORPOS) {
            const LONG pos = ::ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
            if (pos >= 0) {
              cursor = std::min<int32_t>(static_cast<int32_t>(pos), cursor);
            }
          }
          composing_ = true;
          platform::TextInputEvent event;
          event.type = platform::TextInputEvent::Type::kComposing;
          event.text = WideToUtf8(text);
          event.cursor = cursor;
          Emit(event);
        } else if (composing_) {
          // 预编辑被清空(如退格删光拼音)。
          composing_ = false;
          platform::TextInputEvent event;
          event.type = platform::TextInputEvent::Type::kComposingEnd;
          Emit(event);
        }
      } else if (!(lparam & GCS_RESULTSTR) && composing_) {
        // 既无结果串也无预编辑串: 输入法取消了预编辑。
        composing_ = false;
        platform::TextInputEvent event;
        event.type = platform::TextInputEvent::Type::kComposingEnd;
        Emit(event);
      }
      ::ImmReleaseContext(hwnd_, himc);
      UpdateImePosition();
      *result = 0;  // 不交给 DefWindowProc: 否则会再生成 WM_IME_CHAR/WM_CHAR(重复提交)
      return true;
    }

    case WM_IME_ENDCOMPOSITION:
      if (!active_) {
        return false;
      }
      if (composing_) {
        composing_ = false;
        platform::TextInputEvent event;
        event.type = platform::TextInputEvent::Type::kComposingEnd;
        Emit(event);
      }
      *result = 0;
      return true;

    case WM_IME_NOTIFY:
      if (active_ && wparam == IMN_OPENCANDIDATE) {
        UpdateImePosition();
      }
      return false;  // 候选窗由输入法/DefWindowProc 处理

    case WM_IME_REQUEST:
      if (active_ && wparam == IMR_QUERYCHARPOSITION) {
        auto* position = reinterpret_cast<IMECHARPOSITION*>(lparam);
        if (position != nullptr && position->dwSize >= sizeof(IMECHARPOSITION)) {
          // 应答屏幕坐标(物理像素)。所有字符都报告 caret 位置(JS 只上报 caret 矩形)。
          POINT point = {ToLong(caret_rect_.x), ToLong(caret_rect_.y)};
          ::ClientToScreen(hwnd_, &point);
          position->pt = point;
          position->cLineHeight = static_cast<UINT>(std::max<LONG>(1, ToLong(caret_rect_.height)));
          RECT client{};
          ::GetClientRect(hwnd_, &client);
          ::MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&client), 2);
          position->rcDocument = client;
          *result = 1;
          return true;
        }
      }
      return false;

    default:
      return false;
  }
}

void WinTextInput::OnChar(wchar_t code_unit) {
  if (!active_ || composing_) {
    return;
  }
  std::wstring text;
  if (IsHighSurrogate(code_unit)) {
    pending_high_surrogate_ = code_unit;  // 代理对: 等低位码元
    return;
  }
  if (IsLowSurrogate(code_unit)) {
    if (pending_high_surrogate_ == 0) {
      return;
    }
    text.push_back(pending_high_surrogate_);
    text.push_back(code_unit);
    pending_high_surrogate_ = 0;
  } else {
    pending_high_surrogate_ = 0;
    // 控制字符(退格 0x08、Tab 0x09、回车 0x0D、Ctrl+字母 0x01–0x1A、Ctrl+退格 0x7F 等)不是文本:
    // 这些按键的编辑语义由框架根据 platform::KeyData 处理。
    if (code_unit < 0x20 || code_unit == 0x7F || (code_unit >= 0x80 && code_unit < 0xA0)) {
      return;
    }
    text.push_back(code_unit);
  }
  platform::TextInputEvent event;
  event.type = platform::TextInputEvent::Type::kCommit;
  event.text = WideToUtf8(text);
  Emit(event);
}

}  // namespace momo::embedder::windows
