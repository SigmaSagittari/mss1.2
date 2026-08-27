// memlimit_check.cpp — 验证内存上限准确性：默认 1GB，grow 冲到停流，打印估算内存。
#include <cstdio>

#include "analysis/search/midgame_search.h"
#include "core/config.h"

using namespace mss;

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
    const std::size_t cand = s.tree.nodes[0].actions.size();
    const long double perNode = static_cast<long double>(cand) * 64.0L + 1200.0L;
    std::printf("candidates=%zu perNode=%.0LfB maxMem=%.0LfMB\n", cand, perNode,
                static_cast<long double>(s.config.maxMemBytes) / 1048576.0L);
    MidgameSearch::grow(s, 200000);
    const long double est = static_cast<long double>(s.tree.statsNodes) * perNode;
    std::printf("nodes=%d estMem=%.1fMB (cap %.0fMB, %.0f%%)\n", s.tree.statsNodes,
                static_cast<double>(est / 1048576.0L),
                static_cast<double>(s.config.maxMemBytes / 1048576.0),
                100.0 * est / static_cast<double>(s.config.maxMemBytes));
    return 0;
}