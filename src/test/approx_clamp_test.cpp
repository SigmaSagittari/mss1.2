// ─────────────────────────────────────────────────────────────
// test/approx_clamp_test.cpp — 聚合 clamp 高斯（有界高斯）方案测量。
//
// 编译（同 solve_rho_test，项目根执行）：
//   D:\codeenvset\mingw64\bin\g++.exe -std=c++20 -O2 -Isrc ^
//       src\test\approx_clamp_test.cpp -o build\approx_clamp_test.exe
//   build\approx_clamp_test.exe
//
// 被测方案：对聚合量 clamp（用户提出的"有上下限的高斯"）
//   F(θ) = clamp(Ebar + Vbar·θ, Σlo, Σhi) + tSum·σ(θ) − M
// 三段解析判定 + 中段带界牛顿（零到两次 logit，比现高斯更少）。
// 与现高斯（Ebar + Vbar·θ，无界）和精确 T 密度（Exact）三方对比。
//
// 断言（数据驱动）：
//   A. clamp 求解器自身：与 clamp F 的 θ 空间二分逐例一致（求解器正确）。
//   B. clamp 结果恒在 [0,1]。
//   C. 平均/最大 |clamp − 精确| < 平均/最大 |gauss − 精确|（有界化确实改进）。
//   D. 组件总期望越界（Ebar+Vbar·θ 出 [Σlo,Σhi]）在 gauss 下存在、clamp 恒 0。
// ─────────────────────────────────────────────────────────────
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/approx.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "ui/game_control.h"
#include "ui/interactive.h"

using namespace mss;
using ld = long double;

static int gFail = 0;
static long long gCheck = 0;

static ld sig(ld theta) { return 1.0L / (1.0L + std::exp(-theta)); }

static void check(bool ok, const char* what) {
    ++gCheck;
    if (!ok) {
        ++gFail;
        std::printf("  FAIL: %s\n", what);
    }
}

// ── 被测：聚合 clamp 求解器（三段解析 + 中段带界牛顿）──
static ld clampSolveRho(ld Ebar, ld Vbar, ld L, ld H, ld M, ld tSum, int& iters,
                        int& branch) {
    iters = 0;
    if (M <= L) { branch = 1; return 0.0L; }           // 下界：T 格零雷
    if (M >= H + tSum) { branch = 1; return 1.0L; }    // 上界：全 T 格皆雷
    if (Vbar < 1e-10L) {                               // 全确定性：线性解析
        if (tSum <= 0.0L) { branch = 1; return 0.0L; }
        const ld rho = (M - Ebar) / tSum;
        branch = 1;
        return (rho < 0.0L) ? 0.0L : (rho > 1.0L ? 1.0L : rho);
    }
    const ld thLo = (L - Ebar) / Vbar;                 // 下饱和边界
    const ld thHi = (H - Ebar) / Vbar;                 // 上饱和边界
    if (tSum > 0.0L) {
        const ld sL = (M - L) / tSum;                  // 下段根：σ* = (M−L)/tSum
        if (sL > 0.0L && sL < 1.0L) {
            const ld th = std::log(sL / (1.0L - sL));
            if (th <= thLo) { branch = 2; return sL; }
        }
        const ld sH = (M - H) / tSum;                  // 上段根：σ* = (M−H)/tSum
        if (sH > 0.0L && sH < 1.0L) {
            const ld th = std::log(sH / (1.0L - sH));
            if (th >= thHi) { branch = 3; return sH; }
        }
    }
    // 根在中段：E + Vθ + tSum·σ = M（与现高斯同式，但 θ 钳在 [thLo, thHi]，
    // 带界牛顿——F 单调连续、根唯一且在段内，必收敛）。
    branch = 4;
    ld theta = 0.0L;
    if (tSum > 0.0L) {
        ld r = (M - Ebar) / tSum;
        if (r < 1e-3L) r = 1e-3L;
        if (r > 1.0L - 1e-3L) r = 1.0L - 1e-3L;
        theta = std::log(r / (1.0L - r));
    }
    if (theta < thLo) theta = thLo;
    if (theta > thHi) theta = thHi;
    ld lo = thLo, hi = thHi;
    for (int iter = 0; iter < 10; ++iter) {
        ++iters;
        const ld s = 1.0L / (1.0L + std::exp(-theta));
        const ld F = Ebar + Vbar * theta + tSum * s - M;
        const ld scale = std::abs(Ebar) + std::abs(Vbar * theta) + tSum + std::abs(M);
        if (std::abs(F) <= 100.0L * LDBL_EPSILON * scale) break;
        if (F < 0.0L)
            lo = theta;
        else
            hi = theta;
        const ld dF = Vbar + tSum * s * (1.0L - s);
        const ld nr = theta - F / dF;
        theta = (nr > lo && nr < hi) ? nr : (lo + hi) / 2.0L;
    }
    return sig(theta);
}

// ── 参考：现高斯求解器（同构副本）──
static ld gaussSolveRho(ld Ebar, ld Vbar, ld M, ld tSum) {
    if (Vbar < 1e-10L) {
        if (tSum <= 0.0L) return 0.0L;
        const ld rho = (M - Ebar) / tSum;
        return (rho < 0.0L) ? 0.0L : (rho > 1.0L ? 1.0L : rho);
    }
    ld theta = 0.0L;
    if (tSum > 0.0L) {
        ld r = (M - Ebar) / tSum;
        if (r < 1e-3L) r = 1e-3L;
        if (r > 1.0L - 1e-3L) r = 1.0L - 1e-3L;
        theta = std::log(r / (1.0L - r));
    }
    for (int iter = 0; iter < 10; ++iter) {
        const ld s = 1.0L / (1.0L + std::exp(-theta));
        const ld F = Ebar + Vbar * theta + tSum * s - M;
        const ld dF = Vbar + tSum * s * (1.0L - s);
        theta -= F / dF;
        const ld scale = std::abs(Ebar) + std::abs(Vbar * theta) + tSum + std::abs(M);
        if (std::abs(F) <= 100.0L * LDBL_EPSILON * scale) break;
    }
    return sig(theta);
}

// clamp F 的 θ 空间二分参考（验证求解器自身）
static ld bisectClamp(ld Ebar, ld Vbar, ld L, ld H, ld M, ld tSum) {
    ld lo = -100000.0L, hi = 100000.0L;
    for (int i = 0; i < 300; ++i) {
        const ld mid = (lo + hi) / 2.0L;
        const ld m = Ebar + Vbar * mid;
        const ld mc = (m < L) ? L : (m > H) ? H : m;
        if (mc + tSum * sig(mid) - M < 0) lo = mid;
        else hi = mid;
    }
    return sig((lo + hi) / 2.0L);
}

int main() {
    std::printf("== 聚合 clamp 高斯 vs 现高斯 vs 精确 T 密度 ==\n");
    std::mt19937_64 rng(2026);
    struct BC { int rows, cols, mines; const char* name; };
    const std::vector<BC> boards = {
        {9, 9, 10, "9x9/10"},
        {16, 16, 40, "16x16/40"},
        {30, 16, 99, "30x16/99"},
    };

    long long n = 0;
    ld sumG = 0.0L, sumC = 0.0L, worstG = 0.0L, worstC = 0.0L;
    long long gt1pG = 0, gt1pC = 0, clampWins = 0, gaussWins = 0;
    long long oobGauss = 0, oobClamp = 0;
    long long oobN = 0, gt1pGOob = 0, gt1pCOob = 0;
    ld sumGOob = 0.0L, sumCOob = 0.0L;
    long long br[5] = {};      // clamp 分支：1 边界/解析 2 下段 3 上段 4 中段
    long long itSum = 0, itMax = 0;
    ld gaussDeltaSum = 0.0L, clampDeltaSum = 0.0L;

    for (const auto& b : boards) {
        const int total = b.rows * b.cols;
        for (int trial = 0; trial < 40; ++trial) {
            GameController gc(b.rows, b.cols, b.mines, static_cast<unsigned>(rng()));
            const int pick = static_cast<int>(rng() % (unsigned long long)total);
            gc.reveal(pick / b.cols + 1, pick % b.cols + 1);
            Distribution::DistPool testDists;  // 测试自有的分布池（lo/hi 提取）
            for (int step = 0; step < 200; ++step) {
                const auto& gi = gc.info();
                if (gi.status != GameController::Status::Playing) break;
                auto& an = gc.analysis();
                const auto& app = an.approx();
                const auto& basic = an.basicMarks();
                const auto& structure = an.structure();
                const ld M = static_cast<ld>(gi.mines - basic.mineSum);
                const ld tSum = static_cast<ld>(basic.unknownSum);
                if (tSum <= 0.0L) continue;
                // 组件级聚合（Ebar/Vbar 顺带核对引擎聚合一致）
                ld E = 0.0L, V = 0.0L, L = 0.0L, H = 0.0L;
                for (ComponentId cid = 0;
                     cid < static_cast<ComponentId>(structure.components.size()); ++cid) {
                    const Structure::Instance& inst =
                        structure.components[static_cast<std::size_t>(cid)];
                    if (!inst.alive) continue;
                    const Distribution* dist =
                        Distribution::Solver::analyze(*inst.shape, testDists);
                    const Approx::ShapeDensity& d =
                        app.instanceDensity[static_cast<std::size_t>(cid)];
                    E += d.mu;
                    V += d.sigma2;
                    L += static_cast<ld>(dist->entries.front().mineCount);
                    H += static_cast<ld>(dist->entries.back().mineCount);
                }
                // 精确参考（Exact 引擎）
                Distribution::DistPool pool;
                const Probability::Result exact = Exact::analyze(an.state(), basic, structure, pool);
                const ld tExact = exact.tCellProbability;
                ++n;
                const ld rhoG = gaussSolveRho(E, V, M, tSum);
                int it = 0, branch = 0;
                const ld rhoC = clampSolveRho(E, V, L, H, M, tSum, it, branch);
                ++br[branch - 1];
                itSum += it;
                itMax = (std::max)(itMax, static_cast<long long>(it));
                // A. clamp 求解器 vs 自身二分参考
                const ld refC = bisectClamp(E, V, L, H, M, tSum);
                check(std::abs(rhoC - refC) < 1e-9L, "A: clamp 求解器与二分一致");
                // B. 域
                check(rhoC >= 0.0L && rhoC <= 1.0L, "B: clamp 结果在 [0,1]");
                // 误差统计
                const ld eg = std::abs(rhoG - tExact);
                const ld ec = std::abs(rhoC - tExact);
                sumG += eg;
                sumC += ec;
                worstG = (std::max)(worstG, eg);
                worstC = (std::max)(worstC, ec);
                if (eg > 0.01L) ++gt1pG;
                if (ec > 0.01L) ++gt1pC;
                if (ec < eg) ++clampWins;
                if (eg < ec) ++gaussWins;
                // 组件总期望越界统计：gauss 用未钳值；clamp 的模型值恒钳在 [L,H]，
                // 越界（饱和段）即"模型被钳"——单列饱和局面误差对比。
                const ld thG = std::log(rhoG / (1.0L - rhoG));
                const ld eTot = E + V * thG;
                bool gOob = (eTot < L - 1e-9L || eTot > H + 1e-9L);
                if (gOob) ++oobGauss;
                const ld thC = std::log(rhoC / (1.0L - rhoC));
                const ld eTotC = E + V * thC;
                const bool cOob = (eTotC < L - 1e-9L || eTotC > H + 1e-9L);
                if (cOob) ++oobClamp;
                if (gOob || cOob) {
                    // 越界（或饱和）局面单独统计误差改善
                    ++oobN;
                    sumGOob += eg;
                    sumCOob += ec;
                    if (eg > 0.01L) ++gt1pGOob;
                    if (ec > 0.01L) ++gt1pCOob;
                }
                gaussDeltaSum += eg;
                clampDeltaSum += ec;
                // 走棋
                ld bestP = 2.0L; int bestX = 0, bestY = 0; bool clicked = false;
                for (int x = 1; x <= b.rows && !clicked; ++x)
                    for (int y = 1; y <= b.cols; ++y) {
                        if (gi.revealed[x][y] || gi.layout[x][y]) continue;
                        const ld p = Interactive::mineProbability(gc.analysis(), x, y);
                        if (p >= 1.0L) continue;
                        if (p == 0.0L) { gc.reveal(x, y); clicked = true; break; }
                        if (p < bestP) { bestP = p; bestX = x; bestY = y; }
                    }
                if (!clicked && bestX > 0) gc.reveal(bestX, bestY);
                else if (!clicked) break;
            }
        }
    }

    std::printf("局面 %lld：\n", n);
    std::printf("  平均 |gauss−精确| %.6f%%   最大 %.6f%%   >1%% 的 %lld\n",
                100.0 * (double)(sumG / n), 100.0 * (double)worstG, gt1pG);
    std::printf("  平均 |clamp−精确| %.6f%%   最大 %.6f%%   >1%% 的 %lld\n",
                100.0 * (double)(sumC / n), 100.0 * (double)worstC, gt1pC);
    std::printf("  组件总期望越界：gauss %lld (%.2f%%)，clamp %lld"
                "（饱和局面 %lld 例：gauss 平均 %.6f%%/最大含在内，clamp 平均 %.6f%%/含，>1%% 的 %lld → %lld）\n",
                oobGauss, 100.0 * (double)oobGauss / n, oobClamp, oobN,
                100.0 * (double)(oobN ? sumGOob / oobN : 0.0L),
                100.0 * (double)(oobN ? sumCOob / oobN : 0.0L), gt1pGOob, gt1pCOob);
    std::printf("  逐例胜出：clamp %lld (%.2f%%)，gauss %lld (%.2f%%)\n",
                clampWins, 100.0 * (double)clampWins / n, gaussWins,
                100.0 * (double)gaussWins / n);
    std::printf("  clamp 分支分布 [边界/解析, 下段, 上段, 中段]: %lld %lld %lld %lld"
                "（中段牛顿平均 %.3f 次，最大 %lld）\n",
                br[0], br[1], br[2], br[3],
                (double)((long double)itSum / (long double)(br[3] > 0 ? br[3] : 1)), itMax);
    // C. 断言：clamp 平均/最大误差严格优于 gauss
    check(sumC / n < sumG / n, "C: clamp 平均误差 < gauss 平均误差");
    check(worstC < worstG, "C: clamp 最大误差 < gauss 最大误差");
    // D. 断言：越界存在性 + 越界局面里 clamp 平均误差更优
    check(oobGauss > 0, "D: gauss 存在组件总期望越界局面");
    check(oobN > 0 && sumCOob / oobN < sumGOob / oobN,
          "D: 越界/饱和局面 clamp 平均误差 < gauss");
    std::printf("== %lld 项检查，失败 %lld 项 ==\n", gCheck, gFail);
    return gFail != 0;
}