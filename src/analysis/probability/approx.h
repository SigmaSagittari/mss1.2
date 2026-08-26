#pragma once

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/rational.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// approx.h — 近似概率引擎（正态近似 + rho 加权，增量式）。
//
// 精确引擎在大棋盘上组合爆炸，这里改走单连通块的一/二阶矩近似：
//   1. 每连通块从分布提取 (mu, sigma2, lo, hi, logWays)，累计成全局矩。
//   2. 守恒方程 F(ρ) = Lbar + D·u(ρ) + tSum·ρ − M = 0 解 ρ（D=Hbar−Lbar，
//      u(ρ) = ∫₀ᵖw dt / ∫₀¹w dt 积分族填充，w(t)=1+k(t−½)+m(t−½)² 恒正
//      ⟹ 单调性数学保证；k、m 由聚合量回归预测）——带界牛顿 1-2 次；
//      端点/全确定性退化为解析解。
//   3. 系数层 RhoRational 把分布压成以 ρ 为参数的 box 概率函数：
//      单格查询 = eval(系数, ρ)，O(maxD)，无需全量落格。
//
// 与 Exact 的区别：Exact 无状态、每次全量重算、填数值 Result；
// Approx 有 Result（跨 update 累积全局矩），增量更新只增删变化
// 连通块的密度，rho 是 O(1) 全局标量 —— 这是"增量近似更新"的本体。
//
// 数据类：ShapeDensity / Result；算法类：Analyzer / Updater；
// 单格查询走 RhoRational::eval（系数由查询路径惰性记忆化）。数学细节全私有。
// ─────────────────────────────────────────────────────────────

struct Approx {
    // 每连通块形状的密度统计（从 shape 分布提取的一/二阶矩 + 支撑集）。
    struct ShapeDensity {
        long double mu = 0;       // E[X]：平均雷数
        long double sigma2 = 0;   // Var[X]：方差
        long double logWays = 0;  // ln(总摆法数)，用于候选数估算
        int lo = 0;               // 支撑集下界（最少雷数）
        int hi = 0;               // 支撑集上界（最多雷数）
    };

    // 增量引擎的结果容器。跨 update 累积全局矩；rho 是唯一"易变"量。
    // 系数层 RhoRational 是独立池，不在这里 —— Result 只装全局量。
    struct Result {
        std::vector<ShapeDensity> instanceDensity;  // 对齐 components，墓碑保留
        long double Ebar = 0;        // Σ mu_i
        long double Vbar = 0;        // Σ sigma2_i
        long double Lbar = 0;        // Σ lo_i（支撑集下界和，rho 求解用）
        long double Hbar = 0;        // Σ hi_i（支撑集上界和，rho 求解用）
        long double logWaysSum = 0;  // Σ log(ways_i)
        long double rho = 0;         // 当前全局密度（= 非前沿 Unknown 格雷密度）
        long double candidates = 0;  // 候选方案数（鞍点近似，只保证数量级）
    };

    struct Analyzer {
        // 全量构建（首次开局 / 重新对齐）：从零算全部实例密度 + 全局矩。
        static Result analyze(const ObservedBoard& board, const Basic::Result& basic,
                              const Structure::Result& structure,
                              Distribution::DistPool& dists);
    };

    struct Updater {
        // 增量：消费 Structure::Delta，只增减变化实例的密度与全局矩，重解 rho。
        // 就地改 result。系数层 RhoRational 由查询路径惰性记忆化，无需预热。
        static void update(const ObservedBoard& board, const Basic::Result& basic,
                           const Structure::Result& structure,
                           Distribution::DistPool& dists, Result& result,
                           const Structure::Delta& delta);
    };

    // 观察：点开格子 cell 的结果分布（爆炸概率 + 数字 0..8 概率）。
    // 概率 DP：dp[x][y] = 已处理被抓住块总雷数 x、邻域雷数 y 的概率（z 滚动）；
    // 每 (z, x) 状态从 Result 减已处理块矩、重解 ρ'(x) 后做归一化转移
    // （Bernstein 收缩基 [lo,hi]）；x 所在块拆安全/爆炸质量 → dp + 爆炸 = 1
    // 全程守恒；T 邻居按剩余 T 雷期望 k=round(tSum·ρ'') 的超几何伪源转移
    // （预算相关性必须保留，二项会把联合分布铺散开）后直出 digit + 简单归一化。
    // rho 只出概率不计数（候选数相对误差 ±1000%），故无 f[]、无预算卷积。
    static Probability::ObserveResult observe(const ObservedBoard& board,
                                              const Basic::Result& basic,
                                              const Structure::Result& structure,
                                              Distribution::DistPool& dists,
                                              const Approx::Result& result, CellId cell);

private:
    // 从单个分布提取密度统计。
    static ShapeDensity mineDensity(const Distribution& dist);

    // 解守恒方程得非前沿格密度 ρ。
    //   F(ρ) = Lbar + D·[ρ + A·ρ(1−ρ)²] + tSum·ρ − M，D = Hbar−Lbar，
    //   A = a0 + a1·p + a2·v（p=(Ebar−Lbar)/D, v=Vbar/D，百万局面回归，
    //   R²=0.9967）——自适应拟合填充：端点精确、u(0.5)=p 为恒等式。
    //   单调三次，带界牛顿 1-2 次。
    static long double solveRho(long double Ebar, long double Vbar, long double Lbar,
                                long double Hbar, long double M, long double tSum);

    // 候选数鞍点近似（对数域，只保证数量级）。
    static long double estimateCandidates(long double Ebar, long double Vbar,
                                          long double logWaysSum, long double M,
                                          long double tSum, long double rho);

    // 组合数自然对数（lgamma 版）。调用方已 clamp 到合法范围。
    static long double lnComb(int n, int k);

    // observe 内部辅助：
    // 超几何分布：size 格、雷 m、其中邻居格 n → 邻居格中恰 r 雷的概率。
    static std::vector<long double> hypergeom(int n, int s, int m);
    // 两个概率分布卷积。
    static std::vector<long double> convolve(const std::vector<long double>& a,
                                             const std::vector<long double>& b);
};

// ── 实现区 ──

inline Approx::ShapeDensity Approx::mineDensity(const Distribution& dist) {
    long double wSum = 0.0L, mu = 0.0L;
    for (const auto& e : dist.entries) {
        wSum += e.ways;
        mu += static_cast<long double>(e.mineCount) * e.ways;
    }
    // 活组件的分布必有可行解（waySum > 0）；无解说明 shape 构建或分布计算有 bug。
    assert_(wSum > 0.0L, "Approx::mineDensity: 分布无可行方案");
    mu /= wSum;

    long double sigma2 = 0.0L;
    for (const auto& e : dist.entries) {
        const long double delta = static_cast<long double>(e.mineCount) - mu;
        sigma2 += delta * delta * e.ways;
    }
    sigma2 /= wSum;

    return {mu, sigma2, std::log(wSum), dist.entries.front().mineCount,
            dist.entries.back().mineCount};
}

inline long double Approx::solveRho(long double Ebar, long double Vbar, long double Lbar,
                                    long double Hbar, long double M, long double tSum) {
    // 积分族填充（CDF 构造，单调性数学保证）：
    //   w(t) = 1 + k(t−½) + m(t−½)²（恒正 ⟹ u 严格递增）
    //   u(ρ) = ∫₀ᵖw dt / ∫₀¹w dt = [ρ + k·ρ(ρ−1)/2 + m·(ρ³/3−ρ²/2+ρ/4)] / (1+m/12)
    // 端点 u(0)=0、u(1)=1 自动；u(0.5)=p 恒等式（ρ=0.5 ⟺ θ=0 ⟹ 倾斜期望 =
    // 无条件期望 mu）由 k 吸收。k、m 由聚合量回归（样本外 122600 局面）：
    //   k = 3.10326 − 2.46788·p − 7.16219·p²
    //   m = 32.7711 − 210.679·v + 317.91·v²
    //   p = (Ebar−Lbar)/D，v = Vbar/D，D = Hbar−Lbar。
    // F(ρ) = Lbar + D·u(ρ) + tSum·ρ − M，F' = D·w(ρ)/(1+m/12) + tSum ≥ tSum > 0
    // ——严格单调、根唯一，带界牛顿 1-2 次。实测根误差（样本外）：
    // 平均 0.0068%、最大 0.139%、>1% 的 0 例——优于 cap 版（ρ+Aρ(1−ρ)²，其
    // A>3 时非单调、需 cap 缓解）4-8 倍。回归预测的 (k,m) 100% 落在 w>0 区。
    const long double D = Hbar - Lbar;
    if (M <= Lbar) return 0.0L;             // 雷不够填组件下限：T 格零雷
    if (M >= Hbar + tSum) return 1.0L;      // 雷超出组件上限：T 格全雷
    if (D <= 0.0L) {
        // 组件全确定性（lo=hi=mu）：u(ρ) 无定义，退化为线性解析解。
        if (tSum <= 0.0L) return 0.0L;      // 无非前沿格：rho 无定义，返回 0（无人查询）
        const long double rho = (M - Ebar) / tSum;
        return (rho < 0.0L) ? 0.0L : (rho > 1.0L ? 1.0L : rho);
    }
    // tSum=0：F = Lbar + D·u(ρ) − M = 0，牛顿直接解（无特殊分支）。
    const long double p = (Ebar - Lbar) / D;
    const long double v = Vbar / D;
    const long double k = 3.10326L - 2.46788L * p - 7.16219L * p * p;
    const long double m = 32.7711L - 210.679L * v + 317.91L * v * v;
    const long double z = 1.0L + m / 12.0L;
    long double lo = 0.0L, hi = 1.0L;
    long double rho = (M - Lbar) / (D + tSum);  // 线性填充解（k=m=0 情形）作初值
    if (rho < 1e-6L) rho = 1e-6L;
    if (rho > 1.0L - 1e-6L) rho = 1.0L - 1e-6L;
    for (int iter = 0; iter < 20; ++iter) {  // 实测 1-2 次，20 纯防御
        const long double u =
            (rho + k * rho * (rho - 1.0L) / 2.0L +
             m * (rho * rho * rho / 3.0L - rho * rho / 2.0L + rho / 4.0L)) / z;
        const long double F = Lbar + D * u + tSum * rho - M;
        const long double scale = std::abs(Lbar) + D + tSum + std::abs(M);
        if (std::abs(F) <= 100.0L * LDBL_EPSILON * scale) break;  // 到噪声地板
        if (F < 0.0L)
            lo = rho;
        else
            hi = rho;
        const long double w =
            1.0L + k * (rho - 0.5L) + m * (rho - 0.5L) * (rho - 0.5L);
        const long double dF = D * w / z + tSum;
        const long double nr = rho - F / dF;
        rho = (nr > lo && nr < hi) ? nr : (lo + hi) / 2.0L;
    }
    return rho;
}

inline long double Approx::estimateCandidates(long double Ebar, long double Vbar,
                                              long double logWaysSum, long double M,
                                              long double tSum, long double rho) {
    const long double theta = std::log(rho / (1.0L - rho));
    const long double eFront = Ebar + Vbar * theta;  // 前沿期望雷数
    int k = static_cast<int>(std::llround(M - eFront));  // 非前沿(T格)期望雷数
    if (k < 0) k = 0;
    if (k > static_cast<int>(tSum)) k = static_cast<int>(tSum);
    return std::exp(logWaysSum + lnComb(static_cast<int>(tSum), k));
}

inline long double Approx::lnComb(int n, int k) {
    // 调用方已 clamp 到合法范围；越界=调用 bug。
    assert_(k >= 0 && k <= n, "Approx::lnComb: 参数越界");
    return std::lgamma(static_cast<long double>(n) + 1.0L) -
           std::lgamma(static_cast<long double>(k) + 1.0L) -
           std::lgamma(static_cast<long double>(n - k) + 1.0L);
}

inline std::vector<long double> Approx::hypergeom(int n, int s, int m) {
    // P(邻居格中恰 r 雷) = C(n,r)·C(s−n,m−r)/C(s,m)，r = 0..n。
    // 调用方保证 m ≤ s（越界 = 结构 bug）。
    std::vector<long double> out(static_cast<std::size_t>(n + 1), 0.0L);
    const long double den = lnComb(s, m);
    for (int r = 0; r <= n; ++r) {
        const int rest = m - r;
        if (rest < 0 || rest > s - n) continue;
        out[static_cast<std::size_t>(r)] =
            std::exp(lnComb(n, r) + lnComb(s - n, rest) - den);
    }
    return out;
}

inline std::vector<long double> Approx::convolve(const std::vector<long double>& a,
                                                 const std::vector<long double>& b) {
    std::vector<long double> c(a.size() + b.size() - 1, 0.0L);
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i] != 0.0L)
            for (std::size_t j = 0; j < b.size(); ++j)
                c[i + j] += a[i] * b[j];
    return c;
}

inline Probability::ObserveResult Approx::observe(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, Distribution::DistPool& dists,
    const Approx::Result& result, CellId cell) {
    using Mark = Basic::Mark;
    const auto [qx, qy] = board.pos(cell);
    const int rows = board.rows, cols = board.cols;
    const int tSum = basic.unknownSum;
    const long double M = static_cast<long double>(board.totalMines - basic.mineSum);

    Probability::ObserveResult out;

    // 边界：已揭示格不可开 → 全 0；basic 已定雷 → explosion=1。
    if (board.board[qx][qy] != Cell::Hidden) return out;
    if (basic.marks[qx][qy] == Mark::Mine) {
        out.explosion = 1.0L;
        return out;
    }
    const bool xInT = (basic.marks[qx][qy] == Mark::Unknown);
    const CellLocation xloc = structure.cellLoc[static_cast<std::size_t>(cell)];
    const bool xInBox = (xloc.component >= 0);
    const ComponentId xCid = xloc.component;
    const BoxId xBox = xloc.box;

    // ── 步骤 1：邻居分类（同 Exact）──
    // Mine → fixed 源；Unknown → uT 伪源；Frontier → 所属连通块（去重）。
    int fixed = 0;
    int uT = 0;
    std::vector<ComponentId> captured;
    std::vector<char> seen(structure.components.size(), 0);
    forEachAdjacent(qx, qy, rows, cols, [&](int nx, int ny) {
        if (basic.marks[nx][ny] == Mark::Mine) {
            ++fixed;
            return;
        }
        if (basic.marks[nx][ny] == Mark::Unknown) {
            ++uT;
            return;
        }
        const CellLocation loc = structure.cellLoc[static_cast<std::size_t>(board.id(nx, ny))];
        if (loc.component < 0) return;  // 已揭示数字 / Safe
        if (!seen[static_cast<std::size_t>(loc.component)]) {
            seen[static_cast<std::size_t>(loc.component)] = 1;
            captured.push_back(loc.component);
        }
    });
    // x 在 box 里却未被捕获 = Basic/Structure 跨层不变式被破坏。
    assert_(!xInBox || seen[static_cast<std::size_t>(xCid)] != 0,
            "Approx::observe: x 所在连通块未被捕获");

    // ── 步骤 2：结构预计算（rho 无关，纯结构）──
    // 每被抓住块折成分配表：总雷数 m、ways、邻域贡献 conv（归一化超几何，
    // x 所在 box 池子 s−1 的安全侧）、安全/爆炸占比。枚举只做一次。
    struct Assign {
        int m = 0;
        long double ways = 0;
        std::array<long double, 9> conv{};
        long double safeFrac = 1.0L;  // 安全质量占比（x 非雷）
        long double mineFrac = 0.0L;  // 爆炸质量占比（x 是雷）
    };
    struct CompPre {
        ComponentId cid = -1;
        const Distribution* dist = nullptr;
        int lo = 0;  // Bernstein 收缩基支持集 [lo, hi]
        int hi = 0;
        std::vector<Assign> assigns;
    };
    int xMax = uT;  // dp 的 x 上限：被抓住块最大雷数 + T 邻居
    std::vector<CompPre> pre;
    pre.reserve(captured.size());
    for (ComponentId cid : captured) {
        const Structure::Instance& inst = structure.components[static_cast<std::size_t>(cid)];
        const Structure::Shape& shape = *inst.shape;
        for (const auto& box : shape.boxes) xMax += box.size;
        const Distribution* dist = Distribution::Solver::analyze(shape, dists);
        const bool hasX = (cid == xCid);
        // 各 box 与 x 相邻的格数（预计算专用，不外泄）
        std::vector<int> u(shape.boxes.size(), 0);
        for (std::size_t b = 0; b < shape.boxes.size(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [cx, cy] = board.pos(inst.boxes.cells[k]);
                if (std::abs(cx - qx) <= 1 && std::abs(cy - qy) <= 1 &&
                    !(cx == qx && cy == qy))
                    ++u[b];
            }
        CompPre cp;
        cp.cid = cid;
        cp.dist = dist;
        cp.lo = dist->entries.front().mineCount;
        cp.hi = dist->entries.back().mineCount;
        Distribution::Solver::forEachAssignment(
            shape, [&](const std::vector<char>& assignment, long double ways) {
                Assign a;
                a.ways = ways;
                std::vector<long double> conv{1.0L};
                for (std::size_t b = 0; b < assignment.size(); ++b) {
                    const int mb = assignment[b];
                    a.m += mb;
                    const int ub = u[b];
                    const bool isXBox = (hasX && static_cast<BoxId>(b) == xBox);
                    if (ub == 0 && !isXBox) continue;  // 不邻接且非 x 的 box：无邻域贡献
                    const int s = shape.boxes[b].size;
                    if (isXBox) {
                        a.safeFrac = static_cast<long double>(s - mb) / s;
                        a.mineFrac = static_cast<long double>(mb) / s;
                    }
                    const int pool = isXBox ? s - 1 : s;
                    if (mb > pool) {
                        conv.assign(1, 0.0L);  // 安全侧不可能（x 必雷）→ 贡献全零
                    } else {
                        conv = convolve(conv, hypergeom(ub, pool, mb));
                    }
                }
                for (std::size_t h = 0; h < conv.size(); ++h)
                    a.conv[h] = conv[h];
                cp.assigns.push_back(std::move(a));
            });
        pre.push_back(std::move(cp));
    }

    // ── 步骤 3：概率 DP（z 滚动）──
    // dp[x][y]：已处理被抓住块总雷数 x、邻域雷数 y 的概率。
    // 每 (z, x) 状态：从 Result 减已处理块矩、重解 ρ'(x)，本块按 ρ'(x) 的
    // 归一化倾斜分布转移；x 所在块拆分安全/爆炸质量 → dp + 爆炸 = 1 守恒。
    // T 池用全量 tSum：uT 邻居与 x∈T 本身是 T 池成员，密度必须与池一致
    // （Exact 的 tPool 减去它们是因为那边是精确计数；这里没有）。
    // rho 只出概率，绝不计数（计数相对误差 ±1000%）。
    const int stride = 9;
    const long double tSumL = static_cast<long double>(tSum);
    std::vector<long double> dp(static_cast<std::size_t>(xMax + 1) * stride, 0.0L);
    dp[0 * stride + 0] = 1.0L;
    long double E = 0.0L;
    long double Eproc = 0.0L, Vproc = 0.0L, Lproc = 0.0L, Hproc = 0.0L;

    auto collectActive = [&]() {
        std::vector<int> out;
        for (int xs = 0; xs <= xMax; ++xs) {
            bool any = false;
            for (int y = 0; y <= 8; ++y)
                if (dp[static_cast<std::size_t>(xs) * stride + y] != 0.0L) {
                    any = true;
                    break;
                }
            if (any) out.push_back(xs);
        }
        return out;
    };

    for (const CompPre& cp : pre) {
        const std::vector<int> activeX = collectActive();
        std::vector<long double> ndp(dp.size(), 0.0L);
        for (int xs : activeX) {
            const long double rhoP =
                solveRho(result.Ebar - Eproc, result.Vbar - Vproc,
                         result.Lbar - Lproc, result.Hbar - Hproc,
                         M - static_cast<long double>(xs), tSumL);
            // 归一化常数 Z = Σ entries ways·ρ'^(m−lo)·(1−ρ')^(hi−m)
            long double Z = 0.0L;
            for (const auto& e : cp.dist->entries) {
                const int me = e.mineCount;
                Z += e.ways * std::pow(rhoP, me - cp.lo) *
                     std::pow(1.0L - rhoP, cp.hi - me);
            }
            if (Z <= 0.0L) continue;
            long double rowSum = 0.0L;
            for (int y = 0; y <= 8; ++y)
                rowSum += dp[static_cast<std::size_t>(xs) * stride + y];
            for (const Assign& a : cp.assigns) {
                const long double w =
                    a.ways * std::pow(rhoP, a.m - cp.lo) *
                    std::pow(1.0L - rhoP, cp.hi - a.m) / Z;
                // 爆炸质量（x 所在块）
                if (a.mineFrac != 0.0L) E += rowSum * w * a.mineFrac;
                if (a.safeFrac == 0.0L) continue;
                const int nx = xs + a.m;
                if (nx > xMax) continue;
                for (int y = 0; y <= 8; ++y) {
                    const long double base = dp[static_cast<std::size_t>(xs) * stride + y];
                    if (base == 0.0L) continue;
                    for (int h = 0; h + y <= 8; ++h) {
                        const long double c = a.conv[static_cast<std::size_t>(h)];
                        if (c == 0.0L) continue;
                        ndp[static_cast<std::size_t>(nx) * stride + y + h] +=
                            base * w * a.safeFrac * c;
                    }
                }
            }
        }
        dp.swap(ndp);
        const ShapeDensity& d = result.instanceDensity[static_cast<std::size_t>(cp.cid)];
        Eproc += d.mu;
        Vproc += d.sigma2;
        Lproc += static_cast<long double>(d.lo);
        Hproc += static_cast<long double>(d.hi);
    }

    // ── 步骤 4：T 邻居伪源（+ x∈T 因子）──
    // 每状态重解 ρ''，剩余 T 雷期望 μ = tSum·ρ''。T 邻居按超几何
    // C(uT,r)·C(池−uT, k−r)/C(池,k) 转移——预算相关性必须保留：二项会
    // 无视"剩余雷数少"的负相关，把小 T 池的联合分布铺散开（误差可达 0.2+）。
    // μ 有离散性（组件雷数非定点），用 k0=floor(μ) 与 k0+1 的两点混合平滑；
    // x∈T 时 x 是 T 池成员：P(x 雷) = μ/tSum，安全侧池子减 1。
    if (uT > 0 || xInT) {
        const std::vector<int> activeX = collectActive();
        std::vector<long double> ndp(dp.size(), 0.0L);
        for (int xs : activeX) {
            const long double rhoP =
                solveRho(result.Ebar - Eproc, result.Vbar - Vproc,
                         result.Lbar - Lproc, result.Hbar - Hproc,
                         M - static_cast<long double>(xs), tSumL);
            long double mu = rhoP * tSumL;
            if (mu < 0.0L) mu = 0.0L;
            if (mu > tSumL) mu = tSumL;
            long double rowSum = 0.0L;
            for (int y = 0; y <= 8; ++y)
                rowSum += dp[static_cast<std::size_t>(xs) * stride + y];
            if (xInT) E += rowSum * (mu / tSumL);  // 爆炸 = T 池密度
            const int k0 = static_cast<int>(std::floor(mu));
            const long double f = mu - static_cast<long double>(k0);
            for (int kk = 0; kk <= 1; ++kk) {
                const int k = k0 + kk;
                const long double wk = (kk == 0) ? (1.0L - f) : f;
                if (wk <= 0.0L) continue;
                const int pool = xInT ? tSum - 1 : tSum;
                if (k > pool) continue;  // x∈T 且全池皆雷：安全质量 0
                const long double xFrac =
                    xInT ? (1.0L - static_cast<long double>(k) / tSumL) : 1.0L;
                if (xFrac <= 0.0L) continue;
                const std::vector<long double> hg = hypergeom(uT, pool, k);
                for (int r = 0; r <= uT && r <= 8; ++r) {
                    const long double w = wk * xFrac * hg[static_cast<std::size_t>(r)];
                    if (w == 0.0L) continue;
                    for (int y = 0; y + r <= 8; ++y) {
                        const long double base = dp[static_cast<std::size_t>(xs) * stride + y];
                        if (base == 0.0L) continue;
                        ndp[static_cast<std::size_t>(xs + r) * stride + y + r] += base * w;
                    }
                }
            }
        }
        dp.swap(ndp);
    }

    // ── 步骤 5：直出 + 简单归一化 ──
    std::array<long double, 9> D{};
    long double Dtot = 0.0L;
    for (int xs = 0; xs <= xMax; ++xs)
        for (int y = 0; y <= 8; ++y) {
            const long double v = dp[static_cast<std::size_t>(xs) * stride + y];
            D[static_cast<std::size_t>(y)] += v;
            Dtot += v;
        }
    out.explosion = (E < 0.0L) ? 0.0L : (E > 1.0L ? 1.0L : E);
    const long double safeMass = 1.0L - out.explosion;
    if (Dtot > 0.0L)
        for (int k = 0; k <= 8 && fixed + k <= 8; ++k)
            out.digit[static_cast<std::size_t>(fixed + k)] =
                D[static_cast<std::size_t>(k)] * safeMass / Dtot;
    return out;
}

inline Approx::Result Approx::Analyzer::analyze(const ObservedBoard& board,
                                                const Basic::Result& basic,
                                                const Structure::Result& structure,
                                                Distribution::DistPool& dists) {
    const long double M = static_cast<long double>(board.totalMines - basic.mineSum);
    const long double tSum = static_cast<long double>(basic.unknownSum);

    Result result;
    result.instanceDensity.resize(structure.components.size());

    // 活组件（跳过墓碑）：累计全局矩。
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        if (!inst.alive) continue;
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, dists);
        const ShapeDensity d = mineDensity(*dist);
        result.instanceDensity[static_cast<std::size_t>(cid)] = d;
        result.Ebar += d.mu;
        result.Vbar += d.sigma2;
        result.Lbar += static_cast<long double>(d.lo);
        result.Hbar += static_cast<long double>(d.hi);
        result.logWaysSum += d.logWays;
    }

    result.rho = solveRho(result.Ebar, result.Vbar, result.Lbar, result.Hbar, M, tSum);
    result.candidates =
        estimateCandidates(result.Ebar, result.Vbar, result.logWaysSum, M, tSum, result.rho);
    return result;
}

inline void Approx::Updater::update(const ObservedBoard& board,
                                    const Basic::Result& basic,
                                    const Structure::Result& structure,
                                    Distribution::DistPool& dists, Result& result,
                                    const Structure::Delta& delta) {
    const long double M = static_cast<long double>(board.totalMines - basic.mineSum);
    const long double tSum = static_cast<long double>(basic.unknownSum);

    // 摘除的连通块：减去墓碑实例的密度（数据保留，undo 可恢复）。
    for (ComponentId cid : delta.removed) {
        const ShapeDensity& d = result.instanceDensity[static_cast<std::size_t>(cid)];
        result.Ebar -= d.mu;
        result.Vbar -= d.sigma2;
        result.Lbar -= static_cast<long double>(d.lo);
        result.Hbar -= static_cast<long double>(d.hi);
        result.logWaysSum -= d.logWays;
    }

    // 新重建的连通块：算密度 + 累计全局矩。
    result.instanceDensity.resize(structure.components.size());
    for (ComponentId cid : delta.added) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, dists);
        const ShapeDensity d = mineDensity(*dist);
        result.instanceDensity[static_cast<std::size_t>(cid)] = d;
        result.Ebar += d.mu;
        result.Vbar += d.sigma2;
        result.Lbar += static_cast<long double>(d.lo);
        result.Hbar += static_cast<long double>(d.hi);
        result.logWaysSum += d.logWays;
    }

    result.rho = solveRho(result.Ebar, result.Vbar, result.Lbar, result.Hbar, M, tSum);
    result.candidates =
        estimateCandidates(result.Ebar, result.Vbar, result.logWaysSum, M, tSum, result.rho);
}

}  // namespace mss