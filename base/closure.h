#pragma once

// MoveOnlyFunction<R(Args...)>: 仅可移动的可调用对象(C++23 std::move_only_function 的子集)。
// std::function 要求可拷贝, 无法捕获 unique_ptr / 大对象的移动语义, 跨线程投递一律用本类型。
// Closure = MoveOnlyFunction<void()>。空对象调用: void 返回值时为空操作, 否则 CHECK 失败。
// header-only, 属于 momo_base。

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace momo {

template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
 public:
  MoveOnlyFunction() = default;
  MoveOnlyFunction(std::nullptr_t) {}  // NOLINT(implicit)

  template <typename F,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
                                        !std::is_same_v<std::decay_t<F>, std::nullptr_t> &&
                                        std::is_invocable_r_v<R, std::decay_t<F>&, Args...>>>
  MoveOnlyFunction(F&& f)  // NOLINT(implicit)
      : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

  MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;
  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

  explicit operator bool() const { return impl_ != nullptr; }

  R operator()(Args... args) {
    if constexpr (std::is_void_v<R>) {
      if (impl_) {
        impl_->Run(std::forward<Args>(args)...);
      }
    } else {
      if (!impl_) {
        std::abort();  // 无返回值可给: 调用空的非 void 函数是编程错误
      }
      return impl_->Run(std::forward<Args>(args)...);
    }
  }

 private:
  struct Base {
    virtual ~Base() = default;
    virtual R Run(Args... args) = 0;
  };

  template <typename F>
  struct Impl final : Base {
    template <typename G>
    explicit Impl(G&& g) : f(std::forward<G>(g)) {}
    R Run(Args... args) override { return f(std::forward<Args>(args)...); }
    F f;
  };

  std::unique_ptr<Base> impl_;
};

using Closure = MoveOnlyFunction<void()>;

}  // namespace momo
