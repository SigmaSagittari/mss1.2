// ─────────────────────────────────────────────────────────────
// test/solve_rho_test.cpp — Approx::solveRho（theta 版）测试。
//
// 编译（g++，项目根执行）：
//   D:\codeenvset\mingw64\bin\g++.exe -std=c++20 -O2 -Isrc ^
//       src\test\solve_rho_test.cpp -o build\solve_rho_test.exe
//   build\solve_rho_test.exe
//
// 覆盖：
//   A. 已知值：4x4/4、9x9/10 全隐藏盘面 rho 精确值（真实管线）。
//   B. 真实对局全管线：三种盘面用 Approx 引擎驱动走棋（点 0% 安全格，
//      没有点最低 p，p≥1 跳过），每步验证 rho 与 θ 空间二分真根一致、
//      内部残差≈0。
//   C. 合成配置扫描：目标式随机 + 12 档边界目标 + Vbar=0 钳位 + tSum=0。
//
// 注意：
//   - 机器边界（rho 四舍五入成精确 0/1）是 long double 表示限制，不是
//     求解器错误——此时与 θ 空间二分结果必然一致，跳过残差检查。
//   - 残差检查只在 (1e-6, 1-1e-6) 带内有效：更靠近边界时 1-rho（或 rho）
//     的相对精度只有 ~1e-13..1e-8，logit 误差传导使残差失真到 ~1e-8。
// ─────────────────────────────────────────────────────────────
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/approx.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "ui/game_control.h"
#include "ui/interactive.h"

using namespace mss;
using ld = long double;

static int gFail = 0;
static long long gCheck = 0;

static ld sig(ld theta) { return 1.0L / (1.0L + std::exp(-theta)); }

static ld F(ld Ebar, ld Vbar, ld M, ld tSum, ld rho) {
    return Ebar + Vbar * std::log(rho / (1.0L - rho)) + tSum * rho - M;
}

// θ 空间二分：对机器边界（rho→0/1）稳健，θ ∈ [-1e5, 1e5]
static ld bisectTheta(ld Ebar, ld Vbar, ld M, ld tSum) {
    ld lo = -100000.0L, hi = 100000.0L;
    for (int i = 0; i < 300; ++i) {
        const ld mid = (lo + hi) / 2.0L;
        if (Ebar + Vbar * mid + tSum * sig(mid) - M < 0) lo = mid;
        else hi = mid;
    }
    return (lo + hi) / 2.0L;
}

static void check(bool ok, const char* what) {
    ++gCheck;
    if (!ok) {
        ++gFail;
        std::printf("  FAIL: %s\n", what);
    }
}

// 通用核对：θ 空间二分一致 + 内部残差
static void verifySolve(ld Ebar, ld Vbar, ld M, ld tSum, ld rho, int iters, const char* tag) {
    const ld thetaRef = bisectTheta(Ebar, Vbar, M, tSum);
    const ld rhoRef = sig(thetaRef);
    const bool ok1 = rho >= 0.0L && rho <= 1.0L;
    const bool ok2 = std::abs(rho - rhoRef) < 1e-12L;
    // 残差检查只在 (1e-6, 1-1e-6) 带内有效：更靠近边界时 1-rho（或 rho）
    // 的相对精度只有 ~1e-13..1e-8，logit 误差传导使残差失真到 ~1e-8。
    const bool interior = rho > 1e-6L && rho < 1.0L - 1e-6L;
    const ld res = interior ? std::abs(F(Ebar, Vbar, M, tSum, rho)) : 0.0L;
    const bool ok3 = !interior || res < 1e-9L;
    const bool ok4 = iters <= 0 || iters <= 10;
    ++gCheck;
    if (ok1 && ok2 && ok3 && ok4) return;
    ++gFail;
    std::printf("  FAIL: %s Ebar=%.6Lg Vbar=%.6Lg M=%.6Lg tSum=%.6Lg\n"
                "        rho=%.18Lg thetaRef=%.6Lg rhoRef=%.18Lg diff=%.3Lg res=%.3Lg iters=%d [%d%d%d%d]\n",
                tag, Ebar, Vbar, M, tSum, rho, thetaRef, rhoRef,
                std::abs(rho - rhoRef), res, iters, ok1, ok2, ok3, ok4);
}

// ── A. 已知值（走真实 Approx 管线）──
static void testKnown() {
    std::printf("== A. 已知值 ==\n");
    {
        ObservedBoard board(4, 4, 4);  // 4x4/4 全隐藏：rho = 4/16 = 0.25
        const Basic::Result basic = Basic::Analyzer::analyze(board);
        Structure::ShapePool shapes;
        const Structure::Result structure = Structure::Analyzer::analyze(board, basic, shapes);
        Distribution::DistPool dists;
        const Approx::Result r = Approx::Analyzer::analyze(board, basic, structure, dists);
        check(std::abs(r.rho - 0.25L) < 1e-12L, "A: 4x4/4 rho=0.25");
        check(std::abs(F(r.Ebar, r.Vbar, 4, 16, r.rho)) < 1e-12L, "A: 4x4/4 残差");
    }
    {
        ObservedBoard board(9, 9, 10);  // 9x9/10 全隐藏：rho = 10/81
        const Basic::Result basic = Basic::Analyzer::analyze(board);
        Structure::ShapePool shapes;
        const Structure::Result structure = Structure::Analyzer::analyze(board, basic, shapes);
        Distribution::DistPool dists;
        const Approx::Result r = Approx::Analyzer::analyze(board, basic, structure, dists);
        check(std::abs(r.rho - 10.0L / 81.0L) < 1e-12L, "A: 9x9/10 rho=10/81");
        check(std::abs(F(r.Ebar, r.Vbar, 10, 81, r.rho)) < 1e-12L, "A: 9x9/10 残差");
    }
}

// ── B. 真实对局全管线（用 Approx 引擎驱动：点 0% 安全格，没有点最低）──
static void testPipeline() {
    std::printf("== B. 真实对局管线（引擎驱动走棋，每步核对）==\n");
    std::mt19937_64 rng(2026);
    struct BC { int rows, cols, mines; const char* name; };
    const std::vector<BC> boards = {
        {9, 9, 10, "9x9/10"},
        {16, 16, 40, "16x16/40"},
        {30, 16, 99, "30x16/99"},
    };
    long long pos = 0, moves = 0, wins = 0;
    int vbar0 = 0, vbarPos = 0;
    ld maxRes = 0.0L;
    for (const auto& b : boards) {
        const int total = b.rows * b.cols;
        for (int trial = 0; trial < 40; ++trial) {
            GameController gc(b.rows, b.cols, b.mines, static_cast<unsigned>(rng()));
            {  // 首步：随机格（真实玩家开局任意）
                const int pick = static_cast<int>(rng() % (unsigned long long)total);
                gc.reveal(pick / b.cols + 1, pick % b.cols + 1);
            }
            for (int step = 0; step < 200; ++step) {
                const auto& gi = gc.info();
                if (gi.status != GameController::Status::Playing) {
                    if (gi.status == GameController::Status::Won) ++wins;
                    break;
                }
                const auto& an = gc.analysis();
                const auto& app = an.approx();
                const auto& basic = an.basicMarks();
                const ld M = static_cast<ld>(gi.mines - basic.mineSum);
                const ld tSum = static_cast<ld>(basic.unknownSum);
                const ld rho = app.rho;
                ++pos;
                if (app.Vbar < 1e-10L) {
                    ++vbar0;
                    // 解析分支核对
                    const ld expect = tSum > 0.0L ? (M - app.Ebar) / tSum : 0.0L;
                    const ld clampE = expect < 0.0L ? 0.0L : (expect > 1.0L ? 1.0L : expect);
                    check(std::abs(rho - clampE) < 1e-12L, "B: 解析分支值");
                } else {
                    ++vbarPos;
                    verifySolve(app.Ebar, app.Vbar, M, tSum, rho, 0, "B: Vbar>0 核对");
                    const ld res =
                        rho > 1e-6L && rho < 1.0L - 1e-6L
                            ? std::abs(F(app.Ebar, app.Vbar, M, tSum, rho))
                            : 0.0L;
                    if (res > maxRes) maxRes = res;
                }
                // 引擎驱动走棋：点 p==0 的安全格；没有 0 点最低 p；p≥1 跳过（视作必雷）
                ld bestP = 2.0L;
                int bestX = 0, bestY = 0;
                bool clicked = false;
                for (int x = 1; x <= b.rows && !clicked; ++x)
                    for (int y = 1; y <= b.cols; ++y) {
                        if (gi.revealed[x][y] || gi.layout[x][y]) continue;
                        const ld p = Interactive::mineProbability(gc.analysis(), x, y);
                        if (p >= 1.0L) continue;
                        if (p == 0.0L) {
                            gc.reveal(x, y);
                            clicked = true;
                            ++moves;
                            break;
                        }
                        if (p < bestP) {
                            bestP = p;
                            bestX = x;
                            bestY = y;
                        }
                    }
                if (!clicked && bestX > 0) {
                    gc.reveal(bestX, bestY);
                    ++moves;
                } else if (!clicked) {
                    break;  // 无可下格（全 p≥1）：模型认为只剩必雷，停
                }
            }
        }
    }
    std::printf("  局位 %lld 个（解析: %d，牛顿: %d），走棋 %lld 步，胜利 %lld 局，最大内部残差 %.3g\n",
                pos, vbar0, vbarPos, moves, wins, (double)maxRes);
}

// ── C. 合成配置扫描（算法副本，与线上 solveRho 逐字相同）──
static ld solveRhoCopy(ld Ebar, ld Vbar, ld M, ld tSum, int& iters) {
    if (Vbar < 1e-10L) {  // 与线上一致：噪声级 Vbar 按 0 处理
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
    int it = 0;
    for (; it < 10; ++it) {
        const ld s = sig(theta);
        const ld f = Ebar + Vbar * theta + tSum * s - M;
        const ld dF = Vbar + tSum * s * (1.0L - s);
        const ld step = f / dF;
        theta -= step;
        if (std::abs(step) < 1e-15L) break;
    }
    iters = it + 1;
    return sig(theta);
}

static void testSweep() {
    std::printf("== C. 合成配置扫描 ==\n");
    std::mt19937_64 rng(99);
    int worst = 0;

    // C1: 目标式随机 3000 组（真实量级参数，rho* 均匀覆盖全域）
    for (int i = 0; i < 3000; ++i) {
        const ld Ebar = (rng() % 6001u) / 100.0L;        // 0..60
        const ld Vbar = ((rng() % 5000u) + 1) / 1000.0L; // 0.001..5
        const ld tSum = (rng() % 6001u) / 10.0L;         // 0..600
        const ld rhoStar = (rng() % 999999998u + 1) / 1e9L;  // 1e-9..1-1e-9
        const ld M = Ebar + Vbar * std::log(rhoStar / (1.0L - rhoStar)) + tSum * rhoStar;
        int it = 0;
        const ld rho = solveRhoCopy(Ebar, Vbar, M, tSum, it);
        worst = (std::max)(worst, it);
        check(std::abs(rho - rhoStar) < 1e-9L, "C1: 目标命中");
        check(std::abs(F(Ebar, Vbar, M, tSum, rho)) < 1e-9L, "C1: 残差");
    }

    // C2: 边界目标（极低/极高密度）
    {
        const ld targets[] = {1e-9L, 1e-6L, 1e-4L, 0.001L, 0.01L, 0.05L,
                              0.3L, 0.5L, 0.95L, 0.99L, 0.9999L, 1.0L - 1e-9L};
        const ld ebs[] = {0.1L, 2.0L, 20.0L};
        const ld vbs[] = {0.001L, 0.05L, 1.0L};
        const ld ts[] = {0.0L, 4.0L, 50.0L};
        for (ld rhoStar : targets)
            for (ld eb : ebs)
                for (ld vb : vbs)
                    for (ld ts : ts) {
                        const ld M = eb + vb * std::log(rhoStar / (1.0L - rhoStar)) + ts * rhoStar;
                        int it = 0;
                        const ld rho = solveRhoCopy(eb, vb, M, ts, it);
                        check(std::abs(rho - rhoStar) < 1e-9L, "C2: 边界目标命中");
                    }
    }

    // C3: Vbar=0 解析钳位（expect = clamp((M-Ebar)/tSum, 0, 1)）
    {
        const ld xs[] = {-5.0L, -0.1L, 0.0L, 0.3L, 1.0L, 1.5L, 10.0L};
        for (ld x : xs) {
            int it = 0;
            const ld rho = solveRhoCopy(2.0L, 0.0L, x + 2.0L, 10.0L, it);
            const ld expect = x < 0.0L ? 0.0L : (x > 10.0L ? 1.0L : x / 10.0L);
            check(std::abs(rho - expect) < 1e-15L, "C3: Vbar=0 钳位");
        }
        // tSum=0 且 Vbar=0：返回 0
        {
            int it = 0;
            const ld rho = solveRhoCopy(3.0L, 0.0L, 5.0L, 0.0L, it);
            check(rho == 0.0L, "C3: tSum=0 Vbar=0 → 0");
        }
    }

    // C4: tSum=0（Vbar>0）：F 对 θ 仿射，一次命中；用 θ 空间二分核对
    {
        int worstT0 = 0;
        for (int i = 0; i < 500; ++i) {
            const ld Ebar = (rng() % 5001u) / 100.0L;
            const ld Vbar = ((rng() % 5000u) + 1) / 1000.0L;
            const ld M = (rng() % 10001u) / 100.0L;
            int it = 0;
            const ld rho = solveRhoCopy(Ebar, Vbar, M, 0.0L, it);
            worstT0 = (std::max)(worstT0, it);
            verifySolve(Ebar, Vbar, M, 0.0L, rho, it, "C4: tSum=0 核对");
        }
        std::printf("  tSum=0 最坏迭代 %d\n", worstT0);
    }

    std::printf("  目标式扫描最坏迭代 %d\n", worst);
}

int main() {
    testKnown();
    testPipeline();
    testSweep();
    std::printf("\n共 %lld 项检查，失败 %d 项\n", gCheck, gFail);
    return gFail == 0 ? 0 : 1;
}