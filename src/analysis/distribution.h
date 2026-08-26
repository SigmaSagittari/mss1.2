#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// distribution.h — 连通块的计数与枚举。
//
// 只做一件事：把 shape 变成分布表，然后喂给概率引擎。
//   - forEachAssignment  深分配 DFS：枚举 shape 内 box 雷数组合。
//   - analyze            汇总分布表（组合 → ways / 每 box 期望），池缓存。
//   - all_distribute     枚举具体摆放方案（深分配 × 每 box 选格 × T 格补足）。
//
// 派生统计（mu/sigma2/logWays）、多项式、精确/近似概率、网格物化
// 全部不归这里——那些是概率引擎拿本层产出去算的下游。
// ─────────────────────────────────────────────────────────────

struct Distribution {
    // ── 数据类 ──

    // 单种雷数的分布。
    struct Entry {
        int mineCount = 0;                           // 连通块总雷数
        long double ways = 0;                        // 摆放方案数
        std::vector<long double> perBoxExpectation;  // 每单位格期望雷数（BoxId 局部）
    };

    std::vector<Entry> entries;

    // ── 池 ──

    // 分布池：const Shape* → const Distribution*，只增不删。
    // 同一 interned Shape 必然同一分布；指针键复用 shape 身份，不重复哈希。
    struct DistPool {
        const Distribution* get(const Structure::Shape* shape);
        const Distribution* get(const Structure::Shape* shape) const;
        const Distribution* insert(const Structure::Shape* shape, Distribution dist);

    private:
        struct Hash {
            std::size_t operator()(const Structure::Shape* p) const noexcept {
                return SplitMix64Hash{}(reinterpret_cast<std::uintptr_t>(p));
            }
        };
        std::vector<std::unique_ptr<Distribution>> dists_;  // 稳定地址，只增不删
        FlatHashTable<const Structure::Shape*, const Distribution*, Hash> index_;
    };

    // ── 算法类（纯空壳）──

    struct Solver {
        // 深分配 DFS：枚举 shape 内所有满足约束的 box 雷数组合。
        // 对每个分配调用 onAssignment(assignment, ways)：
        //   assignment 按 BoxId 索引（各单位格雷数），只读、不可持引用；
        //   ways = 该分配的组合数 ∏ C(boxSize, k)。
        template <typename OnAssignment>
        static void forEachAssignment(const Structure::Shape& shape, OnAssignment&& onAssignment);

        // 汇总分布表（forEachAssignment 聚合），经 DistPool 去重缓存。
        static const Distribution* analyze(const Structure::Shape& shape, DistPool& pool);

        // 枚举当前盘面全部摆雷方案。
        // callback(placed)：placed 为按 CellId 排列的雷格列表（不含 basic 确定 Mine）。
        // 内部：每组件深分配 → 每 box 选具体格子 → Unknown(T 格) 组合补足。
        // 前置：调用方须先预判方案数 ≤ kMaxBruteforceCount。
        template <typename Callback>
        static void all_distribute(const ObservedBoard& board, const Basic::Result& basic,
                                   const Structure::Result& structure, Callback&& callback);

    private:
        // 组合数 C(n,k)：编译时查表。box 规模 ≤ 8（同一数字邻域集合的隐藏格
        // 都在该组任一数字的 8 邻域内）。
        static long double binom(int n, int k);
    };
};

// ── 实现区 ──

inline const Distribution* Distribution::DistPool::get(const Structure::Shape* shape) {
    if (const Distribution** found = index_.find(shape)) return *found;
    return nullptr;
}

inline const Distribution* Distribution::DistPool::get(const Structure::Shape* shape) const {
    if (const Distribution* const* found = index_.find(shape)) return *found;
    return nullptr;
}

inline const Distribution* Distribution::DistPool::insert(const Structure::Shape* shape,
                                                          Distribution dist) {
    if (const Distribution* found = get(shape)) return found;
    dists_.push_back(std::make_unique<Distribution>(std::move(dist)));
    index_.emplace(shape, dists_.back().get());
    return dists_.back().get();
}

// ── Solver 私有工具实现 ──

inline long double Distribution::Solver::binom(int n, int k) {
    constexpr int kMax = 9;
    // 静态 constexpr 局部表，只在此函数内可见。
    static constexpr std::array<std::array<long double, kMax + 1>, kMax + 1> kComb = []() {
        std::array<std::array<long double, kMax + 1>, kMax + 1> t{};
        for (int n = 0; n <= kMax; ++n) {
            t[static_cast<std::size_t>(n)][0] = 1;
            t[static_cast<std::size_t>(n)][static_cast<std::size_t>(n)] = 1;
            for (int k = 1; k < n; ++k)
                t[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)] =
                    t[static_cast<std::size_t>(n - 1)][static_cast<std::size_t>(k - 1)] +
                    t[static_cast<std::size_t>(n - 1)][static_cast<std::size_t>(k)];
        }
        return t;
    }();

    // 调用方（forEachAssignment）保证 0<=k<=n<=box.size<=kMax；越界=结构 bug。
    assert_(k >= 0 && k <= n && n <= kMax, "Distribution::Solver::binom: 参数越界");
    return kComb[static_cast<std::size_t>(n)][static_cast<std::size_t>(k)];
}

template <typename OnAssignment>
inline void Distribution::Solver::forEachAssignment(const Structure::Shape& shape,
                                                    OnAssignment&& onAssignment) {
    const int n = static_cast<int>(shape.boxes.size());
    const int nc = static_cast<int>(shape.constraints.size());

    // 线程局部复用工作区（无重入）：避免每次分析的嵌套 vector 分配。
    static thread_local std::vector<std::vector<int>> tlBoxLimits;
    static thread_local std::vector<int> tlConsSum;
    static thread_local std::vector<int> tlConsMaxAdd;
    static thread_local std::vector<int> tlCurSum;
    static thread_local std::vector<int> tlSizeSum;
    static thread_local std::vector<char> tlAssignment;
    tlBoxLimits.assign(static_cast<std::size_t>(n), {});
    tlConsSum.assign(static_cast<std::size_t>(nc), 0);
    tlConsMaxAdd.assign(static_cast<std::size_t>(nc), 0);
    tlCurSum.assign(static_cast<std::size_t>(nc), 0);
    tlSizeSum.assign(static_cast<std::size_t>(nc), 0);
    tlAssignment.assign(static_cast<std::size_t>(n), 0);

    for (int i = 0; i < nc; ++i) {
        tlConsSum[static_cast<std::size_t>(i)] = shape.constraints[static_cast<std::size_t>(i)].sum;
        for (BoxId boxId : shape.constraints[static_cast<std::size_t>(i)].boxIds) {
            tlConsMaxAdd[static_cast<std::size_t>(i)] +=
                shape.boxes[static_cast<std::size_t>(boxId)].size;
            tlBoxLimits[static_cast<std::size_t>(boxId)].push_back(i);
        }
    }

    // 增量约束剪枝：每个 box 赋值后立即检查其所属约束。
    //   curSum[c] + k > sum[c]         → 超了，剪
    //   curSum[c] + k + rem < sum[c]    → 剩余 box 全摆满也凑不够，剪
    //   （rem = 约束 c 未赋 box 的 size 之和）
    auto dfs = [&](auto&& self, int idx, long double curWays) -> void {
        if (idx == n) {
            onAssignment(tlAssignment, curWays);
            return;
        }
        const int maxK = shape.boxes[static_cast<std::size_t>(idx)].size;
        for (int k = 0; k <= maxK; ++k) {
            tlAssignment[static_cast<std::size_t>(idx)] = static_cast<char>(k);
            bool ok = true;
            for (int c : tlBoxLimits[static_cast<std::size_t>(idx)]) {
                const int s = tlCurSum[static_cast<std::size_t>(c)] + k;
                // rem = 约束 c 未赋 box（含当前 idx）的 size 之和
                const int rem = tlConsMaxAdd[static_cast<std::size_t>(c)] -
                                (tlSizeSum[static_cast<std::size_t>(c)] + maxK);
                if (s > tlConsSum[static_cast<std::size_t>(c)] ||
                    s + rem < tlConsSum[static_cast<std::size_t>(c)]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                for (int c : tlBoxLimits[static_cast<std::size_t>(idx)]) {
                    tlCurSum[static_cast<std::size_t>(c)] += k;
                    tlSizeSum[static_cast<std::size_t>(c)] += maxK;
                }
                self(self, idx + 1, curWays * binom(maxK, k));
                for (int c : tlBoxLimits[static_cast<std::size_t>(idx)]) {
                    tlCurSum[static_cast<std::size_t>(c)] -= k;   // 回溯
                    tlSizeSum[static_cast<std::size_t>(c)] -= maxK;
                }
            }
        }
    };
    dfs(dfs, 0, 1.0);
}

inline const Distribution* Distribution::Solver::analyze(const Structure::Shape& shape,
                                                         DistPool& pool) {
    if (const Distribution* cached = pool.get(&shape)) return cached;

    const int n = static_cast<int>(shape.boxes.size());
    int maxTotal = 0;
    for (const auto& box : shape.boxes) maxTotal += box.size;

    // 线程局部复用工作区（analyze 非重入、无并发）：避免每块新建嵌套 vector。
    static thread_local std::vector<long double> wayTable;
    static thread_local std::vector<long double> expectFlat;
    wayTable.assign(static_cast<std::size_t>(maxTotal + 1), 0.0L);
    expectFlat.assign(static_cast<std::size_t>(maxTotal + 1) * static_cast<std::size_t>(n),
                      0.0L);

    forEachAssignment(shape, [&](const std::vector<char>& assignment, long double ways) {
        int total = 0;
        for (int i = 0; i < n; ++i) total += assignment[static_cast<std::size_t>(i)];
        wayTable[static_cast<std::size_t>(total)] += ways;
        const std::size_t row = static_cast<std::size_t>(total) * static_cast<std::size_t>(n);
        for (int i = 0; i < n; ++i)
            expectFlat[row + static_cast<std::size_t>(i)] +=
                ways * assignment[static_cast<std::size_t>(i)];
    });

    Distribution dist;
    for (int total = 0; total <= maxTotal; ++total) {
        if (wayTable[static_cast<std::size_t>(total)] == 0) continue;
        Entry e;
        e.mineCount = total;
        e.ways = wayTable[static_cast<std::size_t>(total)];
        const std::size_t row = static_cast<std::size_t>(total) * static_cast<std::size_t>(n);
        e.perBoxExpectation.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            e.perBoxExpectation[static_cast<std::size_t>(i)] =
                expectFlat[row + static_cast<std::size_t>(i)] / e.ways;
        dist.entries.push_back(std::move(e));
    }

    return pool.insert(&shape, std::move(dist));
}

template <typename Callback>
inline void Distribution::Solver::all_distribute(const ObservedBoard& board,
                                                 const Basic::Result& basic,
                                                 const Structure::Result& structure,
                                                 Callback&& callback) {
    using Mark = Basic::Mark;

    // 剩余可摆放的雷数 = 总雷数 - basic 已确定的 Mine。
    const int mines = board.totalMines - basic.mineSum;

    // T 格：Unknown 格，最后在这里补足剩余雷数。
    std::vector<CellId> tcells;
    for (int i = 1; i <= board.rows; ++i)
        for (int j = 1; j <= board.cols; ++j)
            if (basic.marks[i][j] == Mark::Unknown)
                tcells.push_back(board.id(i, j));

    // 当前已摆出的雷（不含 basic 已确定的 Mine），叶子节点时交给 callback。
    std::vector<CellId> placed;
    placed.reserve(static_cast<std::size_t>(board.totalMines));

    // 从 positions[start..] 里选 need 个位置，选中时压入 placed，选完调 on_complete。
    auto choose = [&](auto&& self, const std::vector<CellId>& positions, int start, int need,
                      auto&& on_complete) -> void {
        if (need == 0) {
            on_complete();
            return;
        }
        const int n = static_cast<int>(positions.size());
        for (int i = start; i <= n - need; ++i) {
            placed.push_back(positions[static_cast<std::size_t>(i)]);
            self(self, positions, i + 1, need - 1, on_complete);
            placed.pop_back();
        }
    };

    // 组件索引。
    std::vector<ComponentId> comps;
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size()); ++cid)
        comps.push_back(cid);

    const int compCount = static_cast<int>(comps.size());

    // 后缀最大雷数，用于剪枝：当前已用 + 后面所有连通块全摆满仍不够剩余雷数时提前返回。
    std::vector<int> suffixMax(static_cast<std::size_t>(compCount + 1), 0);
    for (int ci = compCount - 1; ci >= 0; --ci) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(comps[static_cast<std::size_t>(ci)])];
        int mx = 0;
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            mx += static_cast<int>(inst.boxes.cellCount(b));
        suffixMax[static_cast<std::size_t>(ci)] =
            suffixMax[static_cast<std::size_t>(ci + 1)] + mx;
    }

    // 按连通块编号预计算深分配（各单位格雷数），只 DFS 一次。
    std::vector<std::vector<std::vector<char>>> deepCache(
        static_cast<std::size_t>(compCount));
    for (int ci = 0; ci < compCount; ++ci) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(comps[static_cast<std::size_t>(ci)])];
        forEachAssignment(*inst.shape, [&](const std::vector<char>& assignment, long double) {
            deepCache[static_cast<std::size_t>(ci)].push_back(assignment);
        });
    }

    auto dfsComponents = [&](auto&& self, int ci, int used) -> void {
        if (used > mines) return;
        if (ci == compCount) {
            // 所有连通块处理完毕，剩余雷数在 T 格里补足。
            const int left = mines - used;
            if (left < 0 || left > static_cast<int>(tcells.size())) return;
            if (left == 0) {
                callback(placed);
                return;
            }
            choose(choose, tcells, 0, left, [&] { callback(placed); });
            return;
        }
        // 后面所有连通块 + 全部 T 格都摆满雷仍不够剩余雷数，则此分支无解。
        if (used + suffixMax[static_cast<std::size_t>(ci)] +
                static_cast<int>(tcells.size()) < mines)
            return;

        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(comps[static_cast<std::size_t>(ci)])];
        const int n = static_cast<int>(inst.boxes.count());
        const auto& deep = deepCache[static_cast<std::size_t>(ci)];

        for (const auto& assignment : deep) {
            int compMines = 0;
            for (char c : assignment) compMines += c;
            auto dfsBox = [&](auto&& selfBox, int boxi) -> void {
                if (boxi == n) {
                    self(self, ci + 1, used + compMines);
                    return;
                }
                // 取第 boxi 个 box 的格子区间。
                std::vector<CellId> boxCells;
                boxCells.reserve(inst.boxes.cellCount(static_cast<std::size_t>(boxi)));
                for (std::size_t k = inst.boxes.boxOf[static_cast<std::size_t>(boxi)];
                     k < inst.boxes.boxOf[static_cast<std::size_t>(boxi + 1)]; ++k)
                    boxCells.push_back(inst.boxes.cells[k]);
                choose(choose, boxCells, 0, assignment[static_cast<std::size_t>(boxi)],
                       [&] { selfBox(selfBox, boxi + 1); });
            };
            dfsBox(dfsBox, 0);
        }
    };
    dfsComponents(dfsComponents, 0, 0);
}

}  // namespace mss