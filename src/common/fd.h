// Owns a file descriptor. Used by the feed's multicast socket and by the context publisher.
#pragma once

#include <unistd.h>

#include <utility>

namespace fillscope {

// Owns a file descriptor and closes it on destruction: RAII, so a descriptor can't leak on an
// early return. Move-only, because two owners would close it twice.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_{fd} {}
    // std::exchange returns the old value and stores the new one: the moved-from Fd is left
    // empty (-1), so its destructor closes nothing.
    Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    // explicit: usable in `if (fd)`, but not silently converted to bool elsewhere.
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

}  // namespace fillscope
