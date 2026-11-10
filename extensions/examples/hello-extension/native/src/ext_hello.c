/* ext_hello.c — 最小示例扩展(纯 C 实现, 同时作为 C ABI 兼容性验证: 不依赖任何 C++ 运行时)。
 *
 * ext:hello 模块:
 *   add(a, b)            同步: 数字相加
 *   greet(name)          同步: 字符串往返(UTF-8)
 *   reverseBytes(bytes)  同步: 返回字节逆序的新 ArrayBuffer
 *   hashAsync(bytes)     异步: 在 worker pool 上算 FNV-1a 64 位哈希, Promise 解析为 8 字节(LE)
 * 消息通道 hello/upper: 在 worker pool 上把 ASCII 文本转大写后回复。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "momo/momo_extension.h"

static const MomoHostApi* g_host = NULL;
static MomoEngineRef g_engine = NULL;

static void Add(void* user_data, MomoCallInfoRef call) {
  double a = 0, b = 0;
  (void)user_data;
  if (g_host->call->arg_double(call, 0, &a) != kMomoOk ||
      g_host->call->arg_double(call, 1, &b) != kMomoOk) {
    g_host->call->throw_error(call, "add(a, b) expects two numbers");
    return;
  }
  g_host->call->return_double(call, a + b);
}

static void Greet(void* user_data, MomoCallInfoRef call) {
  size_t len = 0;
  char* name;
  char* out;
  (void)user_data;
  if (g_host->call->arg_string_utf8(call, 0, NULL, 0, &len) != kMomoOk) {
    g_host->call->throw_error(call, "greet(name) expects a string");
    return;
  }
  name = (char*)malloc(len + 1);
  g_host->call->arg_string_utf8(call, 0, name, len + 1, &len);
  out = (char*)malloc(len + 16);
  snprintf(out, len + 16, "Hello, %s!", name);
  g_host->call->return_string_utf8(call, out, strlen(out));
  free(out);
  free(name);
}

static void ReverseBytes(void* user_data, MomoCallInfoRef call) {
  const uint8_t* data = NULL;
  size_t len = 0, i;
  uint8_t* out;
  (void)user_data;
  if (g_host->call->arg_bytes(call, 0, &data, &len) != kMomoOk) {
    g_host->call->throw_error(call, "reverseBytes(bytes) expects an ArrayBuffer or typed array");
    return;
  }
  out = (uint8_t*)malloc(len ? len : 1);
  for (i = 0; i < len; ++i) out[i] = data[len - 1 - i];
  g_host->call->return_bytes_copy(call, out, len);
  free(out);
}

/* 异步任务: 输入拷贝 + promise token。 */
typedef struct HashJob {
  uint8_t* data;
  size_t len;
  uint64_t token;
} HashJob;

static void RunHash(void* user_data) {
  HashJob* job = (HashJob*)user_data;
  uint64_t h = 1469598103934665603ULL;
  uint8_t out[8];
  size_t i;
  for (i = 0; i < job->len; ++i) {
    h ^= job->data[i];
    h *= 1099511628211ULL;
  }
  for (i = 0; i < 8; ++i) out[i] = (uint8_t)(h >> (8 * i));
  g_host->promise_resolve_bytes(g_engine, job->token, out, sizeof(out));
  free(job->data);
  free(job);
}

static void HashAsync(void* user_data, MomoCallInfoRef call) {
  const uint8_t* data = NULL;
  size_t len = 0;
  HashJob* job;
  (void)user_data;
  if (g_host->call->arg_bytes(call, 0, &data, &len) != kMomoOk) {
    g_host->call->throw_error(call, "hashAsync(bytes) expects an ArrayBuffer or typed array");
    return;
  }
  job = (HashJob*)calloc(1, sizeof(HashJob));
  job->data = (uint8_t*)malloc(len ? len : 1); /* 参数视图只在回调期间有效: 拷贝 */
  memcpy(job->data, data, len);
  job->len = len;
  g_host->call->return_promise(call, &job->token);
  if (g_host->post_task(g_engine, kMomoRunnerWorkerPool, RunHash, job) != kMomoOk) {
    g_host->promise_reject(g_engine, job->token, "worker pool unavailable");
    free(job->data);
    free(job);
  }
}

static void OnUpper(void* user_data, const uint8_t* bytes, size_t len, uint64_t reply_token) {
  uint8_t* out = (uint8_t*)malloc(len ? len : 1);
  size_t i;
  (void)user_data;
  for (i = 0; i < len; ++i) {
    out[i] = (bytes[i] >= 'a' && bytes[i] <= 'z') ? (uint8_t)(bytes[i] - 32) : bytes[i];
  }
  g_host->channel_reply(g_engine, reply_token, out, len);
  free(out);
}

static void SetupModule(void* user_data, MomoJsBuilderRef builder) {
  (void)user_data;
  g_host->builder_add_function(builder, "add", Add, NULL);
  g_host->builder_add_function(builder, "greet", Greet, NULL);
  g_host->builder_add_function(builder, "reverseBytes", ReverseBytes, NULL);
  g_host->builder_add_function(builder, "hashAsync", HashAsync, NULL);
}

static void OnAttach(void* user_data, const MomoHostApi* host, MomoEngineRef engine) {
  (void)user_data;
  g_host = host;
  g_engine = engine;
  host->register_module(engine, "ext:hello", SetupModule, NULL);
  host->channel_set_handler(engine, "hello/upper", kMomoRunnerWorkerPool, OnUpper, NULL);
  host->log(kMomoLogInfo, "hello", "hello extension attached");
}

static void OnDetach(void* user_data) {
  (void)user_data;
  g_host->channel_set_handler(g_engine, "hello/upper", kMomoRunnerPlatform, NULL, NULL);
  g_host = NULL;
  g_engine = NULL;
}

MOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_api_version, MomoExtension* out) {
  if (host_api_version < MOMO_EXT_API_VERSION || out == NULL) {
    return false;
  }
  out->id = "momo.example.hello";
  out->user_data = NULL;
  out->on_attach = OnAttach;
  out->on_detach = OnDetach;
  return true;
}
