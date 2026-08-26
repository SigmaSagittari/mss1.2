#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/config.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"
#include "core/utility/radix_sort.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// bruteforce/endgame_bruteforce.h — 残局精确求解（暴力枚举）。
//
// 枚举全部满足约束的摆雷方案，递归求解"点哪个格、按观测分组继续、
// 能保证赢下多少条方案"。可输出每个候选初始招法的精确胜数。
//
// 依赖 distribution.h 的 all_distribute（枚举摆雷）+ 调用方传入的
// 单格雷概率网格（Exact 物化）。precheck 用
// Exact::analyze 现算候选数，超过 kMaxBruteforceCount 直接 assert_。
//
// 数学/数据结构细节（Session、ScratchBuffers、solve 递归）全私有。
// ─────────────────────────────────────────────────────────────

struct EndgameBruteforce {
    // 求解输入配置。
    struct Config {
        bool checkAllMoves;
        Config() : checkAllMoves(false) {}  // true：输出每个候选格精确胜数；false：只给最优第一步
    };

    // 一次求解的产物。
    struct Result {
        struct Winrate {
            int x = 0;  // 1-based 行
            int y = 0;  // 1-based 列
            int wins = 0;
        };

        int totalPossibilities = 0;  // 方案总数
        long long nodes = 0;         // DFS 节点数
        std::vector<Winrate> result; // checkAllMoves=false 时仅 1 项；true 时每候选格一项
    };

    // 求解一次残局。
    // probability：单格雷概率网格（1-based，与棋盘一致）。0 概率隐藏格也作
    //   候选格参与求解（warn 一次）；概率必须在 [0,1]。
    // minWins：至少要赢下多少条才有意义（仅 checkAllMoves=false 生效）。
    static Result solveEndgame(const ObservedBoard& board, const Basic::Result& basic,
                               const Structure::Result& structure,
                               Distribution::DistPool& pool,
                               const Grid<long double>& probability,
                               const Config& config = Config{}, int minWins = 1);

private:
    // 候选格：点开本格看到的数字 = 相邻待解格的雷数之和（links）。
    struct Candidate {
        int x = 0;
        int y = 0;
        std::vector<int> links;
    };

    // 安全分支分组条目：键序 hash.hi → hash.lo → p（配合基数排序字段序）。
    struct SafeEntry {
        U128 hash;
        std::uint32_t p = 0;
    };

    // 一次求解的全部预处理状态（候选格、方案矩阵、reveal 预计算）。
    struct Session {
        std::vector<Candidate> candidates;
        std::vector<std::vector<char>> mineConfigs;   // 方案 × 待解格：1 = 雷
        std::vector<int> reveal;                      // reveal[p*m+j]：方案 p 点开 j 的数字
        std::vector<std::vector<int>> mineColsPerRow; // 方案 p 中为雷的待解格编号
        std::vector<char> opened;                     // 递归中的"已点开"标记
        long long nodes = 0;
    };

    // 递归逐层暂存池（deque：扩容时已有层引用不失效）。
    class ScratchBuffers {
    public:
        struct Layer {
            std::vector<int> deaths;
            std::vector<int> safeCells;
            std::vector<int> order;
            std::vector<int> suffix;
            std::array<std::vector<int>, 9> groups;      // 风险分支：观测值 0..8
            std::vector<std::pair<int, int>> groupList;  // (观测值, 组大小)
            std::vector<SafeEntry> safeEntries;
            std::vector<SafeEntry> safeEntriesTmp;       // 基数排序输出缓冲
            std::vector<std::vector<int>> safeGroups;    // 安全分支组池
            std::vector<const std::vector<int>*> safeGroupList;
        };

        Layer& layer(int depth) {
            if (static_cast<int>(layers_.size()) <= depth) layers_.emplace_back();
            return layers_[static_cast<std::size_t>(depth)];
        }

        void reset() {
            for (auto& l : layers_) {  // 只清内容，capacity 跨求解复用
                l.deaths.clear();
                l.safeCells.clear();
                l.order.clear();
                l.suffix.clear();
                for (auto& g : l.groups) g.clear();
                l.groupList.clear();
                l.safeEntries.clear();
                l.safeEntriesTmp.clear();
                for (auto& g : l.safeGroups) g.clear();
                l.safeGroupList.clear();
            }
        }

    private:
        std::deque<Layer> layers_;
    };

    // 构造 Session：候选格 + 方案矩阵 + reveal/mineColsPerRow 预计算。
    // 进入枚举前先算精确候选数（Exact::analyze），超过 kMaxBruteforceCount 直接 assert_。
    static Session buildCandidates(const ObservedBoard& board, const Basic::Result& basic,
                                   const Structure::Result& structure,
                                   Distribution::DistPool& pool,
                                   const Grid<long double>& probability);

    // 方案 possibility 点开待解格 cell 后看到的数字。
    static int revealSum(const Session& s, int possibility, int cell);

    // configs（有序方案行号列表）的 128 位指纹。
    static U128 hashConfigs(const std::vector<int>& configs);

    // 递归求解。返回真实值 ≥ need 时为精确赢数；< need 时返回 0（不写缓存）。
    template <bool CheckAllMoves, bool IsRoot>
    static int solve(Session& s, ScratchBuffers& scratch, const std::vector<int>& configs,
                     int need, int depth, FlatHashTable<U128, int, U128Hash>& cache,
                     std::vector<Result::Winrate>& out);
};

// ── 实现区 ──

inline EndgameBruteforce::Result EndgameBruteforce::solveEndgame(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, Distribution::DistPool& pool,
    const Grid<long double>& probability, const Config& config, int minWins) {
    thread_local static FlatHashTable<U128, int, U128Hash> cache;
    thread_local static ScratchBuffers scratch;

    Session s = buildCandidates(board, basic, structure, pool, probability);
    s.nodes = 0;
    s.opened.assign(s.candidates.size(), 0);

    std::vector<int> allConfigs(static_cast<std::size_t>(s.mineConfigs.size()));
    for (int i = 0; i < static_cast<int>(allConfigs.size()); ++i) allConfigs[i] = i;

    scratch.reset();

    Result result;
    result.totalPossibilities = static_cast<int>(s.mineConfigs.size());

    if (config.checkAllMoves) {
        solve<true, true>(s, scratch, allConfigs, 1, 0, cache, result.result);
    } else {
        result.result.resize(1);
        if (!s.candidates.empty()) {
            result.result[0].x = s.candidates[0].x;
            result.result[0].y = s.candidates[0].y;
        }
        const int wins =
            solve<false, true>(s, scratch, allConfigs, minWins, 0, cache, result.result);
        if (wins >= minWins) result.result[0].wins = wins;
    }

    result.nodes = s.nodes;
    cache.clear();
    return result;
}

inline EndgameBruteforce::Session EndgameBruteforce::buildCandidates(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, Distribution::DistPool& pool,
    const Grid<long double>& probability) {
    Session s;

    std::vector<int> posToIndex(
        static_cast<std::size_t>(board.rows + 1) * static_cast<std::size_t>(board.cols + 1), -1);
    bool warnedZero = false;
    for (int i = 1; i <= board.rows; ++i)
        for (int j = 1; j <= board.cols; ++j) {
            if (board.board[i][j] != Cell::Hidden) continue;
            const long double prob = probability[i][j];
            if (prob == 0.0L && !warnedZero) {
                warn_("Hidden cell has zero probability; including it as a candidate");
                warnedZero = true;
            }
            if (prob < 1.0L) {
                posToIndex[static_cast<std::size_t>(board.id(i, j))] =
                    static_cast<int>(s.candidates.size());
                s.candidates.push_back({i, j, {}});
            }
        }

    for (int i = 1; i <= board.rows; ++i)
        for (int j = 1; j <= board.cols; ++j) {
            const int idx = posToIndex[static_cast<std::size_t>(board.id(i, j))];
            if (idx < 0) continue;
            auto& links = s.candidates[static_cast<std::size_t>(idx)].links;
            forEachAdjacent(i, j, board.rows, board.cols, [&](int nx, int ny) {
                const int nid = posToIndex[static_cast<std::size_t>(board.id(nx, ny))];
                if (nid >= 0) links.push_back(nid);
            });
        }

    // precheck：精确候选数超过上限就不枚举（否则指数级枚举把程序卡死）。
    {
        const long double candidates = Exact::analyze(board, basic, structure, pool).candidates;
        assert_(candidates <= static_cast<long double>(kMaxBruteforceCount),
                "EndgameBruteforce: 候选方案数超过 kMaxBruteforceCount");
    }

    Distribution::Solver::all_distribute(
        board, basic, structure, [&](const std::vector<CellId>& mines) {
            std::vector<char> row(s.candidates.size(), 0);
            for (CellId cell : mines) {
                const int idx = posToIndex[static_cast<std::size_t>(cell)];
                if (idx < 0) continue;
                row[static_cast<std::size_t>(idx)] = 1;
            }
            s.mineConfigs.push_back(std::move(row));
        });

    const int m = static_cast<int>(s.candidates.size());
    s.reveal.assign(s.mineConfigs.size() * static_cast<std::size_t>(m), 0);
    s.mineColsPerRow.assign(s.mineConfigs.size(), {});
    for (std::size_t ci = 0; ci < s.mineConfigs.size(); ++ci)
        for (int j = 0; j < m; ++j) {
            int sum = 0;
            for (int link : s.candidates[static_cast<std::size_t>(j)].links)
                sum += s.mineConfigs[ci][static_cast<std::size_t>(link)];
            s.reveal[ci * static_cast<std::size_t>(m) + static_cast<std::size_t>(j)] = sum;
            if (s.mineConfigs[ci][static_cast<std::size_t>(j)])
                s.mineColsPerRow[ci].push_back(j);
        }

    return s;
}

inline int EndgameBruteforce::revealSum(const Session& s, int possibility, int cell) {
    return s.reveal[static_cast<std::size_t>(possibility) * s.candidates.size() +
                    static_cast<std::size_t>(cell)];
}

inline U128 EndgameBruteforce::hashConfigs(const std::vector<int>& configs) {
    U128Hasher h;
    const std::uint64_t n = static_cast<std::uint64_t>(configs.size());
    for (std::uint64_t i = 0; i < n; ++i)
        h.mix(static_cast<std::uint64_t>(configs[static_cast<std::size_t>(i)]) * (n + 1) + i);
    return h.finalize();
}

// 递归求解当前集合 configs（有序的方案行号列表，s.opened 为当前已点开格）。
// need：本组至少要赢下多少条才对上层有意义；depth：递归深度，对应第 depth 层暂存。
// 返回：真实值 ≥ need 时为精确赢数；真实值 < need 时返回 0（不写缓存）。
//       0 只表示"真实值 < need"，不表示真实值为 0。
// CheckAllMoves=true 仅用于根层：对每个初始招法独立计算精确胜数并全部写入 out。
// IsRoot 编译期标记当前节点是否为首层；只有首层实例化才会写入 out。
template <bool CheckAllMoves, bool IsRoot>
inline int EndgameBruteforce::solve(Session& s, ScratchBuffers& scratch,
                                    const std::vector<int>& configs, int need, int depth,
                                    FlatHashTable<U128, int, U128Hash>& cache,
                                    std::vector<Result::Winrate>& out) {
    if constexpr (CheckAllMoves && IsRoot) {
        ++s.nodes;
        const int n = static_cast<int>(configs.size());
        out.clear();
        if (need > n) return 0;

        ScratchBuffers::Layer& buf = scratch.layer(depth);
        const int m = static_cast<int>(s.candidates.size());

        auto& deaths = buf.deaths;
        deaths.assign(m, 0);
        for (int ci : configs) {
            const auto& mines = s.mineColsPerRow[static_cast<std::size_t>(ci)];
            for (int j : mines) ++deaths[j];
        }

        // 根层：逐个初始招法独立求解，不做 best 剪枝、不跳过单一观测值的招法
        int best = 0;
        auto& groups = buf.groups;
        for (int j = 0; j < m; ++j) {
            if (s.opened[static_cast<std::size_t>(j)]) continue;
            for (auto& g : groups) g.clear();
            for (int ci : configs) {
                if (s.mineConfigs[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)]) continue;
                groups[static_cast<std::size_t>(revealSum(s, ci, j))].push_back(ci);
            }

            s.opened[static_cast<std::size_t>(j)] = 1;
            int wins = 0;
            for (int r = 0; r < 9; ++r) {
                if (groups[static_cast<std::size_t>(r)].empty()) continue;
                const int v = solve<false, false>(s, scratch, groups[static_cast<std::size_t>(r)],
                                                  1, depth + 1, cache, out);
                wins += v;
            }
            s.opened[static_cast<std::size_t>(j)] = 0;

            out.push_back({s.candidates[static_cast<std::size_t>(j)].x,
                           s.candidates[static_cast<std::size_t>(j)].y, wins});
            best = (std::max)(best, wins);
        }
        return best;
    } else {
        (void)out;
        ++s.nodes;
        const int n = static_cast<int>(configs.size());
        if (need > n) return 0;
        if (n <= 1) {
            if constexpr (IsRoot) {
                if (n == 1 && !configs.empty()) {
                    const int ci = configs[0];
                    const int m = static_cast<int>(s.candidates.size());
                    for (int j = 0; j < m; ++j) {
                        if (!s.mineConfigs[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)]) {
                            out[0].x = s.candidates[static_cast<std::size_t>(j)].x;
                            out[0].y = s.candidates[static_cast<std::size_t>(j)].y;
                            break;
                        }
                    }
                }
            }
            return n;
        }

        const U128 cacheKey = hashConfigs(configs);
        const int* cached = cache.find(cacheKey);
        if (cached != nullptr) {
            const int exact = *cached;
            return exact >= need ? exact : 0;
        }

        ScratchBuffers::Layer& buf = scratch.layer(depth);
        const int m = static_cast<int>(s.candidates.size());

        auto& deaths = buf.deaths;
        deaths.assign(m, 0);
        for (int ci : configs) {
            const auto& mines = s.mineColsPerRow[static_cast<std::size_t>(ci)];
            for (int j : mines) ++deaths[j];
        }

        auto& safeCells = buf.safeCells;
        safeCells.clear();
        for (int j = 0; j < m; ++j)
            if (!s.opened[static_cast<std::size_t>(j)] && deaths[j] == 0) safeCells.push_back(j);

        if (!safeCells.empty()) {
            if constexpr (IsRoot) {
                out[0].x = s.candidates[static_cast<std::size_t>(safeCells[0])].x;
                out[0].y = s.candidates[static_cast<std::size_t>(safeCells[0])].y;
            }
            // 安全格不杀任何方案，是免费信息：全部同时点开，按观测向量分组
            for (int j : safeCells) s.opened[static_cast<std::size_t>(j)] = 1;
            auto& entries = buf.safeEntries;
            entries.clear();
            entries.reserve(configs.size());
            const std::size_t keyLen = safeCells.size();
            for (int ci : configs) {
                U128Hasher hasher;
                for (std::size_t i = 0; i < keyLen; ++i) {
                    const int v = revealSum(s, ci, safeCells[static_cast<std::size_t>(i)]);
                    hasher.mix(static_cast<std::uint64_t>(v) * (keyLen + 1) + i);
                }
                entries.push_back({hasher.finalize(), static_cast<std::uint32_t>(ci)});
            }
            radix_sort::sort(entries, buf.safeEntriesTmp,
                             [](const SafeEntry& e) { return e.hash.hi; },
                             [](const SafeEntry& e) { return e.hash.lo; },
                             [](const SafeEntry& e) { return e.p; });  // 同组内行号升序，solve 依赖有序
            auto& groups = buf.safeGroups;
            for (auto& g : groups) g.clear();
            std::size_t gcnt = 0;
            for (std::size_t i = 0; i < entries.size();) {
                std::size_t j = i + 1;
                while (j < entries.size() && entries[j].hash == entries[i].hash) ++j;
                if (gcnt == groups.size()) groups.emplace_back();
                auto& g = groups[gcnt++];
                g.reserve(j - i);
                for (std::size_t k = i; k < j; ++k)
                    g.push_back(static_cast<int>(entries[k].p));
                i = j;
            }
            // 指针在池扩容完成后统一收集，避免悬垂
            auto& groupList = buf.safeGroupList;
            groupList.clear();
            for (std::size_t gi = 0; gi < gcnt; ++gi)
                groupList.push_back(&groups[gi]);
            std::sort(groupList.begin(), groupList.end(),
                      [](const auto* a, const auto* b) { return a->size() > b->size(); });
            auto& suffix = buf.suffix;
            suffix.assign(groupList.size() + 1, 0);
            for (int i = static_cast<int>(groupList.size()) - 1; i >= 0; --i)
                suffix[i] = suffix[i + 1] + static_cast<int>(groupList[static_cast<std::size_t>(i)]->size());

            int result = 0;
            bool bailed = false;
            for (int i = 0; i < static_cast<int>(groupList.size()); ++i) {
                const int sizeG = static_cast<int>(groupList[static_cast<std::size_t>(i)]->size());
                if (result + sizeG + suffix[i + 1] < need) { bailed = true; break; }
                // 本组至少吃下 target - 已得 - 后面总和（保底 1），不足则整枝作废
                const int needG = (std::max)(1, need - result - suffix[i + 1]);
                const int v = solve<false, false>(s, scratch, *groupList[static_cast<std::size_t>(i)],
                                                  needG, depth + 1, cache, out);
                if (v == 0) { bailed = true; break; }
                result += v;
            }
            for (int j : safeCells) s.opened[static_cast<std::size_t>(j)] = 0;
            if (bailed) return 0;
            cache[cacheKey] = result;
            return result;
        }

        auto& order = buf.order;
        order.clear();
        for (int j = 0; j < m; ++j)
            if (!s.opened[static_cast<std::size_t>(j)]) order.push_back(j);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return deaths[static_cast<std::size_t>(a)] < deaths[static_cast<std::size_t>(b)]; });

        int best = 0;
        for (std::size_t idx = 0; idx < order.size(); ++idx) {
            const int j = order[idx];
            // 本招目标：压过已找到的最优（best+1），同时满足上层需求（need）
            const int target = (std::max)(best + 1, need);

            // 全活也到不了目标，后面致死数不减，直接停
            if (n - deaths[static_cast<std::size_t>(j)] < target) break;

            auto& groups = buf.groups;
            for (auto& g : groups) g.clear();
            int groupCount = 0;
            for (int ci : configs) {
                if (s.mineConfigs[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)]) continue;
                const int r = revealSum(s, ci, j);
                if (groups[static_cast<std::size_t>(r)].empty()) ++groupCount;
                groups[static_cast<std::size_t>(r)].push_back(ci);
            }

            // 只有一种观测值 = 无信息，跳过（可证不优于其他招法）
            if (groupCount <= 1) continue;

            auto& groupList = buf.groupList;
            groupList.clear();
            for (int r = 0; r < 9; ++r)
                if (!groups[static_cast<std::size_t>(r)].empty())
                    groupList.push_back({r, static_cast<int>(groups[static_cast<std::size_t>(r)].size())});
            std::sort(groupList.begin(), groupList.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            auto& suffix = buf.suffix;
            suffix.assign(groupList.size() + 1, 0);
            for (int i = static_cast<int>(groupList.size()) - 1; i >= 0; --i)
                suffix[i] = suffix[i + 1] + groupList[static_cast<std::size_t>(i)].second;

            s.opened[static_cast<std::size_t>(j)] = 1;
            int wins = 0;
            bool bailed = false;
            for (int i = 0; i < static_cast<int>(groupList.size()); ++i) {
                const auto& group = groups[static_cast<std::size_t>(groupList[static_cast<std::size_t>(i)].first)];
                const int sizeG = groupList[static_cast<std::size_t>(i)].second;
                if (wins + sizeG + suffix[i + 1] < target) { bailed = true; break; }
                const int needG = (std::max)(1, target - wins - suffix[i + 1]);
                const int v = solve<false, false>(s, scratch, group, needG, depth + 1, cache, out);
                if (v == 0) { bailed = true; break; }
                wins += v;
            }
            s.opened[static_cast<std::size_t>(j)] = 0;
            if (bailed) continue;

            if (wins > best) {
                best = wins;
                if constexpr (IsRoot) {
                    out[0].x = s.candidates[static_cast<std::size_t>(j)].x;
                    out[0].y = s.candidates[static_cast<std::size_t>(j)].y;
                }
            }
        }

        if (best >= need) {
            cache[cacheKey] = best;
            return best;
        }
        // 跟随任意一条固定方案点开即可至少赢下它
        if (best == 0 && need <= 1) {
            if constexpr (IsRoot) {
                if (!configs.empty()) {
                    const int ci = configs[0];
                    for (int j = 0; j < m; ++j) {
                        if (!s.mineConfigs[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)]) {
                            out[0].x = s.candidates[static_cast<std::size_t>(j)].x;
                            out[0].y = s.candidates[static_cast<std::size_t>(j)].y;
                            break;
                        }
                    }
                }
            }
            return 1;
        }
        return 0;
    }
}

}  // namespace mss