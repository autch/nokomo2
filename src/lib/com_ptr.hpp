#pragma once
// 最小限の COM スマートポインタ。
// KPI の参照カウント規約(取得時カウント1、Release で解放)を RAII で扱う。

namespace nokomo::detail {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~ComPtr() { reset(); }

    // 参照カウントを増やさずに所有権を引き取る(new 直後や獲得系メソッドの戻り値用)
    void attach(T* p) { reset(); p_ = p; }
    T* detach() { T* p = p_; p_ = nullptr; return p; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // Open(..., &ptr) 形式の出力引数へ渡す(既存の保持は解放)
    T** put() { reset(); return &p_; }

private:
    T* p_ = nullptr;
};

} // namespace nokomo::detail
