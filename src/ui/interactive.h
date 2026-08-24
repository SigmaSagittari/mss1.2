#pragma once

#include "analysis/basic.h"
#include "analysis/probability.h"
#include "analysis/structure.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// interactive.h — 把内部算法翻译成 UI 能看懂的数值（脏活累活）。
//
// 不承载任何分析逻辑，只是把 analysis 层的查询视图摊成前端可直接用的
// 稠密表示。目前只有一个：概率网格物化。
// 后续可加：q_k 分布物化、推荐操作等。
// ─────────────────────────────────────────────────────────────

namespace Interactive {

// 物化：Probability::Result → 整盘每格雷概率网格（1-based，与棋盘一致）。
// 纯读取，不修改任何状态；重复调用无副作用。
inline Grid<long double> materialize(const Probability::Result& result,
                                     const ObservedBoard& board,
                                     const Basic::Result& basic,
                                     const Structure::Result& structure) {
    Grid<long double> grid(board.rows, board.cols, 0.0L);
    for (int x = 1; x <= board.rows; ++x)
        for (int y = 1; y <= board.cols; ++y)
            grid[x][y] = result.mineProbability(board.id(x, y), board, basic, structure);
    return grid;
}

}  // namespace Interactive

}  // namespace mss