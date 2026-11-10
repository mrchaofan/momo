#pragma once

// Golden 像素测试(docs/09 §5): 软件光栅化结果与 PNG 基准比对。
//   EXPECT_TRUE(MatchesGolden("name", pixels));
// 基准位于 engine/graphics/testdata/<name>.png(由 CMake 定义 MOMO_GOLDEN_DIR)。
// 设置环境变量 MOMO_UPDATE_GOLDENS=1 运行即(重新)生成基准。
// 比对允许每通道 ±2 的误差(不同 CPU 指令集下 Skia 的抗锯齿/混合可能有极小差异)。

#include <string>

#include "engine/graphics/software_rasterizer.h"
#include "gtest/gtest.h"

namespace momo::engine::graphics::testing {

::testing::AssertionResult MatchesGolden(const std::string& name, const OwnedPixelBuffer& actual);

}  // namespace momo::engine::graphics::testing
