// mem_probe.cpp — 实测搜索树内存：不再用估算，逐节点统计分配器视角的真实占用。
// 输出：各部分字节 / 每节点平均 / 精简模拟（r/score double、digit 稀疏）。
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "analysis/search/midgame_search.h"
#include "core/config.h"

using namespace mss;

static void probe(const MidgameSearch::Session& s, const char* tag) {
    const MidgameSearch::Tree& t = s.tree;
    std::size_t nodeStruct = t.nodes.capacity() * sizeof(MidgameSearch::Node);
    std::size_t actionsBytes = 0, rBytes = 0, scoreBytes = 0, actionsCount = 0;
    std::size_t observedActions = 0, observedDigits = 0, branchesCount = 0, readyBytes = 0;
    for (const MidgameSearch::Node& n : t.nodes) {
        actionsBytes += n.actions.capacity() * sizeof(MidgameSearch::Action);
        actionsCount += n.actions.size();
        rBytes += n.r.capacity() * sizeof(long double);
        scoreBytes += n.score.capacity() * sizeof(long double);
        branchesCount += n.branches.capacity() * sizeof(MidgameSearch::BranchRef);
        readyBytes += n.readyDigits.capacity() * sizeof(std::array<double, 9>);
        for (const MidgameSearch::Action& a : n.actions) {
            if (MidgameSearch::digitOf(n, a) != nullptr) {
                ++observedActions;
                for (int k = 0; k <= 8; ++k)
                    if ((*MidgameSearch::digitOf(n, a))[static_cast<std::size_t>(k)] > 0.0)
                        ++observedDigits;
            }
        }
    }
    std::size_t subtreeBytes = t.subtreeNodes.capacity() * sizeof(int);
    std::size_t seenCap = 4;
    while (seenCap < t.seen.size() * 2) seenCap <<= 1;
    std::size_t seenBytes = seenCap * (sizeof(U128) + sizeof(int) + 1);
    const std::size_t total =
        nodeStruct + actionsBytes + rBytes + scoreBytes + subtreeBytes + seenBytes +
        branchesCount * sizeof(MidgameSearch::BranchRef) + readyBytes;

    std::printf("%s: nodes=%zu (root candidates=%zu) sizeof(Action)=%zu sizeof(Node)=%zu "
                "sizeof(BranchRef)=%zu\n",
                tag, t.nodes.size(), t.nodes[0].actions.size(), sizeof(MidgameSearch::Action),
                sizeof(MidgameSearch::Node), sizeof(MidgameSearch::BranchRef));
    std::printf(
        "  nodeStruct=%zu(%.1fKB) actions=%zu(%.1fKB, %.1fKB/node) r=%zu(%.1fKB) score=%zu(%.1fKB) "
        "branches=%zu(%zu, %.1fKB) readyDigits=%zu(%.1fKB) subtree=%zu seen=%zu\n",
        nodeStruct, nodeStruct / 1024.0, actionsBytes, actionsBytes / 1024.0,
        actionsBytes / 1024.0 / static_cast<double>(t.nodes.size()), rBytes, rBytes / 1024.0,
        scoreBytes, scoreBytes / 1024.0, branchesCount,
        branchesCount * sizeof(MidgameSearch::BranchRef),
        branchesCount * sizeof(MidgameSearch::BranchRef) / 1024.0, readyBytes, readyBytes / 1024.0,
        subtreeBytes, seenBytes);
    std::printf("  TOTAL=%zu (%.2fMB) = %.1fKB/node\n", total, total / 1048576.0,
                total / 1024.0 / static_cast<double>(t.nodes.size()));
    std::printf("  observedActions=%zu (%.1f%% of nodes) avgNonZeroDigit=%.2f\n", observedActions,
                t.nodes.size() > 0
                    ? 100.0 * static_cast<double>(observedActions) /
                          static_cast<double>(t.nodes.size())
                    : 0.0,
                observedActions ? static_cast<double>(observedDigits) / observedActions : 0.0);

    // 精简模拟。
    const std::size_t rsDoubleSave = (rBytes + scoreBytes) / 2;
    std::printf("  sim: r/score->double saves %.0fKB (-%.1f%%)\n", rsDoubleSave / 1024.0,
                100.0 * static_cast<double>(rsDoubleSave) / total);
    if (observedActions > 0) {
        // digit 稀疏：array<double,9> 72B/动作 → (index+val) 8+8B × 平均非零数。
        const double avgNZ = static_cast<double>(observedDigits) / observedActions;
        const double sparseSave = static_cast<double>(observedActions) * (72.0 - avgNZ * 16.0);
        std::printf("  sim: digit sparse (avg %.2f nonzero) saves %.0fKB on ready actions\n", avgNZ,
                    sparseSave / 1024.0);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 盘面 1：30x16/99 四角为 1（476 候选，用户同款盘面）。
    {
        constexpr int R = 16, C = 30, M = 99;
        ObservedBoard b(R, C, M);
        b.board[1][1] = Cell::Num1;
        b.board[1][C] = Cell::Num1;
        b.board[R][1] = Cell::Num1;
        b.board[R][C] = Cell::Num1;
        long double cand = 0;
        MidgameSearch::Session s;
        if (MidgameSearch::build(s, b)) {
            cand = s.prob.candidates;
            MidgameSearch::grow(s, 5000);
        }
        std::printf("board 16x30/99 corners=1 candidates=%Lf\n", cand);
        probe(s, "16x30/99 x5000");
    }
    // 盘面 2：9x9/10（候选少，对比候选数对每节点内存的影响）。
    {
        ObservedBoard b(9, 9, 10);
        // 点开中心 + 贪心低概率推进若干步，形成有候选的盘面。
        // 直接用四角 1 盘面不行（9x9 四个角 1 也是合理盘面）。
        // 这里用手工盘面：中央 3x3 翻开为 1-3 数字 + 周围未知，保证 basic 非空。
        for (int x = 3; x <= 7; ++x)
            for (int y = 3; y <= 7; ++y) b.board[x][y] = Cell::Num1;
        b.board[5][5] = Cell::Num2;
        long double cand = 0;
        MidgameSearch::Session s;
        if (MidgameSearch::build(s, b)) {
            cand = s.prob.candidates;
            MidgameSearch::grow(s, 5000);
        }
        std::printf("board 9x9/10 manual candidates=%Lf\n", cand);
        probe(s, "9x9/10 x5000");
    }
    return 0;
}