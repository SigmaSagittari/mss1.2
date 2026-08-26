// struct_perf_test.cpp — 结构层性能剖析（供 VS 性能探查器采样）。
//
// 用法：在 VS 里以"性能探查器 → CPU 采样"运行本程序（Release x64），
// 采样结果重点看 Analysis::update 的调用树：
//   - Structure::Updater::update 内部热点：
//       * hashAt（重建块每格 8 邻域 splitmix64 哈希，见 structure.h）
//       * buildComponent（sort + lower_bound + 按 box 双循环 + intern + computeHash）
//       * collectComponent（DFS 收集连通格）
//   - Exact::analyze（块分布枚举 + 多项式卷积）
//
// 本程序跑 6 局 30x16/99 真实对局（最小概率策略，平局最左上），
// 热循环集中在 main → GameController::reveal → Analysis::update。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "ui/game_control.h"
#include "ui/interactive.h"

using namespace mss;
using ld = long double;

// 由 mss_1.2.cpp 的 main 调用（临时挂载），跑完 exit(0)。
void runStructPerfTest() {
    const int rows = 16, cols = 30, mines = 99;
    std::mt19937 rng(20260826);
    const auto t0 = std::chrono::steady_clock::now();
    long long moves = 0;
    int wins = 0, losses = 0;
    for (int g = 0; g < 1000; ++g) {
        GameController gc(rows, cols, mines, static_cast<unsigned>(rng()));
        while (gc.info().status == GameController::Status::Playing) {
            auto& an = gc.analysis();
            const auto& board = an.state();
            const auto& basic = an.basicMarks();
            ld minP = 2.0L;
            int bx = 0, by = 0;
            for (int x = 1; x <= rows; ++x)
                for (int y = 1; y <= cols; ++y) {
                    if (gc.info().revealed[x][y]) continue;
                    if (basic.marks[x][y] == Basic::Mark::Mine) continue;
                    const ld p = Interactive::mineProbability(an, x, y);
                    if (p < minP - 1e-12L) { minP = p; bx = x; by = y; }
                }
            if (bx == 0) break;
            gc.reveal(bx, by);
            ++moves;
        }
        if (gc.info().status == GameController::Status::Won) ++wins;
        else if (gc.info().status == GameController::Status::Lost) ++losses;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("1000 局 %lld 步，总耗时 %.2f s，%.1f μs/步；胜 %d 负 %d 胜率 %.1f%%\n",
                moves, sec, sec / static_cast<double>(moves) * 1e6, wins, losses,
                100.0 * static_cast<double>(wins) / 1000.0);
    std::exit(0);
}