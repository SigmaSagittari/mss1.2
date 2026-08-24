#pragma once

#include <algorithm>
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
//   1. 每连通块从分布提取 (mu, sigma2, logWays)，累计成全局矩。
//   2. 守恒方程 F(θ) = Ebar + Vbar·θ + tSum·σ(θ) − M = 0 解 ρ（θ=logit(ρ)）：
//        Vbar=0 → 解析解 σ=(M−Ebar)/tSum（零迭代）；
//        Vbar>0 → logit 空间对 θ 牛顿（无边界钳位，二次收敛，实测 ≤6 次）。
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
    // 每连通块形状的密度统计（从 shape 分布提取的一/二阶矩）。
    struct ShapeDensity {
        long double mu = 0;       // E[X]：平均雷数
        long double sigma2 = 0;   // Var[X]：方差
        long double logWays = 0;  // ln(总摆法数)，用于候选数估算
    };

    // 增量引擎的结果容器。跨 update 累积全局矩；rho 是唯一"易变"量。
    // 系数层 RhoRational 是独立池，不在这里 —— Result 只装全局量。
    struct Result {
        std::vector<ShapeDensity> instanceDensity;  // 对齐 components，墓碑保留
        long double Ebar = 0;        // Σ mu_i
        long double Vbar = 0;        // Σ sigma2_i
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
    // 与 Exact::observe 同签名（多一个 rho），填同一份 ObserveResult。
    // 算法：rho 加权 each 连通块分配（P∝ways·(rho/(1-rho))^m），
    // 邻域贡献 box 超几何卷积，T 格用 rho 独立补足。
    static Probability::ObserveResult observe(const ObservedBoard& board,
                                              const Basic::Result& basic,
                                              const Structure::Result& structure,
                                              Distribution::DistPool& dists,
                                              long double rho, CellId cell);

private:
    // 从单个分布提取密度统计。
    static ShapeDensity mineDensity(const Distribution& dist);

    // 解守恒方程得非前沿格密度 ρ。
    //   Vbar<1e-10（含 0 与噪声级）→ 解析解；Vbar≥1e-10 → logit 空间对 θ 牛顿
    //   （θ 仅内部使用，不对外）。
    static long double solveRho(long double Ebar, long double Vbar,
                                long double M, long double tSum);

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

    return {mu, sigma2, std::log(wSum)};
}

inline long double Approx::solveRho(long double Ebar, long double Vbar,
                                    long double M, long double tSum) {
    // Vbar=0（含噪声级：增量累积的浮点残留。真实方差要么精确 0，要么
    // ≥1e-2 量级，1e-10 以下必是噪声）：方程退化为线性 Ebar + tSum·σ − M = 0，
    // σ 解析可得，且天然落在 (0,1)（前端组合数保证 M−Ebar ∈ [0, tSum]），零迭代。
    // 噪声级 Vbar 若走牛顿：根在 σ~1e-19 处，dF 被 tSum·σ(1−σ) 主导，
    // 每步只把 θ 挪 ~1，10 次收敛不到——按 0 处理直接给解析解。
    if (Vbar < 1e-10L) {
        if (tSum <= 0.0L) return 0.0L;  // 无非前沿格：rho 无定义，返回 0（无人查询）
        const long double rho = (M - Ebar) / tSum;
        return (rho < 0.0L) ? 0.0L : (rho > 1.0L ? 1.0L : rho);
    }

    // Vbar>0：logit 空间对 θ 牛顿。θ = logit(ρ)，σ(θ) = 1/(1+e^-θ)：
    //   F(θ) = Ebar + Vbar·θ + tSum·σ(θ) − M，F'(θ) = Vbar + tSum·σ(1−σ) ≥ Vbar > 0。
    // θ∈ℝ 无边界，σ(θ)∈(0,1) 恒在域内——无钳位、无越界。ρ 空间牛顿在边界
    // （rho<0.05 或 >0.95）会大步越界、钳位后退化为线性爬升，12 次仍不收敛；
    // logit 空间根除该问题：边界场景 1~6 次、二次收敛。θ 仅内部计算。
    long double theta = 0.0L;
    if (tSum > 0.0L) {
        // 初值：Vbar=0 的解析解（线性近似）钳到安全开区间后转 logit，
        // 边界处 θ 空间近似仿射，常 0~1 次命中。
        long double r = (M - Ebar) / tSum;
        if (r < 1e-3L) r = 1e-3L;
        if (r > 1.0L - 1e-3L) r = 1.0L - 1e-3L;
        theta = std::log(r / (1.0L - r));
    }
    for (int iter = 0; iter < 10; ++iter) {  // 实测 ≤6 次，10 纯防御
        const long double sig = 1.0L / (1.0L + std::exp(-theta));
        const long double F = Ebar + Vbar * theta + tSum * sig - M;
        const long double dF = Vbar + tSum * sig * (1.0L - sig);
        const long double step = F / dF;
        theta -= step;
        if (std::abs(step) < 1e-15L) break;
    }
    return 1.0L / (1.0L + std::exp(-theta));
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
        result.logWaysSum += d.logWays;
    }

    result.rho = solveRho(result.Ebar, result.Vbar, M, tSum);
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
        result.logWaysSum += d.logWays;
    }

    result.rho = solveRho(result.Ebar, result.Vbar, M, tSum);
    result.candidates =
        estimateCandidates(result.Ebar, result.Vbar, result.logWaysSum, M, tSum, result.rho);
}

}  // namespace mss