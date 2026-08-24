#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace mss {

// 默认哈希：splitmix64 风格 64 位混合（无符号整数专用）
struct SplitMix64Hash {
    std::size_t operator()(std::uint64_t x) const noexcept {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }
};

// 开放寻址、线性探测的扁平哈希表：只支持插入/查找（不支持删除）。
// 键值连续存储、缓存友好；容量恒为 2 的幂，负载因子不超过 kMaxLoadFactor，
// 平均探测次数很小，比 std::unordered_map 的节点式布局快得多。
// clear() 保留 capacity，适合"整表清空后复用"的场景。
template <typename Key, typename Value, typename Hash = SplitMix64Hash>
class FlatHashTable {
public:
    static constexpr double kMaxLoadFactor = 0.5;

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    void clear() {
        used_.assign(used_.size(), 0);  // 保留 capacity，只清占位标记
        size_ = 0;
    }

    // 预留容量，使负载因子不超过 kMaxLoadFactor
    void reserve(std::size_t expected) {
        const std::size_t need = nextPowerOfTwo(expected > 0 ? expected * 2 : 1);
        if (need > capacity()) rehash(need);
    }

    // 查找：命中返回指向值的指针，未命中返回 nullptr
    Value* find(const Key& key) {
        if (size_ == 0) return nullptr;
        const std::size_t mask = capacity() - 1;
        std::size_t i = hash_(key) & mask;
        for (;;) {
            if (!used_[i]) return nullptr;
            if (keys_[i] == key) return &values_[i];
            i = (i + 1) & mask;
        }
    }

    const Value* find(const Key& key) const {
        if (size_ == 0) return nullptr;
        const std::size_t mask = capacity() - 1;
        std::size_t i = hash_(key) & mask;
        for (;;) {
            if (!used_[i]) return nullptr;
            if (keys_[i] == key) return &values_[i];
            i = (i + 1) & mask;
        }
    }

    // 查找，不存在则插入默认值并返回引用（与 std::unordered_map::operator[] 一致）
    Value& operator[](const Key& key) {
        if (size_ + 1 > capacity() * kMaxLoadFactor) grow();
        const std::size_t mask = capacity() - 1;
        std::size_t i = hash_(key) & mask;
        for (;;) {
            if (!used_[i]) {
                used_[i] = 1;
                keys_[i] = key;
                values_[i] = Value{};
                ++size_;
                return values_[i];
            }
            if (keys_[i] == key) return values_[i];
            i = (i + 1) & mask;
        }
    }

    // 仅当键不存在时插入（与 std::unordered_map::emplace 一致）
    void emplace(const Key& key, const Value& value) {
        if (size_ + 1 > capacity() * kMaxLoadFactor) grow();
        const std::size_t mask = capacity() - 1;
        std::size_t i = hash_(key) & mask;
        for (;;) {
            if (!used_[i]) {
                used_[i] = 1;
                keys_[i] = key;
                values_[i] = value;
                ++size_;
                return;
            }
            if (keys_[i] == key) return;
            i = (i + 1) & mask;
        }
    }

private:
    std::size_t capacity() const { return keys_.size(); }

    static std::size_t nextPowerOfTwo(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    void grow() { rehash(capacity() > 0 ? capacity() * 2 : 4); }

    void rehash(std::size_t newCap) {
        std::vector<Key> newKeys(newCap);
        std::vector<Value> newValues(newCap);
        std::vector<unsigned char> newUsed(newCap, 0);
        const std::size_t mask = newCap - 1;
        for (std::size_t i = 0; i < capacity(); ++i) {
            if (!used_[i]) continue;
            std::size_t j = hash_(keys_[i]) & mask;
            while (newUsed[j]) j = (j + 1) & mask;
            newUsed[j] = 1;
            newKeys[j] = keys_[i];
            newValues[j] = values_[i];
        }
        keys_ = std::move(newKeys);
        values_ = std::move(newValues);
        used_ = std::move(newUsed);
    }

    Hash hash_;
    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<unsigned char> used_;
    std::size_t size_ = 0;
};

}  // namespace mss
