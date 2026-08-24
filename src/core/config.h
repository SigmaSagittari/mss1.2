#pragma once

namespace mss {

// 全局配置：集中放置跨层共享的魔法数字。
// 各层内部专用的阈值（如搜索 eps、近似参数）放各自层，不堆在这里。

// 暴力枚举的最大方案数上限。
// all_distribute / 残局求解会在枚举前预估候选方案数，超过该值时
// 直接放弃枚举（否则指数级枚举把程序卡死）。需要放开限制时改这个数。
inline constexpr int kMaxBruteforceCount = 200000;

}  // namespace mss