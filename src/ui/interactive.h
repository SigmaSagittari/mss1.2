#pragma once

#include <chrono>
#include <string>

#include "analysis/basic.h"
#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/rational.h"
#include "analysis/structure.h"
#include "core/config.h"
#include "core/types.h"
#include "ui/game_control.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// interactive.h — 分析/游戏数据 → UI 可消费表示的翻译层（垃圾桶）。
//
// 各种"计算小垃圾"都扔这里：引擎无关的概率查询、整盘物化、精确对比、
// 全局分析（合法性 + 候选数 + 暴力枚举）等。ui_app 只负责路由/JSON，
// GameController 只负责游戏规则（翻开/泛洪/标旗/胜负）。本层不含游戏
// 规则或 HTTP 逻辑，全是纯计算。
// ─────────────────────────────────────────────────────────────

namespace Interactive {

// ── 引擎无关查询（基于当前 Analysis 管线）──

// 单格雷概率（Mine→1，Unknown→tProb/rho，Safe→0，前沿格→boxProbs）。
// 非 const：RhoRational 惰性记忆化会就地补缓存（幂等，不影响结果）。
inline long double mineProbability(GameController::Analysis& an, int x, int y);

// 候选方案数。
inline long double candidates(const GameController::Analysis& an);

// 非前沿（Unknown）格雷密度。
inline long double tCellProbability(const GameController::Analysis& an);

// 整盘概率网格物化（1-based，与棋盘一致）。逐格查询，O(rows*cols)。
inline Grid<long double> materializeProbability(GameController::Analysis& an);

// 点开某格的结果分布（explosion + digit[0..8]），详情面板查询。
inline Probability::ObserveResult observe(GameController::Analysis& an, int x, int y);

// ── 全局分析（编辑后的盘面全量重构）──

// 一次全局分析的结果。
struct AnalyzeResult {
    bool valid = false;          // 盘面合法（basic + 各连通块分布）
    std::string reason;          // 不合法 / 不暴力的原因说明
    bool bruteforce = false;     // 候选数低于阈值、已执行暴力枚举
    long double candidates = 0;  // 候选方案数（含 T 格组合）
    long double tProb = 0;       // 非前沿雷概率
    Grid<long double> grid;      // 物化概率网格（1-based，与 board 同尺寸）
    int total = 0;               // 暴力方案总数
    int firstX = 0, firstY = 0;  // 最优首招（1-based）
    int wins = 0;                // 可保证赢下的方案数
    double winRate = 0;          // 胜率 %
    long long nodes = 0;         // DFS 节点数
    long long ms = 0;            // 暴力耗时
};

// 对（可能被编辑过的）盘面做全局分析：全量重构 basic/structure/probability，
// 合法性检查 → 候选数 → 低于暴力阈值则残局求解。
inline AnalyzeResult analyze(const ObservedBoard& board);

// ── 实现区 ──

inline long double mineProbability(GameController::Analysis& an, int x, int y) {
    return an.probability().mineProbability(an.state().id(x, y), an.state(),
                                            an.basicMarks(), an.structure());
}

inline long double candidates(const GameController::Analysis& an) {
    return an.probability().candidates;
}

inline long double tCellProbability(const GameController::Analysis& an) {
    return an.probability().tCellProbability;
}

inline Grid<long double> materializeProbability(GameController::Analysis& an) {
    Grid<long double> grid(an.state().rows, an.state().cols, 0.0L);
    for (int x = 1; x <= an.state().rows; ++x)
        for (int y = 1; y <= an.state().cols; ++y)
            grid[x][y] = mineProbability(an, x, y);
    return grid;
}

inline Probability::ObserveResult observe(GameController::Analysis& an, int x, int y) {
    const ObservedBoard& state = an.state();
    const auto& basic = an.basicMarks();
    const auto& structure = an.structure();
    return Exact::observe(state, basic, structure, an.probability(), an.dists(),
                          state.id(x, y));
}

inline AnalyzeResult analyze(const ObservedBoard& state) {
    AnalyzeResult out;

    // 编辑后每次点「开始分析」全量重构。
    const Basic::Result basic = Basic::Analyzer::analyze(state);
    Structure::ShapePool shapes;
    const Structure::Result structure = Structure::Analyzer::analyze(state, basic, shapes);
    Distribution::DistPool dists;

    // 合法性 1：basic 矛盾（数字约束无解）。
    if (!basic.valid) {
        out.reason = "盘面矛盾（basic 无解）";
        return out;
    }
    out.valid = true;

    // 合法性 2：每个连通块的分布非空（无可行摆法 = 结构矛盾）。
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, dists);
        if (dist->entries.empty()) {
            out.valid = false;
            out.reason = "连通块无可行摆法（矛盾）";
            return out;
        }
    }

    // 候选数（精确）：含 T 格组合，= all_distribute 会产出的总方案数。
    const Probability::Result prob = Exact::analyze(state, basic, structure, dists);
    out.candidates = prob.candidates;
    out.tProb = prob.tCellProbability;

    // 物化概率网格（用刚算的 Exact 结果，保证与候选数同源）。
    out.grid = Grid<long double>(state.rows, state.cols, 0.0L);
    for (int i = 1; i <= state.rows; ++i)
        for (int j = 1; j <= state.cols; ++j)
            out.grid[i][j] = prob.mineProbability(state.id(i, j), state, basic, structure);

    // 超过暴力枚举阈值：暂不处理，之后用中盘分析补上（概率网格仍给出）。
    if (out.candidates > static_cast<long double>(kMaxBruteforceCount)) {
        out.bruteforce = false;
        out.reason = "候选方案数超过暴力阈值，中盘分析待实现";
        return out;
    }

    // 开始暴力：残局求解。
    out.bruteforce = true;
    const auto t0 = std::chrono::steady_clock::now();
    const EndgameBruteforce::Result r =
        EndgameBruteforce::solveEndgame(state, basic, structure, dists, out.grid);
    const auto t1 = std::chrono::steady_clock::now();
    out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    out.total = r.totalPossibilities;
    const auto& mv = r.result[0];
    out.firstX = mv.x;
    out.firstY = mv.y;
    out.wins = mv.wins;
    out.nodes = r.nodes;
    out.winRate = r.totalPossibilities > 0
                      ? static_cast<double>(mv.wins) / r.totalPossibilities * 100.0
                      : 0.0;
    return out;
}

}  // namespace Interactive

}  // namespace mss