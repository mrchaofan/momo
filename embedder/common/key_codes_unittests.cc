#include "embedder/common/key_codes.h"

#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

TEST(KeyCodesTest, PhysicalKeyIsTheHidUsageOnTheKeyboardPage) {
  EXPECT_EQ(PhysicalKeyFromHidUsage(0x04), 0x00070004u);
  EXPECT_EQ(PhysicalKeyFromHidUsage(0), 0u);
}

TEST(KeyCodesTest, ClassifiesSpecialKeys) {
  EXPECT_TRUE(IsSpecialHidUsage(0x28));   // Enter
  EXPECT_TRUE(IsSpecialHidUsage(0x29));   // Escape
  EXPECT_TRUE(IsSpecialHidUsage(0x2A));   // Backspace
  EXPECT_TRUE(IsSpecialHidUsage(0x2B));   // Tab
  EXPECT_TRUE(IsSpecialHidUsage(0x39));   // CapsLock
  EXPECT_TRUE(IsSpecialHidUsage(0xE1));   // ShiftLeft
  EXPECT_TRUE(IsSpecialHidUsage(0xE7));   // MetaRight
  EXPECT_TRUE(IsSpecialHidUsage(0x3A));   // F1
  EXPECT_TRUE(IsSpecialHidUsage(0x52));   // ArrowUp
  EXPECT_TRUE(IsSpecialHidUsage(0x58));   // NumpadEnter
  EXPECT_TRUE(IsSpecialHidUsage(0x68));   // F13
  // 输入法切换键: Lang1..Lang5 在每个平台都是特殊键(这里曾经各写一份, macOS 只认到 Lang2)。
  for (uint32_t lang = 0x90; lang <= 0x94; ++lang) {
    EXPECT_TRUE(IsSpecialHidUsage(lang)) << "Lang usage " << lang;
  }
  EXPECT_FALSE(IsSpecialHidUsage(0x04));  // A
  EXPECT_FALSE(IsSpecialHidUsage(0x2C));  // Space
  EXPECT_FALSE(IsSpecialHidUsage(0x62));  // Numpad0
  EXPECT_FALSE(IsSpecialHidUsage(0x67));  // NumpadEqual
  EXPECT_FALSE(IsSpecialHidUsage(0));     // 未知键: 由它产生的字符决定
}

TEST(KeyCodesTest, LogicalKeyIsTheBaseCharacterOrASpecialKey) {
  EXPECT_EQ(LogicalKey(0x04, 0, 'a'), uint64_t{'a'});
  EXPECT_EQ(LogicalKey(0x1E, 0, '1'), uint64_t{'1'}) << "Shift+1 is still the '1' key";
  EXPECT_EQ(LogicalKey(0x04, 0, 0x4E2D), 0x4E2Du) << "non-Latin layouts";
  // 特殊键不看字符: 回车产生 '\r', 小键盘回车在某些布局下产生可打印字符。
  EXPECT_EQ(LogicalKey(0x28, 0, '\r'), kSpecialKeyBase + 0x28);
  EXPECT_EQ(LogicalKey(0x58, 0, '3'), kSpecialKeyBase + 0x58);
  // 不产生可打印字符的键也是特殊键(死键、控制字符、C1)。
  EXPECT_EQ(LogicalKey(0x04, 0, 0), kSpecialKeyBase + 0x04);
  EXPECT_EQ(LogicalKey(0x04, 0, 0x7F), kSpecialKeyBase + 0x04);
  EXPECT_EQ(LogicalKey(0x04, 0, 0x85), kSpecialKeyBase + 0x04);
  // 没有 HID usage 的键: 用平台的原生键码, 落在 usage 空间之外, 互不冲突。
  EXPECT_EQ(LogicalKey(0, 0x3F, 0), kSpecialKeyBase + 0x10000 + 0x3F);
  EXPECT_NE(SpecialLogicalKey(0, 0x3F), SpecialLogicalKey(0x3F, 0));
  EXPECT_EQ(LogicalKey(0, 0x3F, 'x'), uint64_t{'x'}) << "an unknown key that types a character is that character";
}

}  // namespace
}  // namespace momo::embedder
