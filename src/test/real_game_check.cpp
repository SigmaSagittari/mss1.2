// real_game_check.cpp — 真实对局锐度：随机高级局推进到中盘，build + grow 4096，
// 看 score 分布锐度（max/分位）与头部节点集中度。
#include <algorithm>
#include <cstdio>
#include <vector>

#include "analysis/search/midgame_search.h"
#include "core/config.h"
#include "ui/game_control.h"
#include "ui/interactive.h"

using namespace mss;

static int subOf(const MidgameSearch::Tree& t, const MidgameSearch::Node& n, int ai) {
    int sub = 0;
    for (int k = 0; k <= 8; ++k) {
        const int c = MidgameSearch::branchOf(n, ai, k);
        if (c >= 0) sub += t.subtreeNodes[static_cast<std::size_t>(c)];
    }
    return sub;
}

static void runOne(unsigned seed) {
    GameController gc(30, 16, 99, seed);
    gc.reveal(8, 15);
    for (int step = 0; step < 40; ++step) {
        if (gc.info().status != GameController::Status::Playing) break;
        auto& an = gc.analysis();
        const ObservedBoard& board = an.state();
        const auto& basic = an.basicMarks();
        long double minP = 2.0L;
        int bx = 0, by = 0;
        for (int x = 1; x <= board.rows; ++x)
            for (int y = 1; y <= board.cols; ++y) {
                if (gc.info().revealed[x][y]) continue;
                if (basic.marks[x][y] == Basic::Mark::Mine) continue;
                const long double p = Interactive::mineProbability(an, x, y);
                if (p < minP - 1e-12L) {
                    minP = p;
                    bx = x;
                    by = y;
                }
            }
        if (bx == 0) break;
        gc.reveal(bx, by);
    }
    MidgameSearch::Session s;
    if (!MidgameSearch::build(s, gc.analysis().state())) {
        std::printf("seed %u: build failed: %s\n", seed, s.reason.c_str());
        return;
    }
    MidgameSearch::grow(s, 4096);
    const MidgameSearch::Tree& t = s.tree;
    const MidgameSearch::Node& root = t.nodes[0];
    const MidgameSearch::Answer ans = MidgameSearch::getAnswer(t);
    const std::size_t na = root.actions.size();

    std::vector<long double> sc(root.score.begin(), root.score.end());
    std::sort(sc.begin(), sc.end(), std::greater<long double>());
    const long double pMin = sc.empty() ? 0 : sc.back();

    std::vector<int> sub(na);
    for (std::size_t i = 0; i < na; ++i) sub[i] = subOf(t, root, static_cast<int>(i));
    std::vector<int> o(na);
    for (std::size_t i = 0; i < na; ++i) o[i] = static_cast<int>(i);
    std::sort(o.begin(), o.end(), [&](int l, int r) { return sub[l] > sub[r]; });
    long double top3 = 0, top10 = 0;
    const int totalSub = t.statsNodes - 1;
    for (int i = 0; i < static_cast<int>(na); ++i) {
        if (i < 3) top3 += sub[o[static_cast<std::size_t>(i)]];
        if (i < 10) top10 += sub[o[static_cast<std::size_t>(i)]];
    }
    std::size_t p10 = std::min<std::size_t>(9, sc.size() - 1);
    std::size_t p50 = sc.size() / 2;
    std::printf(
        "seed %u: cand=%zu getAnswer=(%d,%d) v=%Lf nodes=%d observes=%d\n"
        "  score: max=%Le p10=%Le med=%Le min>0=%Le 锐度max/med=%.1fx max/p10=%.1fx\n"
        "  sub: top3=%.1f%% top10=%.1f%%  top3cells=(%d,%d)(%d,%d)(%d,%d)\n",
        seed, na, ans.x, ans.y, ans.value, t.statsNodes, t.statsObserves, sc[0],
        sc[p10], sc[p50], pMin,
        sc[p50] > 0 ? static_cast<double>(sc[0] / sc[p50]) : 0.0,
        sc[p10] > 0 ? static_cast<double>(sc[0] / sc[p10]) : 0.0,
        totalSub > 0 ? 100.0L * top3 / totalSub : 0,
        totalSub > 0 ? 100.0L * top10 / totalSub : 0,
        root.actions[o[0]].cell / (t.cols + 1), root.actions[o[0]].cell % (t.cols + 1),
        root.actions[o[1]].cell / (t.cols + 1), root.actions[o[1]].cell % (t.cols + 1),
        root.actions[o[2]].cell / (t.cols + 1), root.actions[o[2]].cell % (t.cols + 1));
    std::fflush(stdout);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const unsigned seeds[] = {20260101u, 20260202u, 20260303u, 20260404u};
    for (unsigned seed : seeds) runOne(seed);
    return 0;
}