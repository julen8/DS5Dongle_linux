#pragma once
#include <utility>

namespace defer_detail {

template <typename F>
class DeferGuard {
public:
    explicit DeferGuard(F&& f) noexcept : f_(std::forward<F>(f)) {}
    ~DeferGuard() { f_(); }  // RAII:析构时执行

    DeferGuard(const DeferGuard&) = delete;  // 禁止拷贝
    DeferGuard& operator=(const DeferGuard&) = delete;
    DeferGuard(DeferGuard&&) = delete;  // 也不需要移动
    DeferGuard& operator=(DeferGuard&&) = delete;

private:
    F f_;
};

struct DeferTag {};

// 让 `DeferTag{} + [lambda]` 自动推导出 DeferGuard<Lambda>
template <typename F>
DeferGuard<F> operator+(DeferTag, F&& f) {
    return DeferGuard<F>(std::forward<F>(f));
}

}  // namespace defer_detail

// 拼接出唯一的局部变量名,避免同一作用域多个 defer 冲突
#define DEFER_CONCAT_(a, b) a##b
#define DEFER_CONCAT(a, b) DEFER_CONCAT_(a, b)

#ifdef __COUNTER__
#    define DEFER_VAR DEFER_CONCAT(_defer_, __COUNTER__)
#else
#    define DEFER_VAR DEFER_CONCAT(_defer_, __LINE__)
#endif

// 关键宏:使用方法 ——  defer { ...code... };
#define defer auto DEFER_VAR = ::defer_detail::DeferTag{} + [&]() -> void
