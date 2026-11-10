#pragma once

// 单调时钟类型。header-only, 属于 momo_base。

#include <chrono>

namespace momo {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using TimeDelta = Clock::duration;

}  // namespace momo
