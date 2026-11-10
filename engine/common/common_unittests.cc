#include <thread>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/standard_codec.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

std::vector<uint8_t> B(std::initializer_list<int> bytes) {
  std::vector<uint8_t> out;
  for (int b : bytes) out.push_back(static_cast<uint8_t>(b));
  return out;
}

TEST(StandardCodecTest, ScalarsMatchFlutterByteFormat) {
  EXPECT_EQ(EncodeStandard(EncodableValue()), B({0}));
  EXPECT_EQ(EncodeStandard(true), B({1}));
  EXPECT_EQ(EncodeStandard(false), B({2}));
  EXPECT_EQ(EncodeStandard(int32_t{-2}), B({3, 0xFE, 0xFF, 0xFF, 0xFF}));
  EXPECT_EQ(EncodeStandard(int64_t{1} << 40), B({4, 0, 0, 0, 0, 0, 1, 0, 0}));
  // float64: 标签后按 8 字节对齐(相对消息起点)。
  EXPECT_EQ(EncodeStandard(1.0), B({6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F}));
  EXPECT_EQ(EncodeStandard("hi"), B({7, 2, 'h', 'i'}));
}

TEST(StandardCodecTest, LongSizesUseEscapes) {
  const std::string s300(300, 'x');
  const std::vector<uint8_t> bytes = EncodeStandard(s300);
  ASSERT_EQ(bytes.size(), 1u + 3u + 300u);
  EXPECT_EQ(bytes[1], 254);
  EXPECT_EQ(bytes[2], 300 & 0xFF);
  EXPECT_EQ(bytes[3], 300 >> 8);
  auto decoded = DecodeStandard(bytes);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->AsString(), s300);
}

TEST(StandardCodecTest, NestedRoundTrip) {
  EncodableValue value = EncodableMap{
      {"list", EncodableList{int32_t{1}, 2.5, "s", EncodableValue(), true}},
      {"bytes", std::vector<uint8_t>{1, 2, 3}},
      {"ints", std::vector<int32_t>{-1, 7}},
      {"doubles", std::vector<double>{0.5, -3}},
      {int32_t{42}, "int key"},
  };
  auto decoded = DecodeStandard(EncodeStandard(value));
  ASSERT_TRUE(decoded.ok()) << decoded.status();
  EXPECT_EQ(*decoded, value);
  EXPECT_EQ(decoded->Find("bytes")->GetIf<std::vector<uint8_t>>()->size(), 3u);
}

TEST(StandardCodecTest, RejectsMalformedInput) {
  EXPECT_FALSE(DecodeStandard(B({7, 5, 'a'})).ok());         // 字符串截断
  EXPECT_FALSE(DecodeStandard(B({99})).ok());                // 未知标签
  EXPECT_FALSE(DecodeStandard(B({0, 0})).ok());              // 多余字节
  EXPECT_FALSE(DecodeStandard(B({12, 255, 255, 255, 255, 0x7F})).ok());  // 巨大长度
}

TEST(StandardCodecTest, MethodCallsUseFlutterWireFormat) {
  // method 值紧接 args 值(非 list);信封首字节 0/1。
  EXPECT_EQ(EncodeMethodCall({"a", int32_t{1}}), B({7, 1, 'a', 3, 1, 0, 0, 0}));
  EXPECT_EQ(EncodeSuccessEnvelope(true), B({0, 1}));
  EXPECT_EQ(EncodeErrorEnvelope("c", "m"), B({1, 7, 1, 'c', 7, 1, 'm', 0}));
  // float64 对齐以整个缓冲为基准: 信封字节 + 标签后补齐到 8。
  EXPECT_EQ(EncodeSuccessEnvelope(0.0).size(), 16u);
  auto call = DecodeMethodCall(B({7, 1, 'x'}));  // 省略 args
  ASSERT_TRUE(call.ok());
  EXPECT_TRUE(call->args.IsNull());
}

TEST(StandardCodecTest, MethodCallsAndEnvelopes) {
  auto call = DecodeMethodCall(EncodeMethodCall({"setText", "abc"}));
  ASSERT_TRUE(call.ok());
  EXPECT_EQ(call->method, "setText");
  EXPECT_EQ(call->args.AsString(), "abc");

  auto ok = DecodeEnvelope(EncodeSuccessEnvelope(int32_t{5}));
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok->AsInt(), 5);
  auto err = DecodeEnvelope(EncodeErrorEnvelope("denied", "no"));
  ASSERT_FALSE(err.ok());
  EXPECT_NE(std::string(err.status().message()).find("denied"), std::string::npos);
}

class HubTest : public ::testing::Test {
 protected:
  momo::testing::ScopedTestEventLoop loop_;
  std::shared_ptr<MessageChannelHub> hub_ = std::make_shared<MessageChannelHub>();
};

TEST_F(HubTest, NativeHandlerRepliesOnSenderRunner) {
  hub_->SetNativeHandler("echo", loop_.task_runner(), [](MessageBytes m, MessageReply reply) {
    m.push_back(9);
    reply.Send(std::move(m));
  });
  std::optional<MessageBytes> got;
  hub_->SendToNative("echo", {1}, loop_.task_runner(),
                     [&](std::optional<MessageBytes> r) { got = std::move(r); });
  loop_.RunUntilIdle();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, (MessageBytes{1, 9}));
}

TEST_F(HubTest, MissingHandlerOrDroppedReplyAnswersNull) {
  int replies = 0;
  bool was_null = false;
  hub_->SendToNative("nobody", {1}, loop_.task_runner(), [&](std::optional<MessageBytes> r) {
    replies++;
    was_null = !r.has_value();
  });
  loop_.RunUntilIdle();
  EXPECT_EQ(replies, 1);
  EXPECT_TRUE(was_null);

  hub_->SetNativeHandler("drop", loop_.task_runner(), [](MessageBytes, MessageReply) {});
  hub_->SendToNative("drop", {}, loop_.task_runner(), [&](std::optional<MessageBytes> r) {
    replies++;
    was_null = !r.has_value();
  });
  loop_.RunUntilIdle();
  EXPECT_EQ(replies, 2);
  EXPECT_TRUE(was_null);
}

TEST_F(HubTest, JsEndpointRemovedAfterPostIsNotCalled) {
  int calls = 0;
  hub_->SetJsEndpoint(loop_.task_runner(),
                      [&](const std::string&, MessageBytes, MessageReply) { calls++; });
  hub_->SendToJs("momo/lifecycle", {1});
  hub_->SetJsEndpoint(nullptr, nullptr);  // 例如 JS 运行时在任务执行前销毁
  loop_.RunUntilIdle();
  EXPECT_EQ(calls, 0);

  hub_->SetJsEndpoint(loop_.task_runner(),
                      [&](const std::string& ch, MessageBytes m, MessageReply reply) {
                        calls++;
                        EXPECT_EQ(ch, "c");
                        reply.Send(std::move(m));
                      });
  std::optional<MessageBytes> got;
  hub_->SendToJs("c", {3}, loop_.task_runner(), [&](std::optional<MessageBytes> r) { got = r; });
  loop_.RunUntilIdle();
  EXPECT_EQ(calls, 1);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, MessageBytes{3});
}

TEST_F(HubTest, ReplyFromAnotherThreadIsDeliveredOnSenderRunner) {
  std::thread::id handler_thread;
  hub_->SetNativeHandler("bg", loop_.task_runner(), [&](MessageBytes, MessageReply reply) {
    std::thread t([reply = std::move(reply), &handler_thread]() mutable {
      handler_thread = std::this_thread::get_id();
      reply.Send(MessageBytes{7});
    });
    t.join();
  });
  std::thread::id reply_thread;
  hub_->SendToNative("bg", {}, loop_.task_runner(), [&](std::optional<MessageBytes>) {
    reply_thread = std::this_thread::get_id();
  });
  loop_.RunUntilIdle();
  EXPECT_NE(handler_thread, std::this_thread::get_id());
  EXPECT_EQ(reply_thread, std::this_thread::get_id());
}

}  // namespace
}  // namespace momo::engine
