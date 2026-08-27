// verify_mult.cpp — 验证非前沿惩罚 mult 的实际作用：对比各 bin 的 score/sub。
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
    if (!MidgameSearch::build(s, b)) return 1;
    MidgameSearch::grow(s, 4096);
    const MidgameSearch::Tree& t = s.tree;
    const MidgameSearch::Node& root = t.nodes[0];

    // 组：frontier / bin0..3。每组：动作数、平均 mult、平均 score、平均 sub。
    struct G {
        int n = 0;
        double multSum = 0, scoreSum = 0, subSum = 0;
    };
    G g[5];  // [0]=frontier, [1..4]=bin0..3
    for (std::size_t i = 0; i < root.actions.size(); ++i) {
        const MidgameSearch::Action& a = root.actions[i];
        const auto [x, y] = std::pair<int, int>{a.cell / (t.cols + 1), a.cell % (t.cols + 1)};
        using Mark = Basic::Mark;
        const bool fr = s.basic.marks[x][y] == Mark::Frontier || s.basic.marks[x][y] == Mark::Safe;
        int bin = 0;
        if (!fr) {
            int unknown = 0;
            forEachAdjacent(x, y, R, C, [&](int nx, int ny) {
                if (s.basic.marks[nx][ny] == Mark::Unknown) ++unknown;
            });
            bin = unknown <= 2 ? 0 : (unknown == 3 ? 1 : (unknown == 4 ? 2 : 3));
        }
        G& gd = g[fr ? 0 : bin + 1];
        gd.n += 1;
        gd.multSum += a.mult;
        gd.scoreSum += root.score[i];  // 已含 /mult
        gd.subSum += subOf(t, root, static_cast<int>(i));
    }
    const char* name[5] = {"frontier", "bin0(<=2U)", "bin1(3U)", "bin2(4U)", "bin3(>4U)"};
    const int totalSub = t.statsNodes - 1;
    std::printf("group        n   avgMult  avgScore  avgSub  subShare\n");
    for (int gid = 0; gid < 5; ++gid) {
        const G& gd = g[gid];
        if (gd.n == 0) continue;
        std::printf("%-12s %3d  %6.3f  %7.5f  %6.1f  %5.1f%%\n", name[gid], gd.n,
                    gd.multSum / gd.n, gd.scoreSum / gd.n, gd.subSum / gd.n,
                    totalSub > 0 ? 100.0 * gd.subSum / totalSub : 0.0);
    }
    return 0;
}