// brute_timing_test.cpp — 临时：9x9 初级盘 candidates 在 [1K,200K] 时暴力求解耗时。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/search/midgame_search.h"
#include "ui/game_control.h"
#include "ui/interactive.h"

using namespace mss;
using Clock = std::chrono::steady_clock;

void runSearchTimingTest() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(20260827);
    int hit = 0, slow = 0;
    for (int g = 0; g < 300; ++g) {
        GameController gc(9, 9, 10, static_cast<unsigned>(rng()));
        for (int step = 0; step < 200 && gc.info().status == GameController::Status::Playing;
             ++step) {
            auto& an = gc.analysis();
            const auto& board = an.state();
            const auto& basic = an.basicMarks();
            long double minP = 2.0L;
            int bx = 0, by = 0;
            for (int x = 1; x <= board.rows; ++x)
                for (int y = 1; y <= board.cols; ++y) {
                    if (gc.info().revealed[x][y]) continue;
                    if (basic.marks[x][y] == Basic::Mark::Mine) continue;
                    const long double p = Interactive::mineProbability(an, x, y);
                    if (p < minP - 1e-12L) { minP = p; bx = x; by = y; }
                }
            if (bx == 0) break;
            gc.reveal(bx, by);
            const auto& b2 = an.state();
            MidgameSearch::Session s;
            if (MidgameSearch::build(s, b2)) {
                const long double c = s.prob.candidates;
                if (c >= 1000.0L && c <= 200000.0L) {
                    // 卡在暴力区间的盘面：计时暴力求解。
                    ++hit;
                    Grid<long double> grid(s.board.rows, s.board.cols, 0.0L);
                    for (int x = 1; x <= s.board.rows; ++x)
                        for (int y = 1; y <= s.board.cols; ++y)
                            grid[x][y] = s.prob.mineProbability(s.board.id(x, y), s.board,
                                                                s.basic, s.structure);
                    const auto t0 = Clock::now();
                    const EndgameBruteforce::Result r = EndgameBruteforce::solveEndgame(
                        s.board, s.basic, s.structure, s.pool, grid);
                    const auto t1 = Clock::now();
                    const double ms =
                        std::chrono::duration<double, std::milli>(t1 - t0).count();
                    if (ms > 100.0) ++slow;
                    std::printf("g%03d cand=%.0f total=%d nodes=%lld ms=%.0f\n", g, c,
                                r.totalPossibilities, r.nodes, ms);
                }
            }
        }
    }
    std::printf("hit=%d slow(>100ms)=%d\n", hit, slow);
    std::exit(0);
}