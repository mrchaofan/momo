/* ext_hello.c — 最小的原生扩展: 一个普通的 Node 插件(纯 C, 只用 Node-API)。
 *
 * 它不需要宿主能力表(momo_host.h): 不碰原生 UI、纹理、平台视图的扩展, 与给 Node 写的插件没有区别。
 *
 *   add(a, b)            同步: 数字相加
 *   greet(name)          同步: 字符串往返(UTF-8)
 *   reverseBytes(bytes)  同步: 返回字节逆序的新 Uint8Array
 *   hash(bytes)          异步: 在 Node 的线程池上算 FNV-1a 64 位哈希, Promise 解析为 16 位十六进制字符串
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <node_api.h>

static napi_value Throw(napi_env env, const char* message) {
  napi_throw_type_error(env, NULL, message);
  return NULL;
}

static napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], result;
  double a, b;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2 || napi_get_value_double(env, argv[0], &a) != napi_ok ||
      napi_get_value_double(env, argv[1], &b) != napi_ok) {
    return Throw(env, "add(a, b) expects two numbers");
  }
  napi_create_double(env, a + b, &result);
  return result;
}

static napi_value Greet(napi_env env, napi_callback_info info) {
  size_t argc = 1, len = 0;
  napi_value argv[1], result;
  char* name;
  char* out;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1 || napi_get_value_string_utf8(env, argv[0], NULL, 0, &len) != napi_ok) {
    return Throw(env, "greet(name) expects a string");
  }
  name = (char*)malloc(len + 1);
  napi_get_value_string_utf8(env, argv[0], name, len + 1, &len);
  out = (char*)malloc(len + 16);
  snprintf(out, len + 16, "Hello, %s!", name);
  napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &result);
  free(out);
  free(name);
  return result;
}

/* Uint8Array 参数的字节视图(只在本次调用期间有效)。 */
static int GetBytes(napi_env env, napi_value value, const uint8_t** data, size_t* len) {
  napi_typedarray_type type;
  void* ptr = NULL;
  if (napi_get_typedarray_info(env, value, &type, len, &ptr, NULL, NULL) != napi_ok ||
      type != napi_uint8_array) {
    return 0;
  }
  *data = (const uint8_t*)ptr;
  return 1;
}

static napi_value ReverseBytes(napi_env env, napi_callback_info info) {
  size_t argc = 1, len = 0, i;
  napi_value argv[1], buffer, result;
  const uint8_t* data = NULL;
  void* out = NULL;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1 || !GetBytes(env, argv[0], &data, &len)) {
    return Throw(env, "reverseBytes(bytes) expects a Uint8Array");
  }
  napi_create_arraybuffer(env, len, &out, &buffer);
  for (i = 0; i < len; ++i) ((uint8_t*)out)[i] = data[len - 1 - i];
  napi_create_typedarray(env, napi_uint8_array, len, buffer, 0, &result);
  return result;
}

/* 异步任务: 输入的拷贝 + 结果 + Promise。 */
typedef struct HashJob {
  uint8_t* data;
  size_t len;
  uint64_t hash;
  napi_deferred deferred;
  napi_async_work work;
} HashJob;

/* 线程池线程: 不得调用 Node-API。 */
static void RunHash(napi_env env, void* data) {
  HashJob* job = (HashJob*)data;
  uint64_t h = 1469598103934665603ULL;
  size_t i;
  (void)env;
  for (i = 0; i < job->len; ++i) {
    h ^= job->data[i];
    h *= 1099511628211ULL;
  }
  job->hash = h;
}

/* 回到 JS 线程。 */
static void FinishHash(napi_env env, napi_status status, void* data) {
  HashJob* job = (HashJob*)data;
  napi_value value;
  char hex[17];
  if (status == napi_ok) {
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)job->hash);
    napi_create_string_utf8(env, hex, 16, &value);
    napi_resolve_deferred(env, job->deferred, value);
  } else { /* 运行时正在关闭: 任务被取消 */
    napi_create_string_utf8(env, "cancelled", NAPI_AUTO_LENGTH, &value);
    napi_reject_deferred(env, job->deferred, value);
  }
  napi_delete_async_work(env, job->work);
  free(job->data);
  free(job);
}

static napi_value Hash(napi_env env, napi_callback_info info) {
  size_t argc = 1, len = 0;
  napi_value argv[1], promise, name;
  const uint8_t* data = NULL;
  HashJob* job;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1 || !GetBytes(env, argv[0], &data, &len)) {
    return Throw(env, "hash(bytes) expects a Uint8Array");
  }
  job = (HashJob*)calloc(1, sizeof(HashJob));
  job->data = (uint8_t*)malloc(len ? len : 1);
  memcpy(job->data, data, len);
  job->len = len;
  napi_create_promise(env, &job->deferred, &promise);
  napi_create_string_utf8(env, "hello.hash", NAPI_AUTO_LENGTH, &name);
  napi_create_async_work(env, NULL, name, RunHash, FinishHash, job, &job->work);
  napi_queue_async_work(env, job->work);
  return promise;
}

NAPI_MODULE_INIT() {
  static const struct {
    const char* name;
    napi_callback fn;
  } functions[] = {{"add", Add}, {"greet", Greet}, {"reverseBytes", ReverseBytes}, {"hash", Hash}};
  size_t i;
  for (i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
    napi_value fn;
    napi_create_function(env, functions[i].name, NAPI_AUTO_LENGTH, functions[i].fn, NULL, &fn);
    napi_set_named_property(env, exports, functions[i].name, fn);
  }
  return exports;
}
