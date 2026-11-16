/* ext_lifecycle.c — 测试用插件: 把宿主对它的调用、以及它在各个时刻调用宿主得到的状态, 逐行追加到日志文件。
 *
 *   attach(nativeHost, logPath)   注册平台视图类型 "test.view";此后记录:
 *       create:<id>     工厂 create(platform 线程)
 *       dispose:<id>    工厂 dispose
 *       task            postTask 投递的平台任务执行了
 *       cleanup:<n>     运行时销毁时的清理函数;n 是此刻 post_platform_task 的返回值(应为 kMomoErrClosed)
 *   postTask()                    投递一个平台任务
 */
#include <stdio.h>
#include <string.h>

#include <node_api.h>

#include "momo/momo_host.h"

static const MomoHost* g_host = NULL;
static char g_log_path[1024];
static int g_view; /* 平台视图的"原生视图": 只要是个非空指针 */

static void Log(const char* line) {
  FILE* file = fopen(g_log_path, "a");
  if (file != NULL) {
    fprintf(file, "%s\n", line);
    fclose(file);
  }
}

static void* CreateView(void* user_data, int64_t view_id, const uint8_t* params, size_t len) {
  char line[64];
  (void)user_data;
  (void)params;
  (void)len;
  snprintf(line, sizeof(line), "create:%lld", (long long)view_id);
  Log(line);
  return &g_view;
}

static void DisposeView(void* user_data, int64_t view_id, void* native_view) {
  char line[64];
  (void)user_data;
  (void)native_view;
  snprintf(line, sizeof(line), "dispose:%lld", (long long)view_id);
  Log(line);
}

static void RunTask(void* user_data) {
  (void)user_data;
  Log("task");
}

static void Cleanup(void* data) {
  char line[64];
  (void)data;
  snprintf(line, sizeof(line), "cleanup:%d", (int)g_host->post_platform_task(g_host, RunTask, NULL));
  Log(line);
}

static napi_value Attach(napi_env env, napi_callback_info info) {
  size_t argc = 2, len = 0;
  napi_value argv[2], result;
  const MomoHost* host;
  MomoPlatformViewFactory factory;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  host = argc < 2 ? NULL : momo_host_from_napi(env, argv[0]);
  if (host == NULL ||
      napi_get_value_string_utf8(env, argv[1], g_log_path, sizeof(g_log_path), &len) != napi_ok) {
    napi_throw_type_error(env, NULL, "attach(nativeHost, logPath)");
    return NULL;
  }
  g_host = host;
  memset(&factory, 0, sizeof(factory));
  factory.struct_size = sizeof(factory);
  factory.create = CreateView;
  factory.dispose = DisposeView;
  napi_add_env_cleanup_hook(env, Cleanup, NULL);
  napi_create_int32(env, (int32_t)g_host->platform_view_register_factory(g_host, "test.view", &factory),
                    &result);
  return result;
}

static napi_value PostTask(napi_env env, napi_callback_info info) {
  napi_value result;
  (void)info;
  napi_create_int32(env, (int32_t)g_host->post_platform_task(g_host, RunTask, NULL), &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "attach", NAPI_AUTO_LENGTH, Attach, NULL, &fn);
  napi_set_named_property(env, exports, "attach", fn);
  napi_create_function(env, "postTask", NAPI_AUTO_LENGTH, PostTask, NULL, &fn);
  napi_set_named_property(env, exports, "postTask", fn);
  return exports;
}
