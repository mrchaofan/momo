#pragma once

// IME 接入协议(docs/03 §3)。
//
// 编辑模型归 JS 框架所有(EditableText);平台只负责与系统输入法交互:
//   JS → 平台(经 momo/textinput 通道, 由 Shell 在 platform 线程调用 TextInputConnection):
//     Show / Hide                     开始/结束一次输入会话(获得/失去文本焦点)
//     SetEditingState(state)          把当前文本/选区/composing 区间告诉平台(系统输入法上下文、候选窗定位)
//     SetCaretRect(rect)              光标矩形(物理像素, 相对窗口内容区左上角)用于候选窗定位
//   平台 → JS(WindowDelegate::OnTextInput, platform 线程 → Shell → momo/textinput 通道):
//     TextInputEvent(commit / composing / 删除 / 编辑命令)
// 索引一律为 UTF-16 码元下标(与 JS 字符串一致)。
// 线程约定: TextInputConnection 的方法只在 platform 线程调用。

#include <cstdint>
#include <string>

#include "base/geometry.h"

namespace momo::engine::platform {

struct TextEditingState {
  std::string text;  // UTF-8
  int32_t selection_base = 0;      // UTF-16 下标
  int32_t selection_extent = 0;
  int32_t composing_start = -1;    // -1 = 无 composing 区间
  int32_t composing_end = -1;
  friend bool operator==(const TextEditingState&, const TextEditingState&) = default;
};

struct TextInputEvent {
  enum class Type : uint8_t {
    kCommit,          // 提交 text(替换 composing 区间, 否则替换选区)
    kComposing,       // 更新 composing(预编辑)文本为 text;cursor = 预编辑内光标(UTF-16)
    kComposingEnd,    // 结束 composing(输入法取消预编辑, text 为空)
    // 编辑命令。注意: 由按键产生的编辑操作(退格/方向键/回车等)以 KeyData 送达框架处理, 平台**不**
    // 再为同一按键发 kCommand(否则执行两次);kCommand 只用于没有对应按键事件的来源(如辅助功能、
    // 系统服务菜单)。
    kCommand,         // command = "deleteBackward" | "deleteForward" | "insertNewline" |
                      // "insertTab" | "moveLeft" | "moveRight" | "moveUp" | "moveDown" |
                      // "moveToLineStart" | "moveToLineEnd" | "selectAll" | ...(macOS 选择子名去掉冒号)
  };
  Type type = Type::kCommit;
  std::string text;     // UTF-8
  int32_t cursor = 0;   // kComposing: 预编辑文本内的光标
  std::string command;  // kCommand
  // kCommit / kComposing: 输入法显式指定的替换区间 [replace_start, replace_end)(UTF-16, 相对整段文本),
  // 如按住字母选重音字符、日文重新转换。-1 = 未指定(替换 composing 区间, 否则替换选区)。
  int32_t replace_start = -1;
  int32_t replace_end = -1;
};

class TextInputConnection {
 public:
  virtual ~TextInputConnection() = default;
  virtual void Show() = 0;
  virtual void Hide() = 0;
  virtual void SetCaretRect(Rect physical_rect) = 0;
  virtual void SetEditingState(const TextEditingState& state) = 0;
};

}  // namespace momo::engine::platform
