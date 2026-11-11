#include "engine/runtime/js_runtime.h"

#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/event_loop.h"
#include "engine/runtime/js_runtime_impl.h"

namespace momo::engine::runtime {

StatusOr<std::unique_ptr<JsRuntime>> JsRuntime::Create(const Options& options) {
  EventLoop* loop = EventLoop::Current();
  if (loop == nullptr) {
    return FailedPreconditionError("JsRuntime must be created on a thread running an EventLoop");
  }
  std::vector<std::string> node_args;
  if (options.enable_inspector) {
    node_args.push_back(std::string(options.inspector_wait ? "--inspect-wait" : "--inspect") +
                        "=127.0.0.1:" + std::to_string(options.inspector_port));
  }
  JsRuntimeImpl::Options impl_options;
  impl_options.code_cache_dir = options.code_cache_dir;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<JsRuntimeImpl> impl,
                        JsRuntimeImpl::Create(loop->GetTaskRunner(), impl_options, node_args));
  return std::unique_ptr<JsRuntime>(new JsRuntime(std::move(impl)));
}

JsRuntime::JsRuntime(std::unique_ptr<JsRuntimeImpl> impl) : impl_(std::move(impl)) {}

JsRuntime::~JsRuntime() { MOMO_DCHECK_ON_AFFINITY_THREAD(); }

Status JsRuntime::RunMainModule(const std::filesystem::path& path) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
  if (!std::filesystem::is_regular_file(absolute, ec)) {
    return NotFoundError("JS bundle is not readable: " + absolute.string());
  }
  return impl_->RunModule(absolute.string(), nullptr, absolute.parent_path());
}

Status JsRuntime::RunModuleSource(std::string_view source, std::string_view url,
                                  const std::filesystem::path& base_dir) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  const std::string text(source);
  return impl_->RunModule(std::string(url), &text, base_dir);
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
