// diag_search.cpp — 诊断：①高预算动作为何不展开大数字分支 ②unexpanded 价值近似的偏差
//                    ③“招法→局面”与“局面→招法”转移公式是否混用。
//
// 复现盘面（用户提供）：30x16/99，四个角翻开为 1，其余全 Hidden。
// 复刻求值全部改用 public 只读接口 branchOf/digitOf（稀疏分支 + 懒 digit）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "analysis/search/midgame_search.h"
#include "core/config.h"

using namespace mss;

// ── 复刻求值（读 public 字段 + branchOf/digitOf）──
static long double childValR(const MidgameSearch::Tree& t, const MidgameSearch::Node& n,
                             int ai, int k) {
    const int c = MidgameSearch::branchOf(n, ai, k);
    const long double p = n.actions[static_cast<std::size_t>(ai)].p;
    if (c == MidgameSearch::kDominated) return 1.0L;
    if (c == MidgameSearch::kUnexp) return 1.0L - (1.0L - n.dacc) * (1.0L - p);
    return t.nodes[static_cast<std::size_t>(c)].value;
}
static long double childTR(const MidgameSearch::Tree& t, const MidgameSearch::Node& n, int ai,
                           int k) {
    const int c = MidgameSearch::branchOf(n, ai, k);
    if (c == MidgameSearch::kDominated) return 0.0L;
    if (c == MidgameSearch::kUnexp) return 1.0L;
    return t.nodes[static_cast<std::size_t>(c)].t;
}
static long double childCR(const MidgameSearch::Tree& t, const MidgameSearch::Node& n, int ai,
                           int k) {
    const int c = MidgameSearch::branchOf(n, ai, k);
    if (c == MidgameSearch::kDominated) return 0.0L;
    if (c == MidgameSearch::kUnexp) return 1.0L;
    return t.nodes[static_cast<std::size_t>(c)].C;
}
static long double actionValR(const MidgameSearch::Tree& t, const MidgameSearch::Node& n,
                              const MidgameSearch::Action& a, int ai) {
    const std::array<double, 9>* d = MidgameSearch::digitOf(n, a);
    if (!d) {
        const long double L = 1.0L - (1.0L - n.dacc) * (1.0L - a.p);
        return a.p + (1.0L - a.p) * L;
    }
    long double v = a.p;
    for (int k = 0; k <= 8; ++k)
        if ((*d)[static_cast<std::size_t>(k)] > 0.0L)
            v += (*d)[static_cast<std::size_t>(k)] * childValR(t, n, ai, k);
    return v;
}
static long double valueDepthR(const MidgameSearch::Tree& t, int nodeId, int d);
static long double actionValDepthR(const MidgameSearch::Tree& t, const MidgameSearch::Node& n,
                                   const MidgameSearch::Action& a, int ai, int d) {
    const std::array<double, 9>* dg = MidgameSearch::digitOf(n, a);
    if (!dg) {
        const long double L = 1.0L - (1.0L - n.dacc) * (1.0L - a.p);
        return a.p + (1.0L - a.p) * L;
    }
    long double v = a.p;
    for (int k = 0; k <= 8; ++k) {
        if ((*dg)[static_cast<std::size_t>(k)] <= 0.0L) continue;
        const int c = MidgameSearch::branchOf(n, ai, k);
        long double cv;
        if (c == MidgameSearch::kDominated)
            cv = 1.0L;
        else if (c == MidgameSearch::kUnexp)
            cv = 1.0L - (1.0L - n.dacc) * (1.0L - a.p);
        else
            cv = valueDepthR(t, c, d);
        v += (*dg)[static_cast<std::size_t>(k)] * cv;
    }
    return v;
}
static long double valueDepthR(const MidgameSearch::Tree& t, int nodeId, int d) {
    const MidgameSearch::Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    if (n.depth >= d || !n.expanded) return n.dacc;
    long double best = 1e30L;
    for (std::size_t i = 0; i < n.actions.size(); ++i)
        best = std::min(best, actionValDepthR(t, n, n.actions[i], static_cast<int>(i), d));
    return best;
}
static int subtreeMaxR(const MidgameSearch::Tree& t, int nodeId) {
    const MidgameSearch::Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    int m = n.depth;
    for (std::size_t i = 0; i < n.actions.size(); ++i)
        for (int k = 0; k <= 8; ++k)
            if (MidgameSearch::branchOf(n, static_cast<int>(i), k) >= 0)
                m = std::max(m, subtreeMaxR(t, MidgameSearch::branchOf(n, static_cast<int>(i), k)));
    return m;
}

// ── binOf 复刻（读 basic.marks）──
static int binOfR(const ObservedBoard& board, const Basic::Result& basic, int x, int y) {
    using Mark = Basic::Mark;
    int unknown = 0;
    forEachAdjacent(x, y, board.rows, board.cols, [&](int nx, int ny) {
        if (basic.marks[nx][ny] == Mark::Unknown) ++unknown;
    });
    if (unknown <= 2) return 0;
    if (unknown == 3) return 1;
    if (unknown == 4) return 2;
    return 3;
}

// 注意：MidgameSearch::Action 无 i 字段，这里用局部包装做下标传递；直接给复刻函数传 ai。
// 以上复刻中 a.i 的用法仅出现在 actionValR/actionValDepthR（它们拿的是拷贝）。
// 为保持简单，统一改为显式 ai 参数（见下方调用处）。

struct Row {
    int i = 0, x = 0, y = 0, bin = -1;
    bool frontier = false;
    long double p = 0, mult = 1, r = 0, score = 0, cO = 0, tO = 0;
    int sub = 0;          // 该动作已展开子分支的子树节点总和
    int maxExp = -1;      // 已展开的最大 digit
    int bigUnexp = 0;     // digit>=3 且未展开的分支数
    int bigDom = 0;       // digit>=3 且被支配的分支数
    long double vD = 0;   // d_min 截断价值（复刻）
    long double vLeaf = 0;  // 全未展开叶子近似
    long double vFull = 0;  // 当前树价值（复刻，无截断）
};

static void dump(const MidgameSearch::Session& s, const char* tag) {
    const MidgameSearch::Tree& t = s.tree;
    const MidgameSearch::Node& root = t.nodes[0];
    if (!root.expanded) return;
    int dMin = 1000000000;
    for (std::size_t i = 0; i < root.actions.size(); ++i) {
        const MidgameSearch::Action& a = root.actions[i];
        const int ai = static_cast<int>(i);
        // actionReachR 需要带 ai 的 Accessor：改用内联计算
        int reach = root.depth;
        for (int k = 0; k <= 8; ++k)
            if (MidgameSearch::branchOf(root, ai, k) >= 0)
                reach = std::max(reach, subtreeMaxR(t, MidgameSearch::branchOf(root, ai, k)));
        dMin = std::min(dMin, reach);
        (void)a;
    }
    if (dMin == 1000000000) dMin = 0;
    const MidgameSearch::Answer ans = MidgameSearch::getAnswer(t);
    std::printf(
        "\n========== %s | nodes=%d observes=%d | dMin=%d | scoreSum=%Lf tLocal=%Lf "
        "| getAnswer: (%d,%d) value=%Lf ==========\n",
        tag, t.statsNodes, t.statsObserves, dMin, root.scoreSum, root.tLocal, ans.x, ans.y,
        ans.value);

    std::vector<Row> rows;
    rows.reserve(root.actions.size());
    for (std::size_t i = 0; i < root.actions.size(); ++i) {
        const MidgameSearch::Action& a = root.actions[i];
        const int ai = static_cast<int>(i);
        Row r;
        r.i = ai;
        r.x = a.cell / (t.cols + 1);
        r.y = a.cell % (t.cols + 1);
        r.p = static_cast<long double>(a.p);
        r.mult = static_cast<long double>(a.mult);
        r.r = root.r[i];
        r.score = root.score[i];
        r.bin = binOfR(s.board, s.basic, r.x, r.y);
        r.frontier = s.basic.marks[r.x][r.y] == Basic::Mark::Frontier ||
                     s.basic.marks[r.x][r.y] == Basic::Mark::Safe;
        const std::array<double, 9>* d = MidgameSearch::digitOf(root, a);
        if (!d) {
            r.cO = 1.0L - static_cast<long double>(a.p);
            r.tO = 1.0L - static_cast<long double>(a.p);
        } else {
            for (int k = 0; k <= 8; ++k) {
                if ((*d)[static_cast<std::size_t>(k)] <= 0.0L) continue;
                r.cO += (*d)[static_cast<std::size_t>(k)] * childCR(t, root, ai, k);
                r.tO += (*d)[static_cast<std::size_t>(k)] * childTR(t, root, ai, k);
            }
        }
        for (int k = 0; k <= 8; ++k) {
            const int c = MidgameSearch::branchOf(root, ai, k);
            if (c >= 0) {
                r.sub += t.subtreeNodes[static_cast<std::size_t>(c)];
                r.maxExp = std::max(r.maxExp, k);
            } else if (c == MidgameSearch::kUnexp) {
                if (d && (*d)[static_cast<std::size_t>(k)] > 0.0L && k >= 3) ++r.bigUnexp;
            } else {
                if (d && k >= 3) ++r.bigDom;
            }
        }
        r.vD = actionValDepthR(t, root, a, ai, dMin);
        const long double L = 1.0L - (1.0L - root.dacc) * (1.0L - static_cast<long double>(a.p));
        r.vLeaf = static_cast<long double>(a.p) + (1.0L - static_cast<long double>(a.p)) * L;
        r.vFull = actionValR(t, root, a, ai);
        rows.push_back(r);
    }

    // 交叉验证：复算 score == 节点存的 score（tLocal > eps 时）。
    int mism = 0;
    for (const Row& r : rows) {
        if (root.tLocal > 0.01L && r.r > 0) {
            const long double sc = r.r * r.cO * r.tO / r.mult;
            if (std::fabs(sc - r.score) > 1e-12L * std::max(1.0L, std::fabs(sc))) ++mism;
        }
    }
    std::printf("cross-check score: %s (%d/%zu mismatch)\n",
                mism == 0 ? "OK" : "MISMATCH", mism, rows.size());

    // 排序打印：score 降序 top10 + 全部 frontier 格 + p 最低 5 格。
    std::vector<int> order(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int l, int r) {
        if (rows[l].score != rows[r].score) return rows[l].score > rows[r].score;
        return rows[l].i < rows[r].i;
    });
    const auto printRow = [](const Row& r) {
        std::printf(
            "#%3d (%2d,%2d) bin=%d fr=%d p=%.4Lf m=%.2Lf r=%.4Lf sc=% .6Lf C=%.3Lf t=%.3Lf "
            "sub=%4d maxD=%d bu=%d bd=%d vD=%.4Lf vL=%.4Lf vF=%.4Lf\n",
            r.i, r.x, r.y, r.bin, r.frontier ? 1 : 0, r.p, r.mult, r.r, r.score, r.cO, r.tO,
            r.sub, r.maxExp, r.bigUnexp, r.bigDom, r.vD, r.vLeaf, r.vFull);
    };
    std::printf("-- score top 10 --\n");
    for (int n = 0; n < 10 && n < static_cast<int>(order.size()); ++n)
        printRow(rows[order[static_cast<std::size_t>(n)]]);
    std::printf("-- frontier cells (信息格) --\n");
    for (const Row& r : rows)
        if (r.frontier) printRow(r);

    // 问题1：拿到很多节点但完全不展开大数字（maxD<=2）的动作。
    std::printf("-- 高预算但 maxD<=2（拿节点不展开大数字）--\n");
    int shown = 0;
    for (int n = 0; n < static_cast<int>(order.size()) && shown < 8; ++n) {
        const Row& r = rows[order[static_cast<std::size_t>(n)]];
        if (r.sub >= 4 && r.maxExp <= 2) {
            printRow(r);
            ++shown;
        }
    }
    // 问题2：vD（d_min 截断）与 vLeaf（叶子近似）差异最大的动作。
    std::printf("-- |vD-vLeaf| 最大的 5 个 --\n");
    std::vector<int> o2(order);
    std::sort(o2.begin(), o2.end(), [&](int l, int r) {
        return std::fabs(rows[l].vD - rows[l].vLeaf) > std::fabs(rows[r].vD - rows[r].vLeaf);
    });
    for (int n = 0; n < 5 && n < static_cast<int>(o2.size()); ++n)
        printRow(rows[o2[static_cast<std::size_t>(n)]]);

    // 大数字分支总体统计。
    int bigTot = 0, bigExp = 0, bigUnexp = 0, bigDom = 0;
    for (const Row& r : rows) {
        bigTot += r.bigUnexp + r.bigDom + (r.maxExp >= 3 ? 1 : 0);
        bigExp += r.maxExp >= 3 ? 1 : 0;
        bigUnexp += r.bigUnexp;
        bigDom += r.bigDom;
    }
    std::printf("bigDigit(>=3) branches: total=%d expanded=%d unexpanded=%d dominated=%d\n",
                bigTot, bigExp, bigUnexp, bigDom);
    std::fflush(stdout);
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
    std::printf("board %dx%d mines=%d candidates=%Lf\n", R, C, M, s.prob.candidates);
    dump(s, "after-build (0 nodes)");
    MidgameSearch::grow(s, 128);
    dump(s, "after 128");
    MidgameSearch::grow(s, 896);
    dump(s, "after 1024");
    MidgameSearch::grow(s, 3072);
    dump(s, "after 4096");

    // 内存上限验证：64MB 封顶，节点数应停在 ~maxMemBytes/perNode。
    {
        MidgameSearch::Session s2;
        if (MidgameSearch::build(s2, b)) {
            s2.config.maxMemBytes = 64LL * 1024 * 1024;
            const long double perNode =
                static_cast<long double>(s2.tree.nodes[0].actions.size()) * 140.0L + 256.0L;
            MidgameSearch::grow(s2, 100000);
            std::printf(
                "mem-limit check: perNode=%.0LfB cap64MB -> nodes=%d (expect ~%.0Lf)\n",
                perNode, s2.tree.statsNodes,
                1.0L + static_cast<long double>(64LL * 1024 * 1024) / perNode);
        }
    }
    return 0;
}