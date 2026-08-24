#pragma once

#include <cstddef>
#include <cstdint>

#include "core/utility/rng.h"

namespace mss {

// 128 位哈希值：两个独立的 64 位通道（lo/hi）。
// 同一批数据的碰撞概率约 2^-127，对 1e9 量级的数据也足够安全。
struct U128 {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    bool operator==(const U128& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const U128& o) const { return !(*this == o); }
    // hi 为主序的字典序：与 radix_sort.h 的字节处理顺序保持一致
    bool operator<(const U128& o) const {
        return hi != o.hi ? hi < o.hi : lo < o.lo;
    }

    // 分量相加：用于几何分组时把各数字格种子累加到邻格
    U128& operator+=(const U128& o) {
        lo += o.lo;
        hi += o.hi;
        return *this;
    }
};

// u128 -> size_t 的桶折叠：只用于哈希表定位桶；
// 键的完整相等性由探测时比较 U128 全量保证，折叠碰撞不产生错误结果。
struct U128Hash {
    std::size_t operator()(const U128& k) const noexcept {
        return static_cast<std::size_t>(k.lo ^ k.hi);
    }
};

// 流式 128 位混合哈希：两个不同种子的 splitmix 累加器并行推进。
// 对定宽值反复 mix()，最后 finalize() 得到 U128。
class U128Hasher {
public:
    U128Hasher() = default;

    explicit U128Hasher(std::uint64_t seed)
        : lo_(seed + kLoSeed), hi_(seed + kHiSeed) {}

    void mix(std::uint64_t v) {
        lo_ = splitmix64(lo_ + v);
        hi_ = splitmix64(hi_ + v + kMixOffset);
    }

    void mix(int v) {
        mix(static_cast<std::uint64_t>(v));
    }

    U128 finalize() const { return {lo_, hi_}; }

private:
    static constexpr std::uint64_t kLoSeed = 0x9e3779b97f4a7c15ULL;
    static constexpr std::uint64_t kHiSeed = 0xd1b54a32d192ed03ULL;
    static constexpr std::uint64_t kMixOffset = 0x6d2b79f5a39c8b7dULL;

    std::uint64_t lo_ = kLoSeed;
    std::uint64_t hi_ = kHiSeed;
};

}  // namespace mss
