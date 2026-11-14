// 预编译 Skia / V8 接入冒烟测试: 验证头文件、配置宏、链接与运行期(rpath、ICU 数据)都正确。

#include <cstring>
#include <memory>
#include <string>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradientShader.h"
#if defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_mac_ct.h"
#endif
#include "libplatform/libplatform.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "v8.h"
#include "node.h"
#include "gtest/gtest.h"

namespace {

// ---------------------------------------------------------------------------
// Skia: CPU 光栅化(软件后端, 与 M1 的 SoftwareBackend 同路径)

TEST(SkiaSmokeTest, RasterSurfaceDrawsGradientAndPath) {
  const SkImageInfo info = SkImageInfo::MakeN32Premul(64, 64);
  sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  ASSERT_NE(surface, nullptr);
  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SK_ColorWHITE);

  SkPaint gradient;
  const SkPoint pts[2] = {{0, 0}, {64, 0}};
  const SkColor colors[2] = {SK_ColorRED, SK_ColorBLUE};
  gradient.setShader(SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp));
  canvas->drawRect(SkRect::MakeWH(64, 32), gradient);

  SkPaint green;
  green.setColor(SK_ColorGREEN);
  green.setAntiAlias(true);
  SkPathBuilder builder;
  builder.moveTo(0, 32).lineTo(64, 32).lineTo(64, 64).lineTo(0, 64).close();
  canvas->drawPath(builder.detach(), green);

  SkPixmap pixmap;
  ASSERT_TRUE(surface->peekPixels(&pixmap));
  const SkColor left = pixmap.getColor(1, 10);
  const SkColor right = pixmap.getColor(62, 10);
  EXPECT_GT(SkColorGetR(left), 200u);
  EXPECT_LT(SkColorGetB(left), 60u);
  EXPECT_GT(SkColorGetB(right), 200u);
  EXPECT_EQ(pixmap.getColor(32, 48), SK_ColorGREEN);
}

TEST(SkiaSmokeTest, ParagraphLaysOutMixedScriptText) {
#if defined(_WIN32)
  sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_DirectWrite();
#else
  sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_CoreText(nullptr);
#endif
  ASSERT_NE(font_mgr, nullptr);
  auto fonts = sk_make_sp<skia::textlayout::FontCollection>();
  fonts->setDefaultFontManager(font_mgr);

  skia::textlayout::ParagraphStyle style;
  skia::textlayout::TextStyle text_style;
  text_style.setFontSize(16);
  text_style.setColor(SK_ColorBLACK);
  text_style.setFontFamilies({SkString("Helvetica"), SkString("PingFang SC")});
  style.setTextStyle(text_style);

  auto unicode = SkUnicodes::ICU::Make();
  ASSERT_NE(unicode, nullptr) << "skunicode ICU backend unavailable";
  auto builder = skia::textlayout::ParagraphBuilder::make(style, fonts, unicode);
  builder->addText("Hello 你好 Momo");
  auto paragraph = builder->Build();
  paragraph->layout(200);
  EXPECT_GT(paragraph->getHeight(), 0);
  EXPECT_GT(paragraph->getLongestLine(), 0);
  EXPECT_EQ(paragraph->lineNumber(), 1u);
}

// ---------------------------------------------------------------------------
// V8: 平台初始化 + Isolate/Context + 执行脚本(含 Intl, 验证 ICU 数据)

class V8SmokeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // libnode owns its embedded ICU data and must initialize it before V8.
    auto node_init = node::InitializeOncePerProcess(
        {"momo-smoke"},
        {node::ProcessInitializationFlags::kNoInitializeV8,
         node::ProcessInitializationFlags::kNoInitializeNodeV8Platform,
         node::ProcessInitializationFlags::kNoStdioInitialization,
         node::ProcessInitializationFlags::kNoDefaultSignalHandling,
         node::ProcessInitializationFlags::kDisableNodeOptionsEnv});
    ASSERT_EQ(node_init->exit_code(), 0);
    platform_ = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform_.get());
    ASSERT_TRUE(v8::V8::Initialize());
  }
  static void TearDownTestSuite() {
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    platform_.reset();
  }

  static std::string Eval(const char* source) {
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    v8::Isolate* isolate = v8::Isolate::New(params);
    std::string out;
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      v8::Local<v8::Context> context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      v8::Local<v8::String> code = v8::String::NewFromUtf8(isolate, source).ToLocalChecked();
      v8::Local<v8::Script> script = v8::Script::Compile(context, code).ToLocalChecked();
      v8::Local<v8::Value> result = script->Run(context).ToLocalChecked();
      v8::String::Utf8Value utf8(isolate, result);
      out = *utf8 ? *utf8 : "";
    }
    isolate->Dispose();
    delete params.array_buffer_allocator;
    return out;
  }

  static std::unique_ptr<v8::Platform> platform_;
};

std::unique_ptr<v8::Platform> V8SmokeTest::platform_;

TEST_F(V8SmokeTest, EvaluatesScript) {
  EXPECT_EQ(Eval("const f = (n) => n <= 1 ? 1 : n * f(n - 1); `momo ${f(10)}`"), "momo 3628800");
}

TEST_F(V8SmokeTest, IntlWorksWithIcuData) {
  EXPECT_EQ(Eval("new Intl.NumberFormat('en-US').format(1234567.5)"), "1,234,567.5");
  EXPECT_EQ(Eval("'ä'.localeCompare('z', 'de') < 0 ? 'ok' : 'bad'"), "ok");
}

}  // namespace
