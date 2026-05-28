#pragma once

#include <utility>

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    ~ComPtr() {
        reset();
    }

    T* get() const {
        return ptr_;
    }

    T** put() {
        reset();
        return &ptr_;
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

    void reset(T* ptr = nullptr) {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = ptr;
    }

private:
    T* ptr_ = nullptr;
};
