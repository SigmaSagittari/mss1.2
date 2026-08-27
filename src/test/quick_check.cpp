// quick_check.cpp — 单次快速验证（替代慢的 sweep）：默认参数跑 4096 节点，
// 输出 getAnswer / 头部节点集中度 / 大数字展开 / score 分布。
#include <algorithm>
#include <cstdio>
#include <vector>

#include "analysis/search/midgame_search.h"
#include "core/config.h"

using namespace mss;

static int subOf(const MidgameSearch::Tree& t, const MidgameSearch::Node& n, int ai) {
    int sub = 0;
    for (int k = 0; k <= 8; ++k) {
        const int c = MidgameSearch::branchOf(n, ai, k);
        if (c >= 0) sub += t.subtreeNodes[static_cast<std::size_t>(c)];
    }
    return sub;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr int R = 16, C = 30, M = 99;
    ObservedBoard b(R, C, M);
    b.board[1][1] = Cell::Num1;
    b.board[1][C] = Cell::Num1;
    b.board[R][1] = Cell::Num1;
    b.board[R][C] = Cell::Num1;
    MidgameSearch::Session s;
    if (!MidgameSearch::build(s, b)) {
        std::printf("build failed: %s\n", s.reason.c_str());
        return 1;
    }
    MidgameSearch::grow(s, 4096);
    const MidgameSearch::Tree& t = s.tree;
    const MidgameSearch::Node& root = t.nodes[0];
    const MidgameSearch::Answer ans = MidgameSearch::getAnswer(t);
    std::printf("nodes=%d observes=%d | getAnswer=(%d,%d) v=%Lf | scoreSum=%Lf tLocal=%Lf\n",
                t.statsNodes, t.statsObserves, ans.x, ans.y, ans.value, root.scoreSum,
                root.tLocal);

    const std::size_t na = root.actions.size();
    std::vector<int> sub(na);
    for (std::size_t i = 0; i < na; ++i) sub[i] = subOf(t, root, static_cast<int>(i));
    std::vector<int> o(na);
    for (std::size_t i = 0; i < na; ++i) o[i] = static_cast<int>(i);
    std::sort(o.begin(), o.end(), [&](int l, int r) { return sub[l] > sub[r]; });
    long double top3 = 0, top10 = 0, top50 = 0;
    const int totalSub = t.statsNodes - 1;
    for (int i = 0; i < static_cast<int>(na); ++i) {
        if (i < 3) top3 += sub[o[static_cast<std::size_t>(i)]];
        if (i < 10) top10 += sub[o[static_cast<std::size_t>(i)]];
        if (i < 50) top50 += sub[o[static_cast<std::size_t>(i)]];
    }
    std::printf("sub distribution: top3=%.1f%% top10=%.1f%% top50=%.1f%% zero-sub actions=%d\n",
                totalSub > 0 ? 100.0L * top3 / totalSub : 0,
                totalSub > 0 ? 100.0L * top10 / totalSub : 0,
                totalSub > 0 ? 100.0L * top50 / totalSub : 0, 0);

    std::vector<long double> sc(root.score.begin(), root.score.end());
    int zeroSub = 0;
    for (std::size_t i = 0; i < na; ++i) if (sub[i] == 0) ++zeroSub;
    std::sort(sc.begin(), sc.end(), std::greater<long double>());
    std::printf("score: max=%Lf top10=%Lf med=%Lf min>0=%d | zeroSub=%d\n", sc[0],
                sc[std::min<std::size_t>(9, sc.size() - 1)], sc[sc.size() / 2],
                sc[0] > 0 ? 1 : 0, zeroSub);

    int bigTot = 0, bigExp = 0;
    for (std::size_t i = 0; i < na; ++i) {
        const std::array<double, 9>* d = MidgameSearch::digitOf(root, root.actions[i]);
        for (int k = 3; k <= 8; ++k) {
            if (!d || (*d)[static_cast<std::size_t>(k)] <= 0.0) continue;
            ++bigTot;
            if (MidgameSearch::branchOf(root, static_cast<int>(i), k) >= 0) ++bigExp;
        }
    }
    std::printf("bigDigit(>=3): expanded=%d/%d\n", bigExp, bigTot);
    return 0;
}