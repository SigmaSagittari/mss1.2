// param_sweep.cpp — 参数扫描：s（log-生存尺度）/alpha（需求阻尼）× 分配集中度。
// 同一盘面（16x30/99 四角=1）grow 4096，统计：
//   头部动作子树占比 / score 分布 / getAnswer / 大数字展开。
// 目的：找让"头部几个候选拿更多节点"的参数组合。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "analysis/search/midgame_search.h"
#include "core/config.h"

using namespace mss;

struct Res {
    double s = 0, alpha = 0;
    long double top3SubPct = 0, top10SubPct = 0;
    long double scoreMax = 0, scoreMed = 0, scoreMin = 0;
    int ansX = 0, ansY = 0;
    long double ansValue = 0;
    int bigExp = 0, bigTot = 0;
    int nodes = 0;
};

static int subOf(const MidgameSearch::Tree& t, const MidgameSearch::Node& n, int ai) {
    int sub = 0;
    for (int k = 0; k <= 8; ++k) {
        const int c = MidgameSearch::branchOf(n, ai, k);
        if (c >= 0) sub += t.subtreeNodes[static_cast<std::size_t>(c)];
    }
    return sub;
}

static Res runOne(double s, double alpha) {
    Res r;
    r.s = s;
    r.alpha = alpha;
    constexpr int R = 16, C = 30, M = 99;
    ObservedBoard b(R, C, M);
    b.board[1][1] = Cell::Num1;
    b.board[1][C] = Cell::Num1;
    b.board[R][1] = Cell::Num1;
    b.board[R][C] = Cell::Num1;
    MidgameSearch::Session s2;
    if (!MidgameSearch::build(s2, b)) return r;
    s2.config.s = s;
    s2.config.alpha = alpha;
    MidgameSearch::grow(s2, 4096);
    const MidgameSearch::Tree& t = s2.tree;
    const MidgameSearch::Node& root = t.nodes[0];
    r.nodes = t.statsNodes;
    const MidgameSearch::Answer ans = MidgameSearch::getAnswer(t);
    r.ansX = ans.x;
    r.ansY = ans.y;
    r.ansValue = ans.value;

    const std::size_t na = root.actions.size();
    std::vector<int> sub(na);
    for (std::size_t i = 0; i < na; ++i) sub[i] = subOf(t, root, static_cast<int>(i));
    std::vector<int> o(na);
    for (std::size_t i = 0; i < na; ++i) o[i] = static_cast<int>(i);
    std::sort(o.begin(), o.end(), [&](int l, int r) { return sub[l] > sub[r]; });
    long double top3 = 0, top10 = 0;
    for (int i = 0; i < static_cast<int>(na); ++i) {
        if (i < 3) top3 += sub[o[static_cast<std::size_t>(i)]];
        if (i < 10) top10 += sub[o[static_cast<std::size_t>(i)]];
    }
    const int totalSub = t.statsNodes - 1;
    r.top3SubPct = totalSub > 0 ? top3 / totalSub : 0;
    r.top10SubPct = totalSub > 0 ? top10 / totalSub : 0;

    // score 分布。
    std::vector<long double> sc(root.score.begin(), root.score.end());
    std::sort(sc.begin(), sc.end(), std::greater<long double>());
    r.scoreMax = sc.empty() ? 0 : sc[0];
    r.scoreMed = sc.empty() ? 0 : sc[sc.size() / 2];
    r.scoreMin = sc.empty() ? 0 : sc.back();

    // 大数字展开（digit>=3 的动作级）统计。
    int bigTot = 0, bigExp = 0;
    for (std::size_t i = 0; i < na; ++i) {
        const MidgameSearch::Action& a = root.actions[i];
        const std::array<double, 9>* d = MidgameSearch::digitOf(root, a);
        for (int k = 3; k <= 8; ++k) {
            if (!d || (*d)[static_cast<std::size_t>(k)] <= 0.0) continue;
            ++bigTot;
            if (MidgameSearch::branchOf(root, static_cast<int>(i), k) >= 0) ++bigExp;
        }
    }
    r.bigTot = bigTot;
    r.bigExp = bigExp;
    return r;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("   s     alpha  top3%%  top10%%  scoreMax  scoreMed  getAnswer            bigExp/bigTot\n");
    const double ss[] = {0.02L, 0.05L, 0.1L};
    const double al[] = {0.5L};
    for (double s : ss)
        for (double a : al) {
            const Res r = runOne(s, a);
            std::printf("%5.2f  %5.2f  %5.1Lf  %6.1Lf  %.6Lf  %.6Lf  (%2d,%2d) v=%.4Lf  %d/%d\n",
                        r.s, r.alpha, r.top3SubPct * 100, r.top10SubPct * 100, r.scoreMax,
                        r.scoreMed, r.ansX, r.ansY, r.ansValue, r.bigExp, r.bigTot);
            std::fflush(stdout);
        }
    return 0;
}