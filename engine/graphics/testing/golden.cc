#include "engine/graphics/testing/golden.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace momo::engine::graphics::testing {

namespace {

int ChannelDiff(uint32_t a, uint32_t b, int shift) {
  return std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF));
}

}  // namespace

::testing::AssertionResult MatchesGolden(const std::string& name, const OwnedPixelBuffer& actual) {
  const std::filesystem::path path = std::filesystem::path(MOMO_GOLDEN_DIR) / (name + ".png");
  OwnedPixelBuffer& mutable_actual = const_cast<OwnedPixelBuffer&>(actual);

  if (const char* update = std::getenv("MOMO_UPDATE_GOLDENS"); update && *update == '1') {
    auto png = EncodePng(mutable_actual.view());
    if (!png.ok()) {
      return ::testing::AssertionFailure() << png.status();
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png->data()),
                                                static_cast<std::streamsize>(png->size()));
    return ::testing::AssertionSuccess() << "golden updated: " << path;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return ::testing::AssertionFailure()
           << "missing golden " << path << " (run with MOMO_UPDATE_GOLDENS=1 to create it)";
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  OwnedPixelBuffer expected(0, 0);
  if (Status s = DecodePng(bytes, &expected); !s.ok()) {
    return ::testing::AssertionFailure() << s;
  }
  if (expected.width() != actual.width() || expected.height() != actual.height()) {
    return ::testing::AssertionFailure() << "size mismatch: golden " << expected.width() << "x"
                                         << expected.height() << ", actual " << actual.width()
                                         << "x" << actual.height();
  }
  int bad = 0;
  int first_x = -1, first_y = -1;
  for (int y = 0; y < actual.height(); ++y) {
    for (int x = 0; x < actual.width(); ++x) {
      const uint32_t a = actual.data()[y * actual.width() + x];
      const uint32_t e = expected.data()[y * expected.width() + x];
      int worst = 0;
      for (int shift : {0, 8, 16, 24}) {
        worst = std::max(worst, ChannelDiff(a, e, shift));
      }
      if (worst > 2) {
        if (bad++ == 0) {
          first_x = x;
          first_y = y;
        }
      }
    }
  }
  if (bad > 0) {
    // 把实际结果写到临时目录便于查看。
    const auto out = std::filesystem::temp_directory_path() / (name + ".actual.png");
    if (auto png = EncodePng(mutable_actual.view()); png.ok()) {
      std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(png->data()),
                                                 static_cast<std::streamsize>(png->size()));
    }
    return ::testing::AssertionFailure()
           << bad << " pixel(s) differ from " << path << "; first at (" << first_x << ","
           << first_y << "); actual written to " << out;
  }
  return ::testing::AssertionSuccess();
}

}  // namespace momo::engine::graphics::testing
