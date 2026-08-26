#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// exact.h — 精确概率引擎。
//
// 把每个连通块的分布写成生成函数（Polynomial），乘起来得到所有连通块的
// 联合分布，再用组合数把"非前沿格"的雷数纳入，得到每个格子的精确雷概率。
// 无近似、无 rho；是残局/根节点的无偏参考实现。
//
// 内部细节（Polynomial、阶乘表、binom）全部为 Exact 私有，不暴露。
// ─────────────────────────────────────────────────────────────

struct Exact {
    // 精确分析：吃活组件 + 分布池，输出可查询的 Result。
    static Probability::Result analyze(const ObservedBoard& board,
                                       const Basic::Result& basic,
                                       const Structure::Result& structure,
                                       Distribution::DistPool& pool);

    // 观察：点开格子 cell 的结果分布（爆炸概率 + 数字 0..8 概率）。
    // explosion = prob.mineProbability(cell)（P(x 是雷)，observe 不再自算）；
    // 已揭示/数字格 → 全 0。
    // 算法：对每活连通块 forEachAssignment 枚举 box 雷数分配，每分配对
    // "邻域雷数贡献"做 box 超几何卷积 → 二维块贡献 (h, r)，跨块卷积后
    // 按 T 格组合补足 → digit[0..8]。
    static Probability::ObserveResult observe(const ObservedBoard& board,
                                              const Basic::Result& basic,
                                              const Structure::Result& structure,
                                              const Probability::Result& prob,
                                              Distribution::DistPool& pool, CellId cell);

private:
    // 阶乘对数表（线程局部，惰性扩展）。
    static std::vector<long double>& logFactorial();
    static void combiInit(int n);
    // 组合数（用阶乘对数避免溢出）。调用方保证 0<=k<=n；越界=调用 bug。
    static long double combLog(int n, int k);

    // 生成函数多项式：start 表示最低次幂，coeffs[i] 是 x^(start+i) 的系数。
    struct Polynomial {
        int start = 0;
        std::vector<long double> coeffs;

        Polynomial() = default;
        Polynomial(int s, std::vector<long double> c) : start(s), coeffs(std::move(c)) {}

        explicit Polynomial(const Distribution& dist);
        Polynomial operator*(const Polynomial& other) const;
        Polynomial operator/(const Polynomial& other) const;
    };

    // 候选方案数：把非前沿格的雷数组合也纳入。
    static long double denominator(const Polynomial& gf, int totalMines, int tSum);
};

// ── 实现区 ──

inline std::vector<long double>& Exact::logFactorial() {
    static thread_local std::vector<long double> table;
    return table;
}

inline void Exact::combiInit(int n) {
    auto& t = logFactorial();
    if (t.empty()) t.push_back(0.0);
    while (static_cast<int>(t.size()) <= n + 1)
        t.push_back(t.back() + std::log(static_cast<long double>(t.size())));
}

inline long double Exact::combLog(int n, int k) {
    // 调用方（denominator/analyze）均先检查 lightMines 在 [0, tSum] 内；
    // 越界到达这里 = 内部 bug。
    assert_(k >= 0 && k <= n, "Exact::combLog: 参数越界");
    if (k == 0 || k == n) return 1;
    combiInit(n);
    return std::exp(logFactorial()[static_cast<std::size_t>(n)] -
                    logFactorial()[static_cast<std::size_t>(k)] -
                    logFactorial()[static_cast<std::size_t>(n - k)]);
}

inline Exact::Polynomial::Polynomial(const Distribution& dist) {
    // 活组件的分布必有可行雷数（waySum > 0）；空分布 = shape/分布层 bug。
    assert_(!dist.entries.empty(), "Exact::Polynomial: 空分布");
    int minExp = dist.entries[0].mineCount;
    int maxExp = minExp;
    for (const auto& d : dist.entries) {
        minExp = std::min(minExp, d.mineCount);
        maxExp = std::max(maxExp, d.mineCount);
    }
    std::vector<long double> c(static_cast<std::size_t>(maxExp - minExp + 1), 0.0);
    for (const auto& d : dist.entries)
        c[static_cast<std::size_t>(d.mineCount - minExp)] = d.ways;
    *this = Polynomial(minExp, std::move(c));
}

inline Exact::Polynomial Exact::Polynomial::operator*(const Polynomial& other) const {
    const int start = this->start + other.start;
    const int size = static_cast<int>(coeffs.size()) +
                     static_cast<int>(other.coeffs.size()) - 1;
    std::vector<long double> res(static_cast<std::size_t>(size), 0.0);
    for (int i = 0; i < static_cast<int>(coeffs.size()); ++i)
        for (int j = 0; j < static_cast<int>(other.coeffs.size()); ++j)
            res[static_cast<std::size_t>(i + j)] +=
                coeffs[static_cast<std::size_t>(i)] *
                other.coeffs[static_cast<std::size_t>(j)];
    return Polynomial(start, std::move(res));
}

inline Exact::Polynomial Exact::Polynomial::operator/(const Polynomial& other) const {
    const int start = this->start - other.start;
    const int size = std::max(
        0, static_cast<int>(coeffs.size()) - static_cast<int>(other.coeffs.size()) + 1);
    std::vector<long double> res(static_cast<std::size_t>(size), 0.0);
    std::vector<long double> rem(coeffs);
    const long double otherLeading = other.coeffs.back();
    for (int i = static_cast<int>(rem.size()) - 1;
         i >= static_cast<int>(other.coeffs.size()) - 1; --i) {
        if (std::abs(rem[static_cast<std::size_t>(i)]) < 1e-12L) continue;
        const long double factor = rem[static_cast<std::size_t>(i)] / otherLeading;
        const int quotIdx = i - (static_cast<int>(other.coeffs.size()) - 1);
        res[static_cast<std::size_t>(quotIdx)] = factor;
        for (int j = 0; j < static_cast<int>(other.coeffs.size()); ++j)
            rem[static_cast<std::size_t>(i - j)] -=
                factor * other.coeffs[other.coeffs.size() - 1 - static_cast<std::size_t>(j)];
    }
    return Polynomial(start, std::move(res));
}

inline long double Exact::denominator(const Polynomial& gf, int totalMines, int tSum) {
    long double result = 0.0;
    for (int i = 0; i < static_cast<int>(gf.coeffs.size()); ++i) {
        const int heavyMines = gf.start + i;
        const int lightMines = totalMines - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum)
            result += gf.coeffs[static_cast<std::size_t>(i)] * combLog(tSum, lightMines);
    }
    return result;
}

inline Probability::Result Exact::analyze(const ObservedBoard& board,
                                          const Basic::Result& basic,
                                          const Structure::Result& structure,
                                          Distribution::DistPool& pool) {
    const int M = board.totalMines - basic.mineSum;
    const int tSum = basic.unknownSum;

    // 活组件：收集分布，对齐到 Result.components（下标 = ComponentId）。
    Probability::Result result;
    result.components.resize(structure.components.size());

    std::vector<const Distribution*> distList;
    std::vector<ComponentId> aliveIds;
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        aliveIds.push_back(cid);
        // 确保分布存在：计算并缓存（同一 shape 幂等命中）。
        distList.push_back(Distribution::Solver::analyze(*inst.shape, pool));
    }

    // 全块联合生成函数。
    Polynomial pH(0, {1.0});
    for (const auto* d : distList) pH = pH * Polynomial(*d);

    const long double denom = denominator(pH, M, tSum);

    // Unknown 格是雷的概率：总雷数 - 1 分配给其余格子。
    long double lightProb = 0.0;
    for (int i = 0; i < static_cast<int>(pH.coeffs.size()); ++i) {
        const int heavyMines = pH.start + i;
        const int lightMines = M - 1 - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum - 1)
            lightProb += pH.coeffs[static_cast<std::size_t>(i)] * combLog(tSum - 1, lightMines);
    }
    lightProb /= denom;
    result.tCellProbability = lightProb;
    result.candidates = denom;

    // 每连通块：各分布的取到概率 → 每 box 的雷概率（均摊）。
    for (std::size_t ai = 0; ai < aliveIds.size(); ++ai) {
        const ComponentId cid = aliveIds[ai];
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        const Distribution& dist = *distList[ai];
        const Polynomial pi(dist);
        const Polynomial ti = pH / pi;  // 去掉第 i 块后的联合分布

        std::vector<long double> boxProb(dist.entries.size(), 0.0L);
        for (std::size_t j = 0; j < dist.entries.size(); ++j) {
            const int v = dist.entries[j].mineCount;
            const long double w = dist.entries[j].ways;
            long double numerator = 0.0L;
            for (int k = 0; k < static_cast<int>(ti.coeffs.size()); ++k) {
                const int tMines = ti.start + k;
                const int lightMines = M - v - tMines;
                if (lightMines >= 0 && lightMines <= tSum)
                    numerator += w * ti.coeffs[static_cast<std::size_t>(k)] *
                                 combLog(tSum, lightMines);
            }
            boxProb[j] = numerator / denom;
        }

        // 每 box 期望 × 取到概率 → 均摊到单格。
        auto& cr = result.components[static_cast<std::size_t>(cid)];
        const Structure::Shape& shape = *inst.shape;
        cr.boxProbs.resize(shape.boxes.size());
        for (std::size_t b = 0; b < shape.boxes.size(); ++b) {
            long double perCell = 0.0L;
            for (std::size_t j = 0; j < dist.entries.size(); ++j)
                perCell += boxProb[j] * dist.entries[j].perBoxExpectation[b];
            cr.boxProbs[b] = perCell / static_cast<long double>(shape.boxes[b].size);
        }
    }

    return result;
}

// ── observe 实现 ──
// 方案（纯查询，零修改）：爆炸概率 = prob.mineProbability(cell)，直接取自
// analyze 的雷概率（已含全局预算卷积），不再自算；这里只算数字分布。
// 把"被抓住"的部分（x 邻居所属连通块 + x 所在连通块 + T 邻居伪源）预计算成
// 稀疏转移表 (h, y, w)，dp 逐表卷积得 dp[x][y]（x=邻居雷数，y=被抓住部分雷
// 数），其余棋盘折成 f[t]（恰有 t 雷的摆法数），预算卷积 y+t=M 后归一化。
inline Probability::ObserveResult Exact::observe(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, const Probability::Result& prob,
    Distribution::DistPool& pool, CellId cell) {
    using Mark = Basic::Mark;
    const auto [x, y] = board.pos(cell);
    const int rows = board.rows, cols = board.cols;
    const int tSum = basic.unknownSum;
    const int M = board.totalMines - basic.mineSum;

    Probability::ObserveResult out;

    // 边界：已揭示格不可开 → 全 0；basic 已定雷 → explosion=1（无数字）。
    if (board.board[x][y] != Cell::Hidden) return out;
    if (basic.marks[x][y] == Mark::Mine) {
        out.explosion = 1.0L;
        return out;
    }
    const bool xInT = (basic.marks[x][y] == Mark::Unknown);
    const CellLocation xloc = structure.cellLoc[static_cast<std::size_t>(cell)];
    const bool xInBox = (xloc.component >= 0);
    const ComponentId xCid = xloc.component;
    const BoxId xBox = xloc.box;

    // ── 步骤 1：被抓住的集合 = 邻居所属连通块 ∪ {x 所在连通块} ∪ T 邻居 ──
    // 邻居分类：basic Mine → fixed 源（常数）；Unknown → T 邻居（伪源）；
    //           Frontier → 所属连通块；已揭示数字 / Safe → 无贡献。
    // x 所在连通块不在此显式加入：x 是 Frontier ⟹ 邻接已揭示数字，且该数字
    // 属于 x 的连通块（cellLoc 回填），邻居循环必然捕获它；下方断言兜底。
    // 必须被捕获的原因：x 安全因子（池子 s−1）住在它的 box 转移里。
    int fixed = 0;
    int uT = 0;
    std::vector<ComponentId> captured;
    std::vector<char> seen(structure.components.size(), 0);
    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
        if (basic.marks[nx][ny] == Mark::Mine) {
            ++fixed;
            return;
        }
        if (basic.marks[nx][ny] == Mark::Unknown) {
            ++uT;
            return;
        }
        const CellLocation loc = structure.cellLoc[static_cast<std::size_t>(board.id(nx, ny))];
        if (loc.component == -1) return;  // 已揭示数字 / Safe
        if (!seen[static_cast<std::size_t>(loc.component)]) {
            seen[static_cast<std::size_t>(loc.component)] = 1;
            captured.push_back(loc.component);
        }
    });
    // x 在 box 里却未被捕获 = Basic/Structure 跨层不变式被破坏 → 概率静默错误。
    assert_(!xInBox || seen[static_cast<std::size_t>(xCid)] != 0,
            "Exact::observe: x 所在连通块未被捕获");

    // ── 步骤 2：转移预计算 —— 每个被抓住连通块折成稀疏转移表 ──
    // 每张表是 (h, y, w) 列表：邻域雷数贡献 h、块雷数 y、摆法数 w。
    // 超几何/box 细节只活在这里；dp 只吃表，不知道 box 语义。
    // maxY 是 dp 的 y 上限：被抓住部分总格数（T 伪源 u_T 格 + 被抓住各 box
    // 格数）。x 恒非雷（爆炸概率取自 prob），其雷数不进 y。
    struct Transfer {
        int h;  // 邻域雷数贡献
        int y;  // 块雷数
        long double w;
    };
    int maxY = uT;
    std::vector<std::vector<Transfer>> transfers;
    for (ComponentId cid : captured) {
        const Structure::Instance& inst = structure.components[static_cast<std::size_t>(cid)];
        const Structure::Shape& shape = *inst.shape;
        for (const auto& box : shape.boxes) maxY += box.size;
        // 各 box 与 x 相邻的格数（预计算专用，不外泄）
        std::vector<int> u(shape.boxes.size(), 0);
        for (std::size_t b = 0; b < shape.boxes.size(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [cx, cy] = board.pos(inst.boxes.cells[k]);
                if (std::abs(cx - x) <= 1 && std::abs(cy - y) <= 1 &&
                    !(cx == x && cy == y))
                    ++u[b];
            }
        // 枚举 box 雷数分配：y 由分配确定，邻域贡献是各相邻 box 超几何的卷积。
        // 权重 = ways × conv[h]：ways 含非相邻 box 的 C(s,m)，conv 把相邻 box
        // 的 C(s,m) 抵消、换成受限选格数（x 所在 box 池子恒为 s−1）。
        int maxTotal = 0;
        for (const auto& box : shape.boxes) maxTotal += box.size;
        std::vector<std::array<long double, 9>> acc(
            static_cast<std::size_t>(maxTotal + 1));
        Distribution::Solver::forEachAssignment(
            shape, [&](const std::vector<char>& assignment, long double ways) {
                int yInc = 0;
                std::array<long double, 9> conv{};
                conv[0] = 1.0L;
                for (std::size_t b = 0; b < assignment.size(); ++b) {
                    const int m = assignment[b];
                    yInc += m;
                    const int ub = u[b];
                    const bool isXBox = (cid == xCid &&
                                         static_cast<BoxId>(b) == xBox);
                    if (ub == 0 && !isXBox) continue;  // 不邻接且非 x 的 box：只进 y
                    const int s = shape.boxes[b].size;
                    const int pool = isXBox ? s - 1 : s;  // x 恒非雷：剔除 x
                    std::array<long double, 9> dist{};
                    const int rMax = (std::min)(ub, m);
                    for (int r = 0; r <= rMax; ++r) {
                        const int rest = m - r;
                        if (rest > pool - ub) continue;
                        dist[static_cast<std::size_t>(r)] =
                            combLog(ub, r) * combLog(pool - ub, rest) /
                            combLog(s, m);
                    }
                    // 卷积：conv ⊗ dist（h 平移 r）
                    std::array<long double, 9> nc{};
                    for (int h = 0; h <= 8; ++h)
                        if (conv[static_cast<std::size_t>(h)] != 0.0L)
                            for (int r = 0; r <= 8 - h; ++r)
                                nc[static_cast<std::size_t>(h + r)] +=
                                    conv[static_cast<std::size_t>(h)] *
                                    dist[static_cast<std::size_t>(r)];
                    conv = nc;
                }
                for (int h = 0; h <= 8; ++h)
                    if (conv[static_cast<std::size_t>(h)] != 0.0L)
                        acc[static_cast<std::size_t>(yInc)][static_cast<std::size_t>(h)] +=
                            ways * conv[static_cast<std::size_t>(h)];
            });
        std::vector<Transfer> table;
        for (int t = 0; t <= maxTotal; ++t)
            for (int h = 0; h <= 8; ++h) {
                const long double w =
                    acc[static_cast<std::size_t>(t)][static_cast<std::size_t>(h)];
                if (w != 0.0L) table.push_back(Transfer{h, t, w});
            }
        transfers.push_back(std::move(table));
    }
    // T 邻居伪源：u_T 个 size-1 box（每格 0/1 雷），同为一张 (r, r, C(u_T,r)) 表。
    // 预算不在此约束，由步骤 5 的 f[M−y] 兜底。
    if (uT > 0) {
        std::vector<Transfer> table;
        for (int r = 0; r <= uT; ++r)
            table.push_back(Transfer{r, r, combLog(uT, r)});
        transfers.push_back(std::move(table));
    }

    // ── 步骤 3：dp[x][y] —— 依次应用每张转移表 ──
    // dp 语义：x = 邻居雷数(0..8)，y = 被抓住部分总雷数(0..maxY)，值为方案数。
    const int stride = maxY + 1;
    auto runDp = [&]() -> std::vector<long double> {
        std::vector<long double> dp(static_cast<std::size_t>(9) * stride, 0.0L);
        dp[0 * stride + 0] = 1.0L;
        for (const auto& table : transfers) {
            std::vector<long double> ndp(static_cast<std::size_t>(9) * stride, 0.0L);
            for (const Transfer& t : table)
                for (int xv = 0; xv + t.h <= 8; ++xv)
                    for (int yv = 0; yv + t.y <= maxY; ++yv) {
                        const long double base = dp[xv * stride + yv];
                        if (base == 0.0L) continue;
                        ndp[(xv + t.h) * stride + yv + t.y] += base * t.w;
                    }
            dp.swap(ndp);
        }
        return dp;
    };

    // ── 步骤 4：f[t] —— 其余部分（非被抓住连通块 + 剩余 T 格）恰有 t 雷的摆法数 ──
    // T 池子 = tSum − u_T − [x∈T]：查询格 x 永不进其余部分，x∈T 时自身再剔除。
    // f 的下标范围到 tPool + 非抓住组件最大雷数（组件格子也能吃雷）。
    const int tPool = tSum - uT - (xInT ? 1 : 0);
    Polynomial pRest(0, {1.0});
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst = structure.components[static_cast<std::size_t>(cid)];
        if (seen[static_cast<std::size_t>(cid)]) continue;
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, pool);
        pRest = pRest * Polynomial(*dist);
    }
    const int pRestMax =
        pRest.coeffs.empty() ? 0 : pRest.start + static_cast<int>(pRest.coeffs.size()) - 1;
    std::vector<long double> f(static_cast<std::size_t>(tPool + pRestMax) + 1, 0.0L);
    for (std::size_t i = 0; i < pRest.coeffs.size(); ++i) {
        const long double w = pRest.coeffs[i];
        if (w == 0.0L) continue;
        const int t1 = pRest.start + static_cast<int>(i);
        // T 格最多吃 tPool 个雷（r 是 T 格雷数；组件雷数 t1 不受 tPool 限制）
        for (int r = 0; r <= tPool; ++r)
            f[static_cast<std::size_t>(t1 + r)] += w * combLog(tPool, r);
    }

    // ── 步骤 5：dp 与 f 的预算卷积（y + t = M），按邻居雷数分组 ──
    const int fMax = static_cast<int>(f.size()) - 1;
    const std::vector<long double> dpSafe = runDp();
    std::array<long double, 9> F{};
    for (int xv = 0; xv <= 8; ++xv)
        for (int yv = 0; yv <= maxY; ++yv) {
            const long double d = dpSafe[xv * stride + yv];
            if (d == 0.0L) continue;
            const int t = M - yv;
            if (t < 0 || t > fMax) continue;
            F[static_cast<std::size_t>(xv)] += d * f[static_cast<std::size_t>(t)];
        }

    // ── 步骤 6：explosion —— P(x 是雷) 直接取自 analyze 的雷概率 ──
    // （mineProbability 已含全局预算卷积；observe 的 DP 只算 x 非雷的配置，
    //   故 Σdigit + explosion = 1 由 N = ΣF + E 保证。）
    out.explosion = prob.mineProbability(cell, board, basic, structure);

    // ── 步骤 7：归一化（N = 全盘方案数，T 池子用 tSum，x 允许是雷）──
    Polynomial pAll = pRest;
    for (ComponentId cid : captured) {
        const Structure::Instance& inst = structure.components[static_cast<std::size_t>(cid)];
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, pool);
        pAll = pAll * Polynomial(*dist);
    }
    const long double N = denominator(pAll, M, tSum);
    for (int xv = 0; xv <= 8 && fixed + xv <= 8; ++xv)
        out.digit[static_cast<std::size_t>(fixed + xv)] =
            F[static_cast<std::size_t>(xv)] / N;
    return out;
}

}  // namespace mss