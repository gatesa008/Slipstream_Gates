#pragma once

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>
#include <windows.h>

template<typename... T>
void SafeRelease(T*&... p) {
    ((p ? (p->Release(), p = nullptr) : nullptr), ...);
}

template<typename Q, typename M>
void ClearQueue(Q& q, M& mtx) {
    std::lock_guard<M> lk(mtx);
    while (!q.empty()) q.pop();
}

template<typename Q, typename V>
void PushBoundedQueue(Q& q, size_t maxSize, V&& value) {
    if (q.size() >= maxSize) q.pop();
    q.push(std::forward<V>(value));
}

template<typename T>
void WritePod(uint8_t* dst, const T& value) {
    std::memcpy(dst, &value, sizeof(T));
}

template<typename T>
[[nodiscard]] T ReadPod(const uint8_t* src) {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

template<typename T>
[[nodiscard]] bool ReadPod(const uint8_t* data, size_t len, T& out) {
    if (len < sizeof(T)) return false;
    out = ReadPod<T>(data);
    return true;
}

inline int64_t GetTimestamp() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    // Split into whole seconds + remainder so the intermediate product can't overflow. The old
    // `counter.QuadPart * 1000000` overflows int64 after ~10.6 days of uptime at a 10 MHz QPC,
    // wrapping the timestamp negative; the client reads it as unsigned (~1.84e19) and every frame
    // is rejected by EncodedVideoChunk. A process restart does not help because QPC counts from
    // system boot. `whole * 1000000` is safe for ~292000 years; `rem < frequency` keeps the
    // fractional product well inside int64.
    const int64_t ticks = counter.QuadPart;
    const int64_t whole = ticks / frequency;
    const int64_t rem = ticks % frequency;
    return whole * 1000000LL + (rem * 1000000LL) / frequency;
}
