#include <atomic>

class Spinlock {
public:
    Spinlock() : flag(ATOMIC_FLAG_INIT) {}
    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
            // busy loop
        }
    }

    void unlock() {
        flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag;
};
