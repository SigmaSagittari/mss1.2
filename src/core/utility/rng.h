#pragma once

#include <cstdint>

namespace mss {

// splitmix64：小巧快速的 64 位伪随机混合函数
inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// 显式随机数对象。旧代码到处传 unsigned long long& seed，
// 这里收敛成一个对象，语义更清晰，也方便固定种子复现。
struct Rng {
    std::uint64_t state = 0;

    explicit Rng(std::uint64_t seed = 0) : state(seed) {}

    std::uint64_t next() {
        state = splitmix64(state);
        return state;
    }

    // [0, 1) 的均匀随机数，52 位精度（与原实现一致）
    long double nextUnit() {
        return static_cast<long double>(next() & 0xFFFFFFFFFFFFFULL) /
               static_cast<long double>(1ULL << 52);
    }
};

}  // namespace mss
