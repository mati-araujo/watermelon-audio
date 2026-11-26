/**
 * rtalloc_counter.cpp — ver RtAllocCounter.h. TEST DOUBLE, host test build only.
 */

#include "RtAllocCounter.h"

#include <cstdlib>
#include <new>

namespace {

thread_local bool tArmed = false;
thread_local int64_t tCount = 0;

void* counted(std::size_t size) {
    if (tArmed) ++tCount;
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

}  // namespace

namespace wma_test {

void armAllocCounter() {
    tCount = 0;
    tArmed = true;
}

int64_t disarmAllocCounter() {
    tArmed = false;
    return tCount;
}

}  // namespace wma_test

void* operator new(std::size_t size) { return counted(size); }
void* operator new[](std::size_t size) { return counted(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return counted(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return counted(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
