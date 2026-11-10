#include "engine/shell/pointer_capture.h"

namespace momo::engine {

platform::PointerDataPacket PointerCapture::Filter(platform::ViewId view,
                                                   const platform::PointerDataPacket& packet) {
  platform::PointerDataPacket out;
  out.reserve(packet.size());
  for (const platform::PointerData& p : packet) {
    auto it = captures_.find(p.device);
    if (it != captures_.end()) {
      if (it->second != view) {
        continue;  // 被别的视图捕获: 丢弃
      }
      if (p.phase == platform::PointerPhase::kUp || p.phase == platform::PointerPhase::kRemove) {
        captures_.erase(it);  // 序列结束
      }
    } else if (p.phase == platform::PointerPhase::kDown) {
      captures_[p.device] = view;
    }
    out.push_back(p);
  }
  return out;
}

platform::PointerDataPacket PointerCapture::ReleaseView(platform::ViewId view) {
  platform::PointerDataPacket out;
  for (auto it = captures_.begin(); it != captures_.end();) {
    if (it->second == view) {
      platform::PointerData remove;
      remove.device = it->first;
      remove.phase = platform::PointerPhase::kRemove;
      out.push_back(remove);
      it = captures_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

bool PointerCapture::TransferTo(platform::ViewId view) {
  for (auto& [device, owner] : captures_) {
    owner = view;
  }
  return !captures_.empty();
}

std::optional<platform::ViewId> PointerCapture::captured_by(int32_t device) const {
  auto it = captures_.find(device);
  if (it == captures_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace momo::engine
