// 无 V8 的构建(sanitizer): CreateAppDriver 总是返回占位驱动;扩展 C ABI 的 JS 部分一律返回错误
// (没有 JS 运行时, ext:* 模块不会被安装, 这些函数正常情况下不会被调用)。

#include "base/logging.h"
#include "engine/ui/app_driver.h"

namespace momo::engine::ui {

namespace {

size_t ArgCount(MomoCallInfoRef) { return 0; }
MomoStatus ArgDouble(MomoCallInfoRef, size_t, double*) { return kMomoErrInternal; }
MomoStatus ArgBool(MomoCallInfoRef, size_t, bool*) { return kMomoErrInternal; }
MomoStatus ArgString(MomoCallInfoRef, size_t, char*, size_t, size_t*) { return kMomoErrInternal; }
MomoStatus ArgBytes(MomoCallInfoRef, size_t, const uint8_t**, size_t*) { return kMomoErrInternal; }
void ReturnDouble(MomoCallInfoRef, double) {}
void ReturnBool(MomoCallInfoRef, bool) {}
void ReturnString(MomoCallInfoRef, const char*, size_t) {}
void ReturnBytes(MomoCallInfoRef, const uint8_t*, size_t) {}
void ReturnPromise(MomoCallInfoRef, uint64_t* token) {
  if (token != nullptr) *token = 0;
}
void Throw(MomoCallInfoRef, const char*) {}
void ReturnNull(MomoCallInfoRef) {}

const MomoCallApi kCallApi = {
    sizeof(MomoCallApi), &ArgCount,     &ArgDouble,  &ArgBool,      &ArgString,
    &ArgBytes,           &ReturnDouble, &ReturnBool, &ReturnString, &ReturnBytes,
    &ReturnPromise,      &Throw,        &ReturnNull,
};

MomoStatus AddFunction(MomoJsBuilderRef, const char*, MomoNativeFn, void*) { return kMomoErrInternal; }

}  // namespace

StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings, WindowClient& client,
                                                     const AppHost&, uint64_t) {
  MOMO_LOG(INFO) << "Built without a JS runtime (bundle "
                 << (settings.bundle_path.empty() ? "<none>" : settings.bundle_path.string())
                 << " not executed; placeholder frames).";
  return std::unique_ptr<AppDriver>(std::make_unique<PlaceholderDriver>(client));
}

const ExtensionJsGlue& GetExtensionJsGlue() {
  static const ExtensionJsGlue glue{&kCallApi, &AddFunction};
  return glue;
}

}  // namespace momo::engine::ui
