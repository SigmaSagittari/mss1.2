#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "ui/game_control.h"

using namespace mss;
using ld = long double;

static ld comb(int n, int k) {
    if (k < 0 || k > n) return 0.0L;
    return std::exp(std::lgamma((ld)n + 1) - std::lgamma((ld)k + 1) - std::lgamma((ld)(n - k) + 1));
}

static int gFail = 0;
static long long gCheck = 0;
static void check(bool ok, const char* what) {
    ++gCheck;
    if (!ok) { ++gFail; std::printf("  FAIL: %s\n", what); }
}

int main() {
    // ── A. 4x4/4 全隐藏：解析超几何对照 ──
    std::printf("== A. 4x4/4 全隐藏 ==\n");
    {
        ObservedBoard board(4, 4, 4);
        const Basic::Result basic = Basic::Analyzer::analyze(board);
        Structure::ShapePool shapes;
        const Structure::Result structure = Structure::Analyzer::analyze(board, basic, shapes);
        Distribution::DistPool pool;
        const Probability::Result prob = Exact::analyze(board, basic, structure, pool);
        // 角格 (1,1)：3 个 T 邻居，池子 12，M=4
        const auto r1 = Exact::observe(board, basic, structure, prob, pool, board.id(1, 1));
        ld sum = r1.explosion;
        for (int k = 0; k <= 8; ++k) {
            const ld expect = comb(3, k) * comb(12, 4 - k) / comb(16, 4);
            check(std::abs(r1.digit[k] - expect) < 1e-12L, "A: 角格 digit 解析值");
            sum += r1.digit[k];
        }
        check(std::abs(r1.explosion - 0.25L) < 1e-12L, "A: 角格 explosion=0.25");
        check(std::abs(sum - 1.0L) < 1e-12L, "A: 角格 explosion+Σdigit=1");
        // 中心格 (2,2)：8 个 T 邻居，池子 7，M=4
        const auto r2 = Exact::observe(board, basic, structure, prob, pool, board.id(2, 2));
        ld sum2 = r2.explosion;
        for (int k = 0; k <= 8; ++k) {
            const ld expect = comb(8, k) * comb(7, 4 - k) / comb(16, 4);
            check(std::abs(r2.digit[k] - expect) < 1e-12L, "A: 中心格 digit 解析值");
            sum2 += r2.digit[k];
        }
        check(std::abs(r2.explosion - 0.25L) < 1e-12L, "A: 中心格 explosion=0.25");
        check(std::abs(sum2 - 1.0L) < 1e-12L, "A: 中心格 explosion+Σdigit=1");
    }

    // ── B. 真实对局：每个隐藏格不变量 + explosion 与 analyze 一致 ──
    std::printf("== B. 真实对局（引擎驱动，逐格核对）==\n");
    std::mt19937_64 rng(7);
    int cells = 0;
    for (int trial = 0; trial < 30; ++trial) {
        GameController gc(9, 9, 10, static_cast<unsigned>(rng()));
        {
            const int pick = static_cast<int>(rng() % 81u);
            gc.reveal(pick / 9 + 1, pick % 9 + 1);
        }
        for (int step = 0; step < 20; ++step) {
            const auto& gi = gc.info();
            if (gi.status != GameController::Status::Playing) break;
            const auto& an = gc.analysis();
            // 全量精确结果（对照 explosion）
            Distribution::DistPool probe;
            const Probability::Result prob =
                Exact::analyze(an.state(), an.basicMarks(), an.structure(), probe);
            for (int x = 1; x <= 9; ++x)
                for (int y = 1; y <= 9; ++y) {
                    if (an.state().board[x][y] != Cell::Hidden) continue;
                    const auto r = Exact::observe(an.state(), an.basicMarks(),
                                                  an.structure(), prob, probe,
                                                  an.state().id(x, y));
                    ld sum = r.explosion;
                    for (int k = 0; k <= 8; ++k) sum += r.digit[k];
                    const ld mp = prob.mineProbability(an.state().id(x, y), an.state(),
                                                       an.basicMarks(), an.structure());
                    if (std::abs(sum - 1.0L) > 1e-9L || std::abs(r.explosion - mp) > 1e-9L) {
                        std::printf("  实例 t%02d s%02d (%d,%d): explosion=%.10Lg mp=%.10Lg sum=%.10Lg"
                                    " M=%d tSum=%d\n",
                                    trial, step, x, y, r.explosion, mp, sum,
                                    an.basicMarks().mineSum, an.basicMarks().unknownSum);
                        // 邻域 dump：值 / 标记 / 组件
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -1; dy <= 1; ++dy) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 1 || nx > 9 || ny < 1 || ny > 9) continue;
                                const auto loc = an.structure().cellLoc[an.state().id(nx, ny)];
                                std::printf("    (%d,%d) board=%d mark=%d comp=%d box=%d\n",
                                            nx, ny, (int)an.state().board[nx][ny],
                                            (int)an.basicMarks().marks[nx][ny],
                                            loc.component, loc.box);
                            }
                        std::exit(1);
                    }
                    check(std::abs(sum - 1.0L) < 1e-9L, "B: explosion+Σdigit=1");
                    check(std::abs(r.explosion - mp) < 1e-9L, "B: explosion=雷概率");
                    ++cells;
                }
            // 推进（随机点开）
            std::vector<std::pair<int, int>> cand;
            for (int x = 1; x <= 9; ++x)
                for (int y = 1; y <= 9; ++y)
                    if (!gi.revealed[x][y] && !gi.layout[x][y]) cand.emplace_back(x, y);
            if (cand.empty()) break;
            const auto [x, y] = cand[static_cast<std::size_t>(rng() % cand.size())];
            gc.reveal(x, y);
        }
    }
    std::printf("  核对 %d 格\n", cells);
    std::printf("\n共 %lld 项检查，失败 %d 项\n", gCheck, gFail);
    return gFail == 0 ? 0 : 1;
}