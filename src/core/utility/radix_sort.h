#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mss {

// 通用 LSD 基数排序：语义命名空间（scope struct），不持有任何状态。
// 排序键由调用方以"无符号整数字段访问器"给出，主序在前；
// 排序器按字段本身的大小一把梭扫字节，不构造任何中间关键码。
// 残局搜索用它对"观测向量哈希"分组，entry 小、量大，比 std::sort 快。
struct radix_sort {
    // 条目数小于该值时用比较排序，省掉基数排序的固定开销
    static constexpr std::uint32_t kThreshold = 256;

    // 按访问器给出的字段升序稳定排序。tmp 是输出缓冲，由调用方持有、复用。
    template <typename Entry, typename... Accessor>
    static void sort(std::vector<Entry>& a, std::vector<Entry>& tmp, Accessor... accessor) {
        const std::uint32_t n = static_cast<std::uint32_t>(a.size());
        if (n <= kThreshold) {  // 小数组：比较排序更快
            std::sort(a.begin(), a.end(), [&](const Entry& x, const Entry& y) {
                return std::make_tuple(accessor(x)...) < std::make_tuple(accessor(y)...);
            });
            return;
        }

        tmp.resize(a.size());
        static thread_local std::array<std::uint32_t, 256> bucket;
        Entry* src = a.data();
        Entry* dst = tmp.data();

        // 按某个字段的某个字节做一趟稳定的计数排序：计数 -> 前缀起始下标 -> 正序散射
        auto passByte = [&](auto get, unsigned int bi) {
            for (auto& b : bucket) b = 0;
            for (std::uint32_t i = 0; i < n; ++i)
                ++bucket[static_cast<unsigned char>(get(src[i]) >> (8 * bi))];
            std::uint32_t sum = 0;
            for (auto& b : bucket) {
                const std::uint32_t v = b;
                b = sum;
                sum += v;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                const unsigned char byte = static_cast<unsigned char>(get(src[i]) >> (8 * bi));
                dst[bucket[byte]++] = src[i];
            }
            std::swap(src, dst);
        };

        // 一个字段的全部字节：低字节先排
        auto passField = [&](auto get) {
            using V = std::remove_reference_t<decltype(get(std::declval<Entry&>()))>;
            static_assert(std::is_unsigned_v<V>, "radix_sort 的排序字段必须是无符号整型");
            for (unsigned int bi = 0; bi < sizeof(V); ++bi) passByte(get, bi);
        };

        // LSD：主序字段最后排 —— 反序遍历访问器（最不重要的先排）
        auto gets = std::tuple(accessor...);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            constexpr std::size_t N = sizeof...(Accessor);
            (passField(std::get<N - 1 - I>(gets)), ...);
        }(std::make_index_sequence<sizeof...(Accessor)>{});

        // 总趟数奇偶不预设：结果不在 a 里就整体换回
        if (src != a.data()) a.swap(tmp);
    }
};

}  // namespace mss