#include "engine/runtime/js_runtime.h"

#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "base/event_loop.h"
#include "engine/runtime/js_runtime_impl.h"

namespace momo::engine::runtime {

StatusOr<std::unique_ptr<JsRuntime>> JsRuntime::Create(const Options& options) {
  EventLoop* loop = EventLoop::Current();
  if (loop == nullptr) {
    return FailedPreconditionError("JsRuntime must be created on a thread running an EventLoop");
  }
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<JsRuntimeImpl> impl,
                        JsRuntimeImpl::Create(options.icu_data_path, loop->GetTaskRunner()));
  impl->SetCodeCacheDir(options.code_cache_dir);
  return std::unique_ptr<JsRuntime>(new JsRuntime(std::move(impl)));
}

JsRuntime::JsRuntime(std::unique_ptr<JsRuntimeImpl> impl) : impl_(std::move(impl)) {}

JsRuntime::~JsRuntime() { MOMO_DCHECK_ON_AFFINITY_THREAD(); }

Status JsRuntime::RunMainModule(const std::filesystem::path& path) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
  std::ifstream probe(absolute, std::ios::binary);
  if (!probe) {
    return NotFoundError("JS bundle is not readable: " + absolute.string());
  }
  std::string source((std::istreambuf_iterator<char>(probe)), std::istreambuf_iterator<char>());
  return impl_->RunModule(source, absolute.string(), absolute.parent_path());
}

Status JsRuntime::RunModuleSource(std::string_view source, std::string_view url,
                                  const std::filesystem::path& base_dir) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  return impl_->RunModule(source, std::string(url), base_dir);
}

StatusOr<std::string> JsRuntime::EvalToString(std::string_view source, std::string_view url) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  return impl_->EvalToString(source, url);
}

void JsRuntime::DrainMicrotasks() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  impl_->DrainMicrotasks();
}

}  // namespace momo::engine::runtime
