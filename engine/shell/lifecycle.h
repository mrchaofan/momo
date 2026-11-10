#pragma once

#include <string_view>

namespace momo::engine {

// 经 momo/lifecycle 通道分发给 JS 的应用生命周期事件。
enum class LifecycleEvent { kResumed, kSuspended, kTerminating };

constexpr std::string_view LifecycleEventName(LifecycleEvent event) {
  switch (event) {
    case LifecycleEvent::kResumed: return "resumed";
    case LifecycleEvent::kSuspended: return "suspended";
    case LifecycleEvent::kTerminating: return "terminating";
  }
  return "?";
}

}  // namespace momo::engine
