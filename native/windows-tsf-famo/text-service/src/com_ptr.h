#pragma once

#include <utility>

namespace famo::tsf {

template <typename T> class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T *value) : value_(value) {
    if (value_)
      value_->AddRef();
  }
  ComPtr(const ComPtr &other) : ComPtr(other.value_) {}
  ComPtr(ComPtr &&other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  ~ComPtr() { reset(); }

  ComPtr &operator=(const ComPtr &other) {
    if (this != &other) {
      ComPtr copy(other);
      swap(copy);
    }
    return *this;
  }
  ComPtr &operator=(ComPtr &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  T *get() const { return value_; }
  T *operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

  T **put() {
    reset();
    return &value_;
  }
  void reset(T *value = nullptr) {
    if (value_)
      value_->Release();
    value_ = value;
  }
  T *detach() { return std::exchange(value_, nullptr); }
  void swap(ComPtr &other) noexcept { std::swap(value_, other.value_); }

private:
  T *value_ = nullptr;
};

template <typename T, typename U> bool SameComObject(T *left, U *right) {
  if (!left || !right)
    return left == nullptr && right == nullptr;
  ComPtr<IUnknown> left_identity;
  ComPtr<IUnknown> right_identity;
  if (FAILED(left->QueryInterface(IID_IUnknown,
                                  reinterpret_cast<void **>(left_identity.put()))) ||
      FAILED(right->QueryInterface(
          IID_IUnknown, reinterpret_cast<void **>(right_identity.put()))))
    return false;
  return left_identity.get() == right_identity.get();
}

} // namespace famo::tsf
