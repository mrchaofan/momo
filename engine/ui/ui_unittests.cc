#include <memory>
#include <string>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/standard_codec.h"
#include "engine/graphics/software_rasterizer.h"
#include "engine/graphics/testing/golden.h"
#include "engine/graphics/texture.h"
#include "engine/runtime/js_runtime.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/ui/ui_bindings.h"
#include "gtest/gtest.h"

namespace momo::engine::ui {
namespace {

class FakeWindowClient final : public WindowClient {
 public:
  void ScheduleFrame() override { scheduled++; }
  void Render(std::unique_ptr<graphics::LayerTree> tree) override {
    rendered.push_back(std::move(tree));
  }
  void SetTitle(ViewId, std::string t) override { title = std::move(t); }
  void SetCursor(ViewId, platform::SystemCursor c) override { cursor = c; }
  void SetMinSize(ViewId, Size s) override { min_size = s; }
  platform::ViewportMetrics metrics(ViewId view) const override {
    return view == platform::kImplicitViewId ? current : extra_metrics;
  }
  bool focused(ViewId) const override { return is_focused; }
  std::vector<ViewId> views() const override { return view_ids; }
  void CreateView(uint64_t request, const platform::ViewOptions& options) override {
    create_requests.emplace_back(request, options);
  }
  void CloseView(ViewId view) override { closed.push_back(view); }
  void SetResizeOptions(ViewId, const platform::WindowResizeOptions& options) override {
    resize_options.push_back(options);
  }
  std::vector<platform::WindowResizeOptions> resize_options;
  void DispatchPlatformViewPointer(ViewId view, int64_t platform_view_id, uint64_t native_event_id,
                                   platform::PlatformViewPointerAction action) override {
    platform_view_pointer.push_back({view, platform_view_id, native_event_id, action});
  }

  struct PlatformViewPointer {
    ViewId view;
    int64_t platform_view_id;
    uint64_t native_event_id;
    platform::PlatformViewPointerAction action;
  };
  std::vector<PlatformViewPointer> platform_view_pointer;
  std::vector<ViewId> view_ids{platform::kImplicitViewId};
  platform::ViewportMetrics extra_metrics{{300, 200}, 2.0};
  std::vector<std::pair<uint64_t, platform::ViewOptions>> create_requests;
  std::vector<ViewId> closed;

  platform::SystemCursor cursor = platform::SystemCursor::kArrow;
  Size min_size;
  bool is_focused = false;

  platform::ViewportMetrics current{{400, 270}, 1.0};
  int scheduled = 0;
  std::vector<std::unique_ptr<graphics::LayerTree>> rendered;
  std::string title;
};

class UiBindingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto runtime = runtime::JsRuntime::Create({});
    ASSERT_TRUE(runtime.ok()) << runtime.status();
    runtime_ = std::move(runtime).value();
    host_.ui_runner = loop_.task_runner();
    host_.io_runner = loop_.task_runner();  // 测试中 io 与 ui 同线程, 由 RunUntilIdle 驱动
    host_.hub = hub_;
    host_.textures = textures_;
    Settings settings;
    settings.assets_dir = std::string(MOMO_SOURCE_DIR) + "/engine/graphics/testdata";
    bindings_ = UiBindings::Install(*runtime_, client_, host_, settings);
  }
  void TearDown() override {
    bindings_.reset();
    runtime_.reset();
  }

  Status Run(const std::string& source) { return runtime_->RunModuleSource(source, "app.js"); }
  std::string Eval(const std::string& source) {
    auto r = runtime_->EvalToString(source);
    EXPECT_TRUE(r.ok()) << r.status();
    return r.ok() ? *r : "";
  }
  // 在 JS 中执行并返回抛出的错误消息("" = 未抛出)。
  std::string ErrorOf(const std::string& expr) {
    return Eval("(() => { try { " + expr + "; return ''; } catch (e) { return String(e); } })()");
  }

  momo::testing::ScopedTestEventLoop loop_;
  std::shared_ptr<MessageChannelHub> hub_ = std::make_shared<MessageChannelHub>();
  std::shared_ptr<graphics::TextureRegistry> textures_ =
      std::make_shared<graphics::TextureRegistry>();
  AppHost host_;
  FakeWindowClient client_;
  std::unique_ptr<runtime::JsRuntime> runtime_;
  std::unique_ptr<UiBindings> bindings_;
};

TEST_F(UiBindingsTest, NotImportedMeansNoCallbacks) {
  ASSERT_TRUE(Run("globalThis.x = 1;").ok());
  EXPECT_FALSE(bindings_->is_imported());
  bindings_->BeginFrame(Clock::now());
  bindings_->DrawFrame();
  EXPECT_TRUE(bindings_->CloseRequested());
  EXPECT_TRUE(client_.rendered.empty());
}

TEST_F(UiBindingsTest, FrameCallbacksRenderScene) {
  ASSERT_TRUE(Run(R"JS(
    import { window, PictureRecorder, Paint, SceneBuilder } from 'momo:ui';
    globalThis.calls = [];
    window.onBeginFrame = (t) => { calls.push(typeof t); };
    window.onDrawFrame = () => {
      calls.push('draw');
      const recorder = new PictureRecorder();
      const canvas = recorder.beginRecording();
      const paint = new Paint();
      paint.color = 0xff00ff00;
      canvas.drawRect(10, 10, 20, 20, paint);
      const builder = new SceneBuilder();
      builder.pushOffset(5, 5).addPicture(0, 0, recorder.endRecording()).pop();
      window.render(builder.build());
    };
    window.scheduleFrame();
  )JS").ok());
  EXPECT_TRUE(bindings_->is_imported());
  EXPECT_EQ(client_.scheduled, 1);

  bindings_->BeginFrame(Clock::now());
  bindings_->DrawFrame();
  EXPECT_EQ(Eval("calls.join(',')"), "number,draw");
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::LayerTree& tree = *client_.rendered[0];
  EXPECT_EQ(tree.frame_size(), (Size{400, 270}));

  graphics::OwnedPixelBuffer pixels(400, 270);
  ASSERT_TRUE(graphics::RasterizeLayerTree(tree, pixels.view(), 0xFFFFFFFF).ok());
  EXPECT_EQ(pixels.GetColor(20, 20), 0xFF00FF00u);  // (10,10)+(5,5) 偏移内
  EXPECT_EQ(pixels.GetColor(12, 12), 0xFFFFFFFFu);  // 偏移前的位置为空
}

// drawPath 共享路径快照: 之后修改 Path 不影响已记录的内容(写时复制)。
TEST_F(UiBindingsTest, PathEditsAfterDrawDoNotAffectRecording) {
  ASSERT_TRUE(Run(R"JS(
    import { window, PictureRecorder, Paint, Path, SceneBuilder } from 'momo:ui';
    window.onDrawFrame = () => {
      const recorder = new PictureRecorder();
      const canvas = recorder.beginRecording();
      const paint = new Paint();
      paint.color = 0xffff0000;
      const path = new Path().addRect(0, 0, 10, 10);
      canvas.drawPath(path, paint);
      path.reset().addRect(50, 50, 10, 10);
      canvas.drawPath(path, paint);
      const builder = new SceneBuilder();
      builder.addPicture(0, 0, recorder.endRecording());
      window.render(builder.build());
    };
  )JS").ok());
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::OwnedPixelBuffer pixels(100, 100);
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view(), 0xFFFFFFFF).ok());
  EXPECT_EQ(pixels.GetColor(5, 5), 0xFFFF0000u);
  EXPECT_EQ(pixels.GetColor(55, 55), 0xFFFF0000u);
}

// JS 画出的验收画面必须与 C++ 直接构建的 golden(graphics_unittests)逐像素一致。
TEST_F(UiBindingsTest, JsAcceptanceSceneMatchesGolden) {
  ASSERT_TRUE(Run(R"JS(
    import { window, PictureRecorder, Paint, Path, Gradient, SceneBuilder } from 'momo:ui';
    window.onDrawFrame = () => {
      const recorder = new PictureRecorder();
      const c = recorder.beginRecording();
      c.drawColor(0xFFF5F5F7);

      const gradient = new Paint();
      gradient.shader = Gradient.linear(20, 20, 220, 120, [0xFF4285F4, 0xFFEA4335]);
      c.drawRRect(20, 20, 200, 100, 16, gradient);

      const star = new Path();
      const cx = 320, cy = 70, outer = 50, inner = 20;
      for (let i = 0; i < 10; i++) {
        const r = i % 2 === 0 ? outer : inner;
        const a = -Math.PI / 2 + i * Math.PI / 5;
        if (i === 0) star.moveTo(cx + r * Math.cos(a), cy + r * Math.sin(a));
        else star.lineTo(cx + r * Math.cos(a), cy + r * Math.sin(a));
      }
      star.close();
      const yellow = new Paint();
      yellow.color = 0xFFFBBC05;
      c.drawPath(star, yellow);

      const stroke = new Paint();
      stroke.color = 0xFF34A853;
      stroke.style = 'stroke';
      stroke.strokeWidth = 6;
      stroke.strokeCap = 'round';
      c.drawLine(20, 160, 380, 160, stroke);
      c.drawCircle(80, 220, 30, stroke);

      c.save();
      c.translate(220, 220);
      c.rotate(Math.PI / 4);
      const purple = new Paint();
      purple.color = 0xCC9C27B0;
      c.drawRect(-25, -25, 50, 50, purple);
      c.restore();

      const builder = new SceneBuilder();
      builder.addPicture(0, 0, recorder.endRecording());
      window.render(builder.build());
    };
  )JS").ok());
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::OwnedPixelBuffer pixels(400, 270);
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view()).ok());
  EXPECT_TRUE(graphics::testing::MatchesGolden("acceptance_scene", pixels));
}

TEST_F(UiBindingsTest, ArgumentAndStateErrorsThrow) {
  ASSERT_TRUE(Run("import * as ui from 'momo:ui'; globalThis.ui = ui;").ok());
  EXPECT_NE(ErrorOf("new ui.Canvas()").find("Illegal constructor"), std::string::npos);
  EXPECT_NE(ErrorOf("ui.Paint()").find("without 'new'"), std::string::npos);
  EXPECT_NE(ErrorOf("new ui.Paint().style = 'dotted'").find("'fill', 'stroke'"),
            std::string::npos);
  EXPECT_NE(ErrorOf("new ui.PictureRecorder().beginRecording().drawRect(0, 0, 'x', 1, new ui.Paint())")
                .find("must be a number"),
            std::string::npos);
  EXPECT_NE(ErrorOf("new ui.PictureRecorder().beginRecording().drawRect(0, 0, 1, 1, {})")
                .find("must be a Paint"),
            std::string::npos);
  EXPECT_NE(ErrorOf("const r = new ui.PictureRecorder(); const c = r.beginRecording();"
                    "r.endRecording(); c.save()")
                .find("no longer recording"),
            std::string::npos);
  EXPECT_NE(ErrorOf("new ui.SceneBuilder().pop()").find("nothing to pop"), std::string::npos);
  EXPECT_NE(ErrorOf("ui.window.render({})").find("must be a Scene"), std::string::npos);
  EXPECT_EQ(ErrorOf("new ui.Path().moveTo(0, 0).lineTo(1, 1).close()"), "");
}

TEST_F(UiBindingsTest, PaintPropertiesRoundTrip) {
  ASSERT_TRUE(Run("import * as ui from 'momo:ui'; globalThis.ui = ui;").ok());
  EXPECT_EQ(Eval("const p = new ui.Paint(); p.color = 0x80FF0000; p.style = 'stroke';"
                 "p.strokeWidth = 3; p.strokeJoin = 'bevel'; p.antiAlias = false;"
                 "[p.color.toString(16), p.style, p.strokeWidth, p.strokeJoin, p.antiAlias,"
                 " p.shader].join('|')"),
            "80ff0000|stroke|3|bevel|false|");
  EXPECT_EQ(Eval("const g = ui.Gradient.radial(0, 0, 10, [0xff000000, 0xffffffff]);"
                 "const q = new ui.Paint(); q.shader = g; q.shader === g"),
            "true");
}

TEST_F(UiBindingsTest, MetricsTitleAndCloseRequest) {
  ASSERT_TRUE(Run(R"JS(
    import { window } from 'momo:ui';
    globalThis.window = window;
    globalThis.metricsCalls = 0;
    window.onMetricsChanged = () => { metricsCalls++; };
    window.title = 'Hello Momo';
  )JS").ok());
  EXPECT_EQ(client_.title, "Hello Momo");
  EXPECT_EQ(Eval("window.physicalSize.width + 'x' + window.physicalSize.height + '@' +"
                 " window.devicePixelRatio"),
            "400x270@1");

  client_.current = {{800, 600}, 2.0};
  bindings_->MetricsChanged();
  EXPECT_EQ(Eval("metricsCalls"), "1");
  EXPECT_EQ(Eval("window.physicalSize.width + '@' + window.devicePixelRatio"), "800@2");

  EXPECT_TRUE(bindings_->CloseRequested());  // 未设置 handler: 放行
  Eval("window.onCloseRequested = () => false");
  EXPECT_FALSE(bindings_->CloseRequested());
  Eval("window.onCloseRequested = () => { throw new Error('oops'); }");
  EXPECT_TRUE(bindings_->CloseRequested()) << "handler errors must not block closing";
}

TEST_F(UiBindingsTest, HostObjectsAreCollected) {
  ASSERT_TRUE(Run("import * as ui from 'momo:ui'; globalThis.ui = ui;").ok());
  runtime::JsRuntimeImpl& impl = runtime_->impl();
  Eval("for (let i = 0; i < 2000; i++) { new ui.Paint(); new ui.Path().addRect(0, 0, 1, 1); }");
  const size_t before = impl.live_wrappables();
  EXPECT_GE(before, 1u);
  impl.isolate()->LowMemoryNotification();
  EXPECT_LT(impl.live_wrappables(), before) << "unreachable Paint/Path wrappers should be freed";
}

}  // namespace
}  // namespace momo::engine::ui

namespace momo::engine::ui {
namespace {

// view.setResizeOptions: 未给出的字段取默认值;border 可为数字(四边)或分边对象;负数按 0。
TEST_F(UiBindingsTest, ResizeOptionsAreParsedWithDefaults) {
  ASSERT_TRUE(Run("import { window } from 'momo:ui'; globalThis.window = window;").ok());
  Eval("window.setResizeOptions({});");
  Eval("window.setResizeOptions({ resizable: false, border: 3, liveResizeFrameWait: 0 });");
  Eval("window.setResizeOptions({ border: { top: 0, left: 8, bottom: -2 } });");
  ASSERT_EQ(client_.resize_options.size(), 3u);
  const platform::WindowResizeOptions defaults;
  EXPECT_TRUE(client_.resize_options[0].resizable);
  EXPECT_EQ(client_.resize_options[0].border_top, defaults.border_top);
  EXPECT_EQ(client_.resize_options[0].live_resize_frame_wait_ms, defaults.live_resize_frame_wait_ms);
  EXPECT_FALSE(client_.resize_options[1].resizable);
  EXPECT_EQ(client_.resize_options[1].border_left, 3);
  EXPECT_EQ(client_.resize_options[1].border_bottom, 3);
  EXPECT_EQ(client_.resize_options[1].live_resize_frame_wait_ms, 0);
  EXPECT_EQ(client_.resize_options[2].border_top, 0);
  EXPECT_EQ(client_.resize_options[2].border_left, 8);
  EXPECT_EQ(client_.resize_options[2].border_bottom, 0) << "negative clamps to 0";
  EXPECT_EQ(client_.resize_options[2].border_right, defaults.border_right) << "missing edge keeps the default";
}

// 平台视图输入(docs/05 §9): 指针包带原生事件 id 与滚动阶段;框架的处置经 dispatchPlatformViewPointer 回到宿主。
TEST_F(UiBindingsTest, NativeEventIdsRoundTripToPlatformViews) {
  ASSERT_TRUE(Run(R"JS(
    import { window, POINTER_DATA_STRIDE } from 'momo:ui';
    globalThis.window = window;
    globalThis.log = [];
    window.onPointerPacket = (data, count) => {
      for (let i = 0; i < count; i++) {
        const o = i * POINTER_DATA_STRIDE;
        log.push(`${POINTER_DATA_STRIDE}:${data[o + 12]}:${data[o + 13]}:${data[o + 14]}:${data[o + 15]}:${data[o + 16]}`);
        window.dispatchPlatformViewPointer(77, data[o + 12], 0);
      }
    };
  )JS").ok());
  platform::PointerDataPacket packet(1);
  packet[0].phase = platform::PointerPhase::kScroll;
  packet[0].native_event_id = 9007199254740991ull;  // 2^53 - 1: 以 double 精确往返
  packet[0].signal_phase = platform::SignalPhase::kUpdate;
  packet[0].phase = platform::PointerPhase::kGesture;
  packet[0].gesture_kind = platform::PointerGestureKind::kPressure;
  packet[0].gesture_value = 0.5;
  packet[0].gesture_stage = 2;
  bindings_->PointerPacket(packet);
  EXPECT_EQ(Eval("log.join(' ')"), "17:9007199254740991:2:6:0.5:2");
  ASSERT_EQ(client_.platform_view_pointer.size(), 1u);
  EXPECT_EQ(client_.platform_view_pointer[0].view, platform::kImplicitViewId);
  EXPECT_EQ(client_.platform_view_pointer[0].platform_view_id, 77);
  EXPECT_EQ(client_.platform_view_pointer[0].native_event_id, 9007199254740991ull);
  EXPECT_EQ(client_.platform_view_pointer[0].action, platform::PlatformViewPointerAction::kEvent);
  Eval("window.dispatchPlatformViewPointer(5, 0, 2); window.dispatchPlatformViewPointer(5, 0, 9);");
  ASSERT_EQ(client_.platform_view_pointer.size(), 2u) << "invalid actions are ignored";
  EXPECT_EQ(client_.platform_view_pointer[1].action, platform::PlatformViewPointerAction::kExit);
}

TEST_F(UiBindingsTest, PointerKeyAndFocusEventsReachJs) {
  ASSERT_TRUE(Run(R"JS(
    import { window, POINTER_DATA_STRIDE } from 'momo:ui';
    globalThis.window = window;
    globalThis.log = [];
    window.onPointerPacket = (data, count) => {
      for (let i = 0; i < count; i++) {
        const o = i * POINTER_DATA_STRIDE;
        log.push(`p${data[o + 2]}@${data[o + 4]},${data[o + 5]}b${data[o + 6]}`);
      }
    };
    window.onKeyData = (k) => { log.push(`k:${k.type}:${k.physicalKey.toString(16)}:${k.character}:${k.modifiers}`); return k.character === 'a'; };
    window.onFocusChanged = (f) => { log.push(`f:${f}:${window.focused}`); };
    window.setCursor('text');
    window.setMinSize(320, 200);
  )JS").ok());
  EXPECT_EQ(client_.cursor, platform::SystemCursor::kIBeam);
  EXPECT_EQ(client_.min_size, (Size{320, 200}));
  EXPECT_NE(ErrorOf("window.setCursor('banana')").find("unknown cursor"), std::string::npos);

  platform::PointerDataPacket packet(2);
  packet[0].phase = platform::PointerPhase::kDown;
  packet[0].physical_x = 10;
  packet[0].physical_y = 20;
  packet[0].buttons = 1;
  packet[1].phase = platform::PointerPhase::kUp;
  packet[1].physical_x = 11;
  packet[1].physical_y = 21;
  bindings_->PointerPacket(packet);

  platform::KeyData key;
  key.physical_key = 0x00070004;
  key.character = "a";
  key.modifiers = platform::kModifierShift;
  EXPECT_TRUE(bindings_->KeyData(key));
  key.character = "b";
  EXPECT_FALSE(bindings_->KeyData(key));

  client_.is_focused = true;
  bindings_->FocusChanged(true);
  EXPECT_EQ(Eval("log.join(' ')"),
            "p1@10,20b1 p3@11,21b0 k:down:70004:a:1 k:down:70004:b:1 f:true:true");
}

TEST_F(UiBindingsTest, ServicesChannelRoundTrips) {
  // native 侧 echo handler(在 ui 线程上运行)。
  hub_->SetNativeHandler("test/echo", loop_.task_runner(), [](MessageBytes m, MessageReply r) {
    std::reverse(m.begin(), m.end());
    r.Send(std::move(m));
  });
  ASSERT_TRUE(Run(R"JS(
    import { send, setHandler, platform } from 'momo:services';
    globalThis.result = '';
    globalThis.platformName = platform;
    send('test/echo', new Uint8Array([1, 2, 3])).then((buf) => {
      result = Array.from(new Uint8Array(buf)).join(',');
    });
    send('test/nobody', null).then((buf) => { globalThis.nobody = String(buf); });
    setHandler('test/js', async (data) => {
      const bytes = new Uint8Array(data);
      return new Uint8Array([bytes[0] * 2]).buffer;
    });
  )JS").ok());
  loop_.RunUntilIdle();
  EXPECT_EQ(Eval("result"), "3,2,1");
  EXPECT_EQ(Eval("nobody"), "null");
  EXPECT_FALSE(Eval("platformName").empty());

  std::optional<MessageBytes> reply;
  hub_->SendToJs("test/js", {21}, loop_.task_runner(),
                 [&](std::optional<MessageBytes> r) { reply = std::move(r); });
  loop_.RunUntilIdle();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(*reply, MessageBytes{42});

  // 未注册 JS handler 的通道: 回复 null。
  bool null_reply = false;
  hub_->SendToJs("test/none", {1}, loop_.task_runner(),
                 [&](std::optional<MessageBytes> r) { null_reply = !r.has_value(); });
  loop_.RunUntilIdle();
  EXPECT_TRUE(null_reply);
}

TEST_F(UiBindingsTest, ParagraphLaysOutMixedScriptsAndDraws) {
  ASSERT_TRUE(Run(R"JS(
    import { window, ParagraphBuilder, PictureRecorder, SceneBuilder } from 'momo:ui';
    globalThis.window = window;
    globalThis.ParagraphBuilder = ParagraphBuilder;
    const b = new ParagraphBuilder({ textAlign: 'left', maxLines: 3, ellipsis: '…',
                                     textStyle: { fontSize: 20, color: 0xFF000000 } });
    b.addText('Hello 世界 ');
    b.pushStyle({ fontWeight: 700, color: 0xFFFF0000, decoration: 'underline' });
    b.addText('Momo 😀');
    b.pop();
    const p = b.build();
    p.layout(300);
    globalThis.p = p;
    globalThis.info = [p.height > 0, p.longestLine > 50, p.lineCount, p.didExceedMaxLines].join(',');
    const boxes = p.getBoxesForRange(0, 5);
    globalThis.box = boxes.length === 5 && boxes[2] > boxes[0];
    const pos = p.getPositionForOffset(1000, 5);
    globalThis.endPos = pos.offset;
    const wb = p.getWordBoundary(1);
    globalThis.word = wb.start + '-' + wb.end;
    window.onDrawFrame = () => {
      const r = new PictureRecorder();
      const c = r.beginRecording();
      c.drawColor(0xFFFFFFFF);
      c.drawParagraph(p, 10, 10);
      const sb = new SceneBuilder();
      sb.addPicture(0, 0, r.endRecording());
      window.render(sb.build());
    };
  )JS").ok());
  EXPECT_EQ(Eval("info"), "true,true,1,false");
  EXPECT_EQ(Eval("box"), "true");
  EXPECT_EQ(Eval("endPos"), Eval("'Hello 世界 Momo 😀'.length"));
  EXPECT_EQ(Eval("word"), "0-5");
  EXPECT_EQ(Eval("p.getLineMetrics()[0].endIndex > 10"), "true");
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::OwnedPixelBuffer pixels(400, 270);
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view()).ok());
  int dark = 0;
  for (int y = 10; y < 40; ++y) {
    for (int x = 10; x < 300; ++x) {
      dark += (pixels.GetColor(x, y) & 0xFF) < 0x80 ? 1 : 0;
    }
  }
  EXPECT_GT(dark, 100) << "paragraph glyphs should have been rasterized";
  EXPECT_NE(ErrorOf("new ParagraphBuilder({ textAlign: 'diagonal' })").find("invalid"),
            std::string::npos);
}

TEST_F(UiBindingsTest, ImagesDecodeOnIoAndDraw) {
  ASSERT_TRUE(Run(R"JS(
    import { window, decodeImage, loadAsset, imageFromPixels, instantiateCodec,
             PictureRecorder, SceneBuilder, Paint } from 'momo:ui';
    globalThis.window = window;
    globalThis.state = 'pending';
    const red = imageFromPixels(new Uint8Array([255, 0, 0, 255, 255, 0, 0, 255,
                                                255, 0, 0, 255, 255, 0, 0, 255]), 2, 2);
    globalThis.redSize = red.width + 'x' + red.height;
    loadAsset('acceptance_scene.png')
      .then((bytes) => decodeImage(bytes, 200))
      .then((img) => { globalThis.img = img; state = img.width + 'x' + img.height; })
      .catch((e) => { state = 'error:' + e.message; });
    loadAsset('../../../README.md').catch((e) => { globalThis.escape = e.message; });
    decodeImage(new Uint8Array([1, 2, 3])).catch((e) => { globalThis.bad = 'rejected'; });
    loadAsset('acceptance_scene.png').then((b) => instantiateCodec(b))
      .then((codec) => codec.getNextFrame().then((f) => {
        globalThis.codecInfo = codec.frameCount + ':' + f.image.width;
      }));
    window.onDrawFrame = () => {
      const r = new PictureRecorder();
      const c = r.beginRecording();
      c.drawImageRect(red, 0, 0, 2, 2, 0, 0, 50, 50, null);
      const p = new Paint();
      p.filterQuality = 'high';
      c.drawImage(globalThis.img, 100, 0, p);
      const sb = new SceneBuilder();
      sb.addPicture(0, 0, r.endRecording());
      window.render(sb.build());
    };
  )JS").ok());
  EXPECT_EQ(Eval("redSize"), "2x2");
  loop_.RunUntilIdle();
  EXPECT_EQ(Eval("state"), "200x135");
  EXPECT_NE(Eval("escape").find("escapes"), std::string::npos);
  EXPECT_EQ(Eval("bad"), "rejected");
  EXPECT_EQ(Eval("codecInfo"), "1:400");
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::OwnedPixelBuffer pixels(400, 270);
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view()).ok());
  EXPECT_EQ(pixels.GetColor(25, 25), 0xFFFF0000u);
  EXPECT_EQ(pixels.GetColor(102, 2) >> 24, 0xFFu) << "decoded image drawn at (100, 0)";
}

// addPlatformView: 只记录占位层(视图由宿主合成), 并标记层树含平台视图;无效参数被忽略。
TEST_F(UiBindingsTest, AddPlatformViewMarksTheLayerTree) {
  ASSERT_TRUE(Run(R"JS(
    import { window, SceneBuilder } from 'momo:ui';
    globalThis.window = window;
    globalThis.draw = (withView) => {
      const sb = new SceneBuilder();
      sb.pushOffset(10, 20);
      if (withView) sb.addPlatformView(7, 0, 0, 300, 200);
      sb.addPlatformView(0, 0, 0, 10, 10);   // id 0: 忽略
      sb.addPlatformView(8, 0, 0, 0, 10);    // 空尺寸: 忽略
      sb.pop();
      window.render(sb.build());
    };
  )JS").ok());
  Eval("window.onDrawFrame = () => draw(true)");
  bindings_->DrawFrame();
  Eval("window.onDrawFrame = () => draw(false)");
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 2u);
  EXPECT_TRUE(client_.rendered[0]->has_platform_views());
  EXPECT_FALSE(client_.rendered[1]->has_platform_views()) << "builder flag resets after build()";
  const auto& offset = client_.rendered[0]->root()->children();
  ASSERT_EQ(offset.size(), 1u);
  const auto* container = static_cast<const graphics::ContainerLayer*>(offset[0].get());
  ASSERT_EQ(container->children().size(), 1u);
  const auto* view = static_cast<const graphics::PlatformViewLayer*>(container->children()[0].get());
  EXPECT_EQ(view->view_id(), 7);
  EXPECT_EQ(view->rect(), (Rect{0, 0, 300, 200}));
}

// 多视图: createView 经宿主创建 → views 中出现新视图 → Promise 解析为该视图;按视图 render / close。
TEST_F(UiBindingsTest, ViewsCreateRenderAndClose) {
  ASSERT_TRUE(Run(R"JS(
    import { window, views, createView, SceneBuilder } from 'momo:ui';
    globalThis.window = window;
    globalThis.views = views;
    globalThis.changes = 0;
    window.onViewsChanged = () => { globalThis.changes++; };
    createView({ title: 'second', width: 300, height: 200 }).then((v) => {
      globalThis.created = v;
    });
    globalThis.draw = () => {
      for (const v of views.values()) v.render(new SceneBuilder().build());
    };
  )JS").ok());
  ASSERT_EQ(client_.create_requests.size(), 1u);
  const auto& [request, options] = client_.create_requests[0];
  EXPECT_EQ(options.title, "second");
  EXPECT_EQ(options.size, (Size{300, 200}));

  // 宿主创建窗口: 视图先加入(onViewsChanged), 再结算 Promise。
  client_.view_ids = {0, 5};
  bindings_->ViewAdded(5);
  bindings_->ViewCreateResult(request, platform::ViewId{5});
  EXPECT_EQ(Eval("String(created.id)"), "5");
  EXPECT_EQ(Eval("String(views.size)"), "2");
  EXPECT_EQ(Eval("String(changes)"), "1");
  EXPECT_EQ(Eval("String(created.physicalSize.width)"), "300");  // extra_metrics
  EXPECT_EQ(Eval("String(views.get(0) === window)"), "true");

  // 一帧里每个视图各提交一棵树, 带各自的 view id 与尺寸。
  Eval("window.onDrawFrame = () => draw()");
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 2u);
  EXPECT_EQ(client_.rendered[0]->view_id(), 0);
  EXPECT_EQ(client_.rendered[1]->view_id(), 5);
  EXPECT_EQ(client_.rendered[1]->frame_size(), (Size{300, 200}));

  // 按视图的事件只到达该视图的处理器。
  Eval("globalThis.got = []; created.onFocusChanged = (f) => got.push('5:' + f); window.onFocusChanged = (f) => got.push('0:' + f)");
  bindings_->FocusChanged(5, true);
  EXPECT_EQ(Eval("got.join()"), "5:true");

  // view.close() 请求宿主关闭;主窗口不能从 JS 关闭。
  Eval("created.close()");
  EXPECT_EQ(client_.closed, (std::vector<ViewId>{5}));
  EXPECT_EQ(Eval("(() => { try { window.close(); return 'no'; } catch (e) { return 'threw'; } })()"), "threw");
  bindings_->ViewRemoved(5);
  EXPECT_EQ(Eval("String(views.size)"), "1");
  EXPECT_EQ(Eval("String(changes)"), "2");
  // 已移除视图的 render 为空操作。
  client_.rendered.clear();
  ASSERT_TRUE(Run(R"JS(
    import { SceneBuilder } from 'momo:ui';
    globalThis.created.render(new SceneBuilder().build());
  )JS").ok());
  EXPECT_TRUE(client_.rendered.empty());
}

// 关闭的视图: 引擎只弱持有, JS 不再引用时绑定被回收;JS 仍持有旧对象/旧方法时调用安全且为空操作。
TEST_F(UiBindingsTest, RemovedViewsAreCollectedWhenUnreferenced) {
  ASSERT_TRUE(Run(R"JS(
    import { views } from 'momo:ui';
    globalThis.views = views;
  )JS").ok());
  v8::Isolate* isolate = runtime_->impl().isolate();
  std::vector<ViewId> ids;
  for (ViewId id = 10; id < 15; ++id) {
    client_.view_ids.push_back(id);
    bindings_->ViewAdded(id);
    ids.push_back(id);
  }
  EXPECT_EQ(Eval("String(views.size)"), "6");
  // 保留其中一个视图的旧方法与另一个视图对象, 并挂上事件回调。
  Eval("globalThis.oldRender = views.get(10).render; globalThis.keep = views.get(11);"
       "keep.onFocusChanged = () => {}; 'ok'");
  client_.view_ids = {platform::kImplicitViewId};
  for (ViewId id : ids) bindings_->ViewRemoved(id);
  EXPECT_EQ(Eval("String(views.size)"), "1");
  EXPECT_EQ(Eval("String(keep.onFocusChanged)"), "null") << "handlers are cleared on removal";
  isolate->LowMemoryNotification();
  EXPECT_EQ(bindings_->detached_view_count_for_testing(), 2u) << "only views still referenced by JS survive";

  // 旧方法/旧对象仍可安全调用(空操作)。
  client_.rendered.clear();
  ASSERT_TRUE(Run(R"JS(
    import { SceneBuilder } from 'momo:ui';
    globalThis.oldRender(new SceneBuilder().build());
    globalThis.keep.render(new SceneBuilder().build());
    globalThis.keep.title = 'x';
  )JS").ok());
  EXPECT_TRUE(client_.rendered.empty());
  bindings_->FocusChanged(11, true);  // 已移除视图: 忽略

  Eval("globalThis.oldRender = null; globalThis.keep = null; 'ok'");
  isolate->LowMemoryNotification();
  EXPECT_EQ(bindings_->detached_view_count_for_testing(), 0u);
}

// createView 失败: Promise 被拒绝。
TEST_F(UiBindingsTest, CreateViewFailureRejects) {
  ASSERT_TRUE(Run(R"JS(
    import { createView } from 'momo:ui';
    globalThis.err = '';
    createView({ width: 100, height: 100 }).catch((e) => { globalThis.err = String(e.message); });
  )JS").ok());
  ASSERT_EQ(client_.create_requests.size(), 1u);
  bindings_->ViewCreateResult(client_.create_requests[0].first, UnavailableError("no windows"));
  EXPECT_NE(Eval("err").find("no windows"), std::string::npos);
}

TEST_F(UiBindingsTest, TexturesAndNewLayersComposite) {
  std::shared_ptr<graphics::Texture> texture = textures_->Register();
  std::vector<uint8_t> blue(4 * 4 * 4);
  for (size_t i = 0; i < blue.size(); i += 4) {
    blue[i + 2] = 255;
    blue[i + 3] = 255;
  }
  ASSERT_TRUE(texture->UpdateRgba(blue.data(), 4, 4, 16).ok());
  ASSERT_TRUE(Run(R"JS(
    import { window, SceneBuilder, PictureRecorder, Paint } from 'momo:ui';
    globalThis.window = window;
    globalThis.draw = (id) => {
      const r = new PictureRecorder();
      const c = r.beginRecording();
      const p = new Paint();
      p.color = 0xFF00FF00;
      c.drawRect(0, 0, 100, 100, p);
      const sb = new SceneBuilder();
      sb.pushClipRRect(0, 0, 100, 100, 30).addPicture(0, 0, r.endRecording()).pop();
      sb.pushOpacity(128).addTexture(id, 200, 0, 50, 50).pop();
      window.render(sb.build());
    };
  )JS").ok());
  const std::string id = std::to_string(texture->id());
  Eval("window.onDrawFrame = () => draw(" + id + ")");
  bindings_->DrawFrame();
  ASSERT_EQ(client_.rendered.size(), 1u);
  graphics::OwnedPixelBuffer pixels(400, 270);
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view(), 0xFFFFFFFF).ok());
  EXPECT_EQ(pixels.GetColor(50, 50), 0xFF00FF00u);
  EXPECT_EQ(pixels.GetColor(2, 2), 0xFFFFFFFFu) << "corner clipped by the rounded rect";
  const uint32_t t = pixels.GetColor(225, 25);
  EXPECT_NEAR(static_cast<int>(t & 0xFF), 255, 2);           // 蓝
  EXPECT_NEAR(static_cast<int>((t >> 16) & 0xFF), 127, 3);   // 50% 透明度叠在白底上

  // 纹理推送新帧后, 同一棵层树重画即取最新帧(不需要重新录制)。
  std::vector<uint8_t> red(4 * 4 * 4);
  for (size_t i = 0; i < red.size(); i += 4) {
    red[i] = 255;
    red[i + 3] = 255;
  }
  ASSERT_TRUE(texture->UpdateRgba(red.data(), 4, 4, 16).ok());
  ASSERT_TRUE(graphics::RasterizeLayerTree(*client_.rendered[0], pixels.view(), 0xFFFFFFFF).ok());
  EXPECT_NEAR(static_cast<int>((pixels.GetColor(225, 25) >> 16) & 0xFF), 255, 2);
}

}  // namespace
}  // namespace momo::engine::ui
