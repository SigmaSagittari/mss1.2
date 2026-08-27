#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"
#include "core/utility/rng.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// midgame_search.h — 中盘搜索 v1（anytime 树 + 概率质量流）。
//
// 候选方案数超过残局暴力阈值时，用搜索树推演"点开某格后怎么点最安全"。
//
// anytime：Session 常驻持有搜索树，可跨调用增量生长（grow），任意时刻
// 可查询（getAnswer）。盘面变了（指纹不匹配）才重建会话。
//
// 树节点 = 盘面状态，只存 Delta + 路径指纹，不存盘面快照：
//   - basicDelta / structureDelta：物化时沿路径用 applyDelta 重放（零拷贝重建）
//   - path：U128 位置指纹，兼做换位表（同盘面取低累积死亡，高者当 100% 死丢弃）
//
// 预算 = 节点数（一次物化+展开 = 1 节点），纯计数、确定性、无计时器。
// 分配 = 概率质量流：每个预算单位从入口注入，沿每层由 (c,t) 算出的
// 分流概率抽样下潜（决策转移 vs 观测转移两种公式），落到未展开分支即创建。
// 抽样用固定 seed 的 Rng → 同 seed 同结果，且不锁死局部最优。
//
// 求值 = 增量维护：每个节点的 (价值 V, t_local, t, C, r, 分流分数) 存
// 在节点上，新节点创建后沿路径自底向上刷新（O(depth×actions)），不整树重算。
//
// 价值 = 期望死亡概率（log-生存空间累积 Σlog(1-p)）。
// 不同深度的招法在公共深度 d_min（各动作已搜深度的最小值）上比较：
// 截断偏置一致，直接可比（不需要残差项）。
// ─────────────────────────────────────────────────────────────

struct MidgameSearch {
    // 可标定参数。
    struct Config {
        int nodeBudget = 128;
        long double s = 0.02L;     // log-生存尺度（越小头部越陡：ΔL=0.3 时 r 从 1/7 → 1/16）
        long double eps = 0.01L;   // 停流阈值：t_local ≤ eps → 子树出局
        long double alpha = 0.5L;  // 需求度子项阻尼指数
        // 树内存上限（字节）：树节点只增不删，长时间运行会无限膨胀，
        // 按内存字节封顶（候选少的盘面可跑更多节点）。
        // 存储 double 化 + 懒 observe 后每节点约 67KB（30x16/99 盘面），
        // 默认 1GB ≈ 1.5 万节点（约 4 层深）；想更深直接改这里。
        long long maxMemBytes = 1024LL * 1024 * 1024;
    };

    // 搜索上下文：根盘面状态（只读引用）+ 池 + 抽样 Rng。
    struct Ctx {
        const ObservedBoard& board;
        const Basic::Result& basic;
        const Structure::Result& structure;
        const Probability::Result& prob;
        Structure::ShapePool& shapes;
        Distribution::DistPool& pool;
        Rng& rng;
        Config config;
    };

    // 查询结果。
    struct Answer {
        int x = 0, y = 0;        // 最优落子（1-based）
        long double value = 0;   // 最优动作在公共深度 d_min 上的价值（死亡概率）
        int depth = 0;           // 公共比较深度 d_min
        int nodes = 0;           // 消耗节点数（累计）
        int observes = 0;        // observe 调用次数（累计）
    };

    // 已展开分支的稀疏引用：一个动作的一次 digit 展开占一条。
    // 全树只有真正展开过的分支会记录——不再为每个候选格 × 9 槽存 -1 占位。
    struct BranchRef {
        int ai = -1;         // actions 下标
        int k = -1;          // digit 0..8
        int child = kUnexp;  // 子节点 id；kDominated = 被换位表支配
    };

    // 一个候选动作（盘面节点的孩子；机会节点隐含在分支转移里）。
    // 扁平紧凑：只有 cell/概率/懒 digit 索引；分支引用见 Node::branches。
    struct Action {
        CellId cell = -1;
        double mult = 1.0;   // 非前沿乘系数（只作用于分流分数）
        double p = 0;        // 雷概率（直接查 analyze 结果）
        int digitIdx = -1;   // 懒 observe：Node::readyDigits 索引；-1 = 未观察
    };

    // 树节点 = 一个盘面状态。
    struct Node {
        int parent = -1;
        CellId cell = -1;        // 本节点由哪个格翻开而来（根 = -1）
        int digit = -1;          // 翻开结果 0..8（根 = -1）
        U128 path = {};          // 位置指纹（换位表键）
        int depth = 0;
        long double L = 0;       // log-生存和 Σlog(1-p)
        long double dacc = 0;    // 累积死亡 = 1−exp(L)
        bool expanded = false;
        std::vector<Action> actions;
        std::vector<BranchRef> branches;                  // 本节点已展开分支（稀疏）
        int expActions = 0;                               // 已展开过分支的招法数（探索度；首吃时 ++）
        std::vector<std::array<double, 9>> readyDigits;   // 懒 observe 的 digit 池（Action::digitIdx 索引）
        Basic::Delta basicDelta;       // 从父到本节点的增量（物化重放用）
        Structure::Delta structureDelta;

        // 求值（增量维护，expand 后计算，子变化时沿路径刷新）：
        long double value = 0;     // 当前树价值 = min_a V(O(a))
        long double tLocal = 1;    // 本层争议（log-生存差）
        long double t = 1;         // 链式争议 t_local × Σ r·t(O)
        long double C = 1;         // 需求度 t_local × (Σ r·C(O))^{1−α}
        std::vector<long double> r;      // 归一化竞争力（与 actions 对齐）
        std::vector<long double> score;  // 决策转移分数（与 actions 对齐）
        long double scoreSum = 0;
    };

    // 搜索树本体。nodes[0] = 根（真实盘面，无 delta）。
    struct Tree {
        std::vector<Node> nodes;
        std::vector<int> subtreeNodes;           // 与 nodes 对齐：该节点为根的子树节点数
        FlatHashTable<U128, int, U128Hash> seen;  // path → 节点（换位表）
        int rows = 0, cols = 0;
        int statsNodes = 0;
        int statsObserves = 0;
    };

    // 常驻会话：自持根盘面管线 + 池 + 搜索树 + 抽样 Rng。
    struct Session {
        Tree tree;
        ObservedBoard board;
        Basic::Result basic;
        Structure::Result structure;
        Probability::Result prob;
        Structure::ShapePool shapes;
        Distribution::DistPool pool;
        Rng rng = Rng(0x5EED2026);
        U128 fp = {};          // 盘面指纹（状态一致性）
        Config config;
        bool valid = false;
        std::string reason;    // 不合法原因
    };

    static constexpr int kUnexp = -1;      // 分支未展开（默认 (c,t)=(1,1)）
    static constexpr int kDominated = -2;  // 分支被换位表支配（价值=1，无需求）
    static constexpr std::array<long double, 4> kBinMult = {1.0L, 1.1L, 1.3L, 1.7L};

    // ── 会话 ──
    // 构建会话：全量管线 + 合法性检查 + 展开根。失败返回 false（s.reason 说明）。
    static bool build(Session& s, const ObservedBoard& board);
    // 会话是否匹配当前盘面（指纹一致）。
    static bool matches(const Session& s, const ObservedBoard& board);
    // anytime 增长：追加 n 个节点（内部停流自动提前终止）。
    static void grow(Session& s, int n);
    // 任意时刻查询最优落子（在公共深度 d_min 上比较各动作价值）。
    static Answer getAnswer(const Tree& t);
    // 只读导出搜索树（log/交互展示用），maxDepth/maxNodes 防止输出撑爆。
    static void dumpTree(const Tree& t, std::ostream& os, int maxDepth = 5, int maxNodes = 200);

    // ── 只读树查询（供 UI/诊断读树；稀疏分支/懒 digit 的统一访问口）──
    // 动作分支状态：未展开 kUnexp；被支配 kDominated；否则子节点 id。
    static int branchOf(const Node& n, int ai, int k) {
        for (const BranchRef& b : n.branches)
            if (b.ai == ai && b.k == k) return b.child;
        return kUnexp;
    }
    // 动作 digit 分布指针；nullptr = 未 observe（懒模式下常见）。
    static const std::array<double, 9>* digitOf(const Node& n, const Action& a) {
        return (a.digitIdx >= 0 && a.digitIdx < static_cast<int>(n.readyDigits.size()))
                   ? &n.readyDigits[static_cast<std::size_t>(a.digitIdx)]
                   : nullptr;
    }

    // ── 树操作（供会话使用）──
    // 展开根（花 1 节点）。
    static void init(Tree& t, const Ctx& ctx);
    // 花 1 个预算单位：从 entry 注入质量，沿概率流抽样到未展开分支，创建并展开该节点。
    // 返回新节点 id；-1 = 停流（无可花）；-2 = 分支被支配（未创建，调用方可重试）。
    static int applyNode(Tree& t, int entry, const Ctx& ctx);

private:
    struct Materialized {
        ObservedBoard board;
        Basic::Result basic;
        Structure::Result structure;
        Probability::Result prob;
    };

    static U128 fingerprint(const ObservedBoard& b);
    static U128 pathExtend(U128 parent, CellId cell, int digit);
    static void candidates(const ObservedBoard& board, const Basic::Result& basic,
                           std::vector<Action>& out);
    static int binOf(const ObservedBoard& board, const Basic::Result& basic, int x, int y);
    static void expand(Tree& t, Node& n, const Materialized& m, const Ctx& ctx);
    static Materialized materialize(const Tree& t, int nodeId, const Ctx& ctx);
    // 懒观察：确保动作 digit 分布已算（首次展开分支时才 observe，结果缓存于 readyDigits）。
    static void ensureObserve(Tree& t, int nodeId, int ai, const Materialized& m, const Ctx& ctx);
    // 展开指定分支（k）生成子节点；父状态已在 m 中物化。
    static int expandChild(Tree& t, int parentId, int ai, int k, Materialized& m, const Ctx& ctx);
    // 展开该动作下一个未展开分支（digit 降序）：物化 → 懒 observe → 选分支 → 展开。
    static int expandNextBranch(Tree& t, int parentId, int ai, const Ctx& ctx);
    // 增量刷新单个节点的求值（读子节点已存的值）。
    static void refreshNode(Tree& t, int nodeId, const Config& cfg);

    // ── 查询 ──
    static int sampleByWeight(const long double* w, int n, long double wSum, Rng& rng);
    // 该招法首次有分支记录时计入探索度（expActions++）。
    static void markFirstExp(Node& n, int ai);
};

// ── 实现区 ──

inline U128 MidgameSearch::fingerprint(const ObservedBoard& b) {
    U128Hasher h;
    h.mix(b.rows);
    h.mix(b.cols);
    h.mix(b.totalMines);
    for (int i = 1; i <= b.rows; ++i)
        for (int j = 1; j <= b.cols; ++j)
            h.mix(static_cast<int>(b.board[i][j]));
    return h.finalize();
}

inline U128 MidgameSearch::pathExtend(U128 parent, CellId cell, int digit) {
    U128Hasher h;
    h.mix(parent.lo);
    h.mix(parent.hi);
    h.mix(static_cast<std::uint64_t>(cell));
    h.mix(digit);
    return h.finalize();
}

inline int MidgameSearch::binOf(const ObservedBoard& board, const Basic::Result& basic, int x,
                                int y) {
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

inline void MidgameSearch::candidates(const ObservedBoard& board, const Basic::Result& basic,
                                      std::vector<Action>& out) {
    using Mark = Basic::Mark;
    out.clear();
    const int rows = board.rows, cols = board.cols;
    // 所有未翻开且非已定雷的格都作为候选，不取代表。
    // 非前沿格保留 kBinMult debuff；前沿/安全格 mult=1，语义统一。
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) {
            if (board.board[i][j] != Cell::Hidden) continue;
            if (basic.marks[i][j] == Mark::Mine) continue;
            Action a;
            a.cell = board.id(i, j);
            if (basic.marks[i][j] == Mark::Frontier || basic.marks[i][j] == Mark::Safe) {
                a.mult = 1.0;
            } else {
                a.mult = static_cast<double>(kBinMult[static_cast<std::size_t>(
                    binOf(board, basic, i, j))]);
            }
            out.push_back(a);
        }
}
inline void MidgameSearch::expand(Tree& t, Node& n, const Materialized& m, const Ctx& ctx) {
    n.expanded = true;
    candidates(m.board, m.basic, n.actions);
    for (Action& a : n.actions) {
        // 爆炸概率直接查 analyze 结果（O(1)），不调 observe。
        // digit 分布懒计算：只有该动作首次被选中展开分支时才 observe（见 ensureObserve）。
        a.p = static_cast<double>(
            m.prob.mineProbability(a.cell, m.board, m.basic, m.structure));
    }
}

inline MidgameSearch::Materialized MidgameSearch::materialize(const Tree& t, int nodeId,
                                                              const Ctx& ctx) {
    Materialized m;
    m.board = ctx.board;
    m.basic = ctx.basic;
    m.structure = ctx.structure;
    // 路径：nodeId → 根（逆序后 = 根 → nodeId）。
    std::vector<int> path;
    for (int id = nodeId; id > 0; id = t.nodes[static_cast<std::size_t>(id)].parent)
        path.push_back(id);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Node& nd = t.nodes[static_cast<std::size_t>(*it)];
        const auto [x, y] = m.board.pos(nd.cell);
        m.board.board[x][y] = static_cast<Cell>(nd.digit);
        Basic::Updater::applyDelta(m.basic, nd.basicDelta);
        Structure::Updater::applyDelta(m.structure, nd.structureDelta);
    }
    m.prob = Exact::analyze(m.board, m.basic, m.structure, ctx.pool);
    return m;
}

inline void MidgameSearch::ensureObserve(Tree& t, int nodeId, int ai, const Materialized& m,
                                         const Ctx& ctx) {
    Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    Action& a = n.actions[static_cast<std::size_t>(ai)];
    if (a.digitIdx >= 0) return;
    const Probability::ObserveResult obr =
        Exact::observe(m.board, m.basic, m.structure, m.prob, ctx.pool, a.cell);
    ++t.statsObserves;
    std::array<double, 9> d{};
    for (int k = 0; k <= 8; ++k)
        d[static_cast<std::size_t>(k)] = static_cast<double>(obr.digit[static_cast<std::size_t>(k)]);
    a.digitIdx = static_cast<int>(n.readyDigits.size());
    n.readyDigits.push_back(d);
}

inline int MidgameSearch::expandChild(Tree& t, int parentId, int ai, int k, Materialized& m,
                                      const Ctx& ctx) {
    Node& parent = t.nodes[static_cast<std::size_t>(parentId)];
    Action& a = parent.actions[static_cast<std::size_t>(ai)];

    const long double childDacc = 1.0L - (1.0L - parent.dacc) * (1.0L - a.p);
    const U128 path = pathExtend(parent.path, a.cell, k);

    // 换位表：同 path 已出现过且累积死亡更低 → 本分支当 100% 死丢弃。
    if (const int* found = t.seen.find(path)) {
        const Node& exist = t.nodes[static_cast<std::size_t>(*found)];
        if (exist.dacc <= childDacc + 1e-15L) {
            markFirstExp(parent, ai);  // 该招法被尝试过（即使支配）也计入探索度
            parent.branches.push_back(BranchRef{ai, k, kDominated});
            return -2;
        }
    }

    // m 已由调用方物化（父状态）→ 应用翻开 → 增量维护 → 重算概率。
    const auto [x, y] = m.board.pos(a.cell);
    m.board.board[x][y] = static_cast<Cell>(k);
    std::vector<Basic::Update> updates;
    updates.push_back(Basic::Update{a.cell, static_cast<Cell>(k)});
    Basic::Delta bd = Basic::Updater::update(m.board, m.basic, updates);
    Structure::Delta sd =
        Structure::Updater::update(m.board, m.basic, m.structure, ctx.shapes, updates);
    m.prob = Exact::analyze(m.board, m.basic, m.structure, ctx.pool);

    Node child;
    child.parent = parentId;
    child.cell = a.cell;
    child.digit = k;
    child.path = path;
    child.depth = parent.depth + 1;
    child.L = parent.L + std::log(std::max(1e-15L, 1.0L - std::min(1.0L - 1e-15L,
                                                                     static_cast<long double>(a.p))));
    child.dacc = childDacc;
    child.basicDelta = std::move(bd);
    child.structureDelta = std::move(sd);

    const int childId = static_cast<int>(t.nodes.size());
    expand(t, child, m, ctx);
    t.nodes.push_back(std::move(child));
    t.subtreeNodes.push_back(1);
    t.seen[path] = childId;
    ++t.statsNodes;
    for (int id = parentId; id >= 0; id = t.nodes[static_cast<std::size_t>(id)].parent)
        ++t.subtreeNodes[static_cast<std::size_t>(id)];
    // 写回父分支（push_back 后 Node 引用失效，重新取）。
    Node& parent2 = t.nodes[static_cast<std::size_t>(parentId)];
    markFirstExp(parent2, ai);
    parent2.branches.push_back(BranchRef{ai, k, childId});
    // 增量刷新：新节点 → 根。
    for (int id = childId; id >= 0; id = t.nodes[static_cast<std::size_t>(id)].parent)
        refreshNode(t, id, ctx.config);
    return childId;
}

inline int MidgameSearch::expandNextBranch(Tree& t, int parentId, int ai, const Ctx& ctx) {
    // 物化父状态一次 → 懒 observe 该动作的 digit 分布 → 选 digit 最大的未展开分支展开。
    Materialized m = materialize(t, parentId, ctx);
    Node& parent = t.nodes[static_cast<std::size_t>(parentId)];
    ensureObserve(t, parentId, ai, m, ctx);
    const Action& a = parent.actions[static_cast<std::size_t>(ai)];
    const std::array<double, 9>& d = parent.readyDigits[static_cast<std::size_t>(a.digitIdx)];
    int kExpand = -1;
    long double bestD = -1.0L;
    for (int k = 0; k <= 8; ++k) {
        if (d[static_cast<std::size_t>(k)] <= 0.0) continue;
        if (branchOf(parent, ai, k) == kUnexp && static_cast<long double>(d[static_cast<std::size_t>(k)]) > bestD) {
            bestD = d[static_cast<std::size_t>(k)];
            kExpand = k;
        }
    }
    if (kExpand < 0) return -2;  // 无可展开分支（防御；调用方重试其他动作）
    return expandChild(t, parentId, ai, kExpand, m, ctx);
}

// 求值：读子节点已存的值，只重算本节点。子变化时必须自底向上沿路径调用。
    // 注意：本函数的所有"动作侧概率"用视同概率 p' = p × mult——非前沿格 mult
    // 是"视同概率乘以"（20%×1.2→24%），只在节点分配（r/竞争/score）生效；
    // 评估/展示侧（getAnswer、质量标注）仍用真实 p。
inline void MidgameSearch::refreshNode(Tree& t, int nodeId, const Config& cfg) {
    Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    if (!n.expanded) return;
    const std::size_t na = n.actions.size();
    n.r.assign(na, 0.0L);
    n.score.assign(na, 0.0L);
    if (na == 0) return;

    // 动作视同概率（分配侧）：p' = p × mult。
    auto pEff = [&](int ai) -> long double {
        const Action& a = n.actions[static_cast<std::size_t>(ai)];
        return static_cast<long double>(a.p) * static_cast<long double>(a.mult);
    };
    // 动作当前价值（子节点存 value；未展开 = 叶子值（用视同概率）；被支配 = 100% 死）。
    auto childValue = [&](int ai, int k) -> long double {
        const int c = branchOf(n, ai, k);
        if (c == kDominated) return 1.0L;
        if (c == kUnexp)
            return 1.0L - (1.0L - n.dacc) * (1.0L - pEff(ai));
        return t.nodes[static_cast<std::size_t>(c)].value;
    };
    auto childT = [&](int ai, int k) -> long double {
        const int c = branchOf(n, ai, k);
        if (c == kDominated) return 0.0L;
        if (c == kUnexp) return 1.0L;
        return t.nodes[static_cast<std::size_t>(c)].t;
    };
    auto childC = [&](int ai, int k) -> long double {
        const int c = branchOf(n, ai, k);
        if (c == kDominated) return 0.0L;
        if (c == kUnexp) return 1.0L;
        return t.nodes[static_cast<std::size_t>(c)].C;
    };

    std::vector<long double> v(na, 0.0L);
    std::vector<long double> tO(na, 0.0L);
    std::vector<long double> cO(na, 0.0L);
    long double v1 = 1e30L, v2 = 1e30L;
    bool hasSafe = false;
    for (std::size_t i = 0; i < na; ++i) {
        const Action& a = n.actions[i];
        const long double pe = pEff(static_cast<int>(i));
        long double av = pe;   // 爆炸分支贡献 p'×1（视同概率）
        const std::array<double, 9>* d = digitOf(n, a);
        if (!d) {
            // digit 未 observe（该动作从未展开过分支）：所有分支都是叶子值 L，
            // Σdigit = 1-p' 聚合——不需要单个 digit 概率。
            const long double L = 1.0L - (1.0L - n.dacc) * (1.0L - pe);
            av += (1.0L - pe) * L;
            tO[i] = 1.0L - pe;
            cO[i] = 1.0L - pe;
        } else {
            for (int k = 0; k <= 8; ++k) {
                if ((*d)[static_cast<std::size_t>(k)] <= 0.0) continue;
                av += (*d)[static_cast<std::size_t>(k)] * childValue(static_cast<int>(i), k);
                tO[i] += (*d)[static_cast<std::size_t>(k)] * childT(static_cast<int>(i), k);
                cO[i] += (*d)[static_cast<std::size_t>(k)] * childC(static_cast<int>(i), k);
            }
        }
        v[i] = av;
        if (av < v1) {
            v2 = v1;
            v1 = av;
        } else if (av < v2) {
            v2 = av;
        }
        if (a.p <= 0.0L) hasSafe = true;
    }
    n.value = v1;

    // 必安格（p=0）在当前招法选择上绝对优先：
    // 只要存在必安格，分配只看这些必安格，风险格不参与这层抢资源。
    long double vBest = v1;
    long double vSecond = v2;
    int activeCount = static_cast<int>(na);
    if (hasSafe) {
        vBest = 1e30L;
        vSecond = 1e30L;
        activeCount = 0;
        for (std::size_t i = 0; i < na; ++i) {
            if (n.actions[i].p > 0.0L) continue;
            ++activeCount;
            const long double av = v[i];
            if (av < vBest) {
                vSecond = vBest;
                vBest = av;
            } else if (av < vSecond) {
                vSecond = av;
            }
        }
        n.value = vBest;
    }

    // ΔL（log-生存空间）；v→1 时 log(0) 守卫。
    const long double safe = 1e-15L;
    const long double logV1 = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, vBest)));
    long double rSum = 0;
    for (std::size_t i = 0; i < na; ++i) {
        const bool active = !hasSafe || n.actions[i].p <= 0.0L;
        if (!active) continue;
        const long double lv = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, v[i])));
        n.r[i] = 1.0L / (1.0L + std::max(0.0L, logV1 - lv) / cfg.s);
        rSum += n.r[i];
    }
    if (rSum > 0) {
        for (std::size_t i = 0; i < na; ++i) n.r[i] /= rSum;
    } else if (activeCount > 0) {
        const long double denom = static_cast<long double>(activeCount);
        for (std::size_t i = 0; i < na; ++i)
            if ((!hasSafe || n.actions[i].p <= 0.0L)) n.r[i] = 1.0L / denom;
    } else {
        for (std::size_t i = 0; i < na; ++i) n.r[i] = 1.0L / static_cast<long double>(na);
    }

    // t_local：最优 vs 次优的 log-生存差；只有一个可分配动作时没有本层竞争。
    // 注意：t/C 是乘性链（t = tLocal × Σr·tO 逐层累积），探索类因子若 >1 会指数爆炸
    // （实测 score 到 1e244）。"只搜了一个招法"的探索需求不能直接乘 tLocal，需另议。
    n.tLocal = 1.0L;
    if (activeCount >= 2) {
        const long double logV2 = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, vSecond)));
        n.tLocal = 1.0L / (1.0L + std::max(0.0L, logV1 - logV2) / cfg.s);
    }

    // t / C / 决策转移分数。
    long double sumRT = 0, sumRC = 0;
    for (std::size_t i = 0; i < na; ++i) {
        sumRT += n.r[i] * tO[i];
        sumRC += n.r[i] * cO[i];
    }
    n.t = n.tLocal * sumRT;
    n.C = n.tLocal * std::pow(std::max(0.0L, sumRC), 1.0L - cfg.alpha);
    n.scoreSum = 0;
    if (n.tLocal > cfg.eps) {
        for (std::size_t i = 0; i < na; ++i) {
            if (n.r[i] <= 0.0L) continue;
            // 探索项：该动作子分支局面的未探索度加权（∈[0, 1-p]），乘进 score。
            // score 每层独立（不沿深度累积），探索因子不会像 t/C 那样指数爆炸。
            // 未展开分支 = 完全未知（未探索度 1）；已展开分支 = 子节点已探索比例余量；
            // 被支配分支 = 已判定无需再探（0）。只被搜了一个招法的局面，其父动作
            // 探索项接近最大 → 持续吸引预算，盘面不被"无争议"饿死。
            long double unexp = 0.0L;
            const std::array<double, 9>* d = digitOf(n, n.actions[i]);
            if (!d) {
                unexp = 1.0L - static_cast<long double>(n.actions[i].p);  // 全未观察：全部未知
            } else {
                for (int k = 0; k <= 8; ++k) {
                    const double dk = (*d)[static_cast<std::size_t>(k)];
                    if (dk <= 0.0) continue;
                    const int c = branchOf(n, static_cast<int>(i), k);
                    if (c == kUnexp) {
                        unexp += dk;
                    } else if (c >= 0) {
                        const Node& cn = t.nodes[static_cast<std::size_t>(c)];
                        const long double frac =
                            cn.actions.empty()
                                ? 1.0L
                                : static_cast<long double>(cn.expActions) /
                                      static_cast<long double>(cn.actions.size());
                        unexp += dk * (1.0L - frac);
                    }
                    // kDominated：已判定，探索度 0。
                }
            }
            const long double explore = 1.0L + unexp;
            // 视同概率（mult）已通过 p' 进入 v→r 竞争；score 不再重复除 mult。
            n.score[i] = n.r[i] * cO[i] * tO[i] * explore;
            n.scoreSum += n.score[i];
        }
    }
}
inline int MidgameSearch::sampleByWeight(const long double* w, int n, long double wSum,
                                         Rng& rng) {
    if (n <= 0 || wSum <= 0) return -1;
    const long double r = rng.nextUnit() * wSum;
    long double acc = 0;
    for (int i = 0; i < n; ++i) {
        acc += w[i];
        if (r <= acc) return i;
    }
    return n - 1;
}

inline void MidgameSearch::markFirstExp(Node& n, int ai) {
    for (const BranchRef& b : n.branches)
        if (b.ai == ai) return;
    ++n.expActions;
}

inline int MidgameSearch::applyNode(Tree& t, int entry, const Ctx& ctx) {
    if (t.nodes.empty() || !t.nodes[0].expanded) return -1;
    const Config& cfg = ctx.config;
    // 内存上限：树内存 ≈ 节点数 × 每节点估计（候选数 × 每动作 64B + 固定 ~1.2KB）。
    // 每动作 64B = Action 32B（cell/mult/p/digitIdx）+ r/score 各 16B；
    // 稀疏分支/懒 digit 占树总量比例极低，已含在固定项。
    // 实测（30x16/99，476 候选）：每节点 31.7KB = 476×64+1200 ≈ 31.7KB ✓。
    // 超限即停流（返回 -1，调用方 grow 会停止，后台线程随之退出）。
    if (!t.nodes.empty()) {
        const long double perNode =
            static_cast<long double>(t.nodes[0].actions.size()) * 64.0L + 1200.0L;
        if (static_cast<long double>(t.statsNodes) * perNode >=
            static_cast<long double>(cfg.maxMemBytes))
            return -1;
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        int cur = entry;
        int res = -1;
        for (;;) {
            const Node& n = t.nodes[static_cast<std::size_t>(cur)];
            if (n.scoreSum <= 0) return -1;  // 停流
            // ── 决策转移（局面→招法）：纯 score 分配，不做填满 ──
            // 局面→招法由 score（含不稳定度与概率）自然分配：垃圾动作 score 低，
            // 自然被饿死——不需要特判/保底。填满发生在"招法→局面"层（见观测转移）。
            const int ai = sampleByWeight(n.score.data(), static_cast<int>(n.score.size()),
                                          n.scoreSum, ctx.rng);
            if (ai < 0) return -1;
            // ── 观测转移（招法→局面）：这是"填满"发生的地方 ──
            // 选中的招法只要还有未展开的 digit 分支局面（含未 observe 的首吃），
            // 就展开一个（digit 降序，保证大数字也被评估、拿到完整基本数值）；
            // 该招法的局面层全部填满后，才允许按权重 digit × C(子) × t(子) 下潜深搜。
            const Action& a = n.actions[static_cast<std::size_t>(ai)];
            {
                const std::array<double, 9>* d = digitOf(n, a);
                bool hasUnexp = !d;  // 未 observe：必存在可展开分支（Σdigit = 1-p > 0）
                if (d) {
                    for (int k = 0; k <= 8; ++k)
                        if ((*d)[static_cast<std::size_t>(k)] > 0.0 &&
                            branchOf(n, ai, k) == kUnexp) {
                            hasUnexp = true;
                            break;
                        }
                }
                if (hasUnexp) {
                    res = expandNextBranch(t, cur, ai, ctx);
                    break;
                }
            }
            std::array<long double, 9> bw{};
            long double bwSum = 0;
            for (int k = 0; k <= 8; ++k) {
                const double dk = n.readyDigits[static_cast<std::size_t>(a.digitIdx)]
                                     [static_cast<std::size_t>(k)];
                if (dk <= 0) continue;
                const int c = branchOf(n, ai, k);
                if (c == kDominated) continue;
                const long double ct = t.nodes[static_cast<std::size_t>(c)].t;
                const long double cc = t.nodes[static_cast<std::size_t>(c)].C;
                if (ct <= 0 || cc <= 0) continue;
                bw[static_cast<std::size_t>(k)] = dk * ct * cc;
                bwSum += bw[static_cast<std::size_t>(k)];
            }
            const int k = sampleByWeight(bw.data(), 9, bwSum, ctx.rng);
            if (k < 0) return -1;
            cur = branchOf(n, ai, k);  // 下潜到已展开子树
            continue;
        }
        if (res == -2) continue;  // 支配分支：重试
        return res;
    }
    return -1;
}

inline void MidgameSearch::init(Tree& t, const Ctx& ctx) {
    t.nodes.clear();
    t.seen.clear();
    t.rows = ctx.board.rows;
    t.cols = ctx.board.cols;
    t.statsNodes = 0;
    t.statsObserves = 0;
    t.subtreeNodes.clear();
    Node root;
    root.depth = 0;
    root.L = 0;
    root.dacc = 0;
    Materialized m;
    m.board = ctx.board;
    m.basic = ctx.basic;
    m.structure = ctx.structure;
    m.prob = ctx.prob;
    expand(t, root, m, ctx);
    t.nodes.push_back(std::move(root));
    t.subtreeNodes.push_back(1);
    ++t.statsNodes;
    refreshNode(t, 0, ctx.config);
}

inline MidgameSearch::Answer MidgameSearch::getAnswer(const Tree& t) {
    Answer ans;
    if (t.nodes.empty() || !t.nodes[0].expanded) return ans;
    const Node& root = t.nodes[0];
    if (root.actions.empty()) return ans;
    // 优劣度 = 动作价值（当前树混合价值：已展开分支用子节点真实 value，
    // 未展开分支用叶子近似），最小者最优。不用公共深度截断——截断在浅树下
    // 会退化成纯 p 函数、抹掉全部搜索信息；也不把分配分数 score 当优劣度。
    long double best = 1e30L;
    for (std::size_t i = 0; i < root.actions.size(); ++i) {
        const Action& a = root.actions[i];
        long double v = a.p;   // 爆炸分支贡献 p×1
        const std::array<double, 9>* d = digitOf(root, a);
        if (!d) {
            // 聚合：所有分支未展开，价值只依赖 p（Σdigit = 1-p）。
            const long double L = 1.0L - (1.0L - root.dacc) * (1.0L - a.p);
            v = a.p + (1.0L - a.p) * L;
        } else {
            for (int k = 0; k <= 8; ++k) {
                if ((*d)[static_cast<std::size_t>(k)] <= 0.0) continue;
                long double childV;
                const int c = branchOf(root, static_cast<int>(i), k);
                if (c == kDominated)
                    childV = 1.0L;
                else if (c == kUnexp)
                    childV = 1.0L - (1.0L - root.dacc) * (1.0L - a.p);
                else
                    childV = t.nodes[static_cast<std::size_t>(c)].value;
                v += (*d)[static_cast<std::size_t>(k)] * childV;
            }
        }
        if (v < best - 1e-15L) {
            best = v;
            const auto [x, y] = std::pair<int, int>{a.cell / (t.cols + 1), a.cell % (t.cols + 1)};
            ans.x = x;
            ans.y = y;
            ans.value = v;
        }
    }
    ans.nodes = t.statsNodes;
    ans.observes = t.statsObserves;
    return ans;
}

inline bool MidgameSearch::build(Session& s, const ObservedBoard& b) {
    s = Session{};   // 重置（Rng 回固定种子 → 同盘面确定性）
    s.board = b;
    s.basic = Basic::Analyzer::analyze(b);
    if (!s.basic.valid) {
        s.reason = "盘面矛盾（basic 无解）";
        return false;
    }
    s.structure = Structure::Analyzer::analyze(b, s.basic, s.shapes);

    // 合法性：各连通块分布非空 + 全局雷数范围。
    int minHeavy = 0, maxHeavy = 0;
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(s.structure.components.size());
         ++cid) {
        const Structure::Instance& inst =
            s.structure.components[static_cast<std::size_t>(cid)];
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, s.pool);
        if (dist->entries.empty()) {
            s.reason = "连通块无可行摆法（矛盾）";
            return false;
        }
        minHeavy += dist->entries.front().mineCount;
        maxHeavy += dist->entries.back().mineCount;
    }
    const int M = b.totalMines - s.basic.mineSum;
    if (s.basic.mineSum > b.totalMines) {
        s.reason = "已定雷数超过盘面总雷数，盘面矛盾";
        return false;
    }
    if (M < minHeavy) {
        s.reason = "全局雷数不足（连通块至少需要 " + std::to_string(minHeavy) + " 雷，只剩 " +
                   std::to_string(M) + " 可用）";
        return false;
    }
    if (M > maxHeavy + s.basic.unknownSum) {
        s.reason = "全局雷数过多（连通块至多容纳 " + std::to_string(maxHeavy) + " 雷 + " +
                   std::to_string(s.basic.unknownSum) + " 未知格，共 " + std::to_string(M) +
                   " 雷无处安放）";
        return false;
    }

    s.prob = Exact::analyze(b, s.basic, s.structure, s.pool);
    s.fp = fingerprint(b);
    s.valid = true;
    Ctx ctx{s.board, s.basic, s.structure, s.prob, s.shapes, s.pool, s.rng, s.config};
    init(s.tree, ctx);
    return true;
}

inline bool MidgameSearch::matches(const Session& s, const ObservedBoard& b) {
    return s.valid && s.fp == fingerprint(b);
}

inline void MidgameSearch::grow(Session& s, int n) {
    Ctx ctx{s.board, s.basic, s.structure, s.prob, s.shapes, s.pool, s.rng, s.config};
    for (int i = 0; i < n; ++i)
        if (applyNode(s.tree, 0, ctx) < 0) break;
}

inline void MidgameSearch::dumpTree(const Tree& t, std::ostream& os, int maxDepth, int maxNodes) {
    os << "search tree nodes=" << t.statsNodes << " observes=" << t.statsObserves << "\n";
    int printed = 0;
    std::function<void(int, int)> rec = [&](int id, int depth) {
        if (printed >= maxNodes || depth > maxDepth) return;
        const Node& n = t.nodes[static_cast<std::size_t>(id)];
        ++printed;
        for (int i = 0; i < depth; ++i) os << "  ";
        os << "#" << id << " depth=" << n.depth;
        if (n.cell >= 0) {
            const int x = n.cell / (t.cols + 1);
            const int y = n.cell % (t.cols + 1);
            os << " cell=(" << x << "," << y << ")";
            if (n.digit >= 0) os << " digit=" << n.digit;
        }
        os << " value=" << static_cast<double>(n.value)
           << " tLocal=" << static_cast<double>(n.tLocal)
           << " t=" << static_cast<double>(n.t)
           << " C=" << static_cast<double>(n.C)
           << " actions=" << n.actions.size() << "\n";
        if (printed >= maxNodes || depth >= maxDepth) return;
        for (std::size_t i = 0; i < n.actions.size(); ++i) {
            if (printed >= maxNodes) break;
            const Action& a = n.actions[i];
            for (int d = 0; d < depth + 1; ++d) os << "  ";
            const int x = a.cell / (t.cols + 1);
            const int y = a.cell % (t.cols + 1);
            os << "move (" << x << "," << y << ") p=" << static_cast<double>(a.p)
               << " r=" << (i < n.r.size() ? static_cast<double>(n.r[i]) : 0.0)
               << " score=" << (i < n.score.size() ? static_cast<double>(n.score[i]) : 0.0)
               << "\n";
            const std::array<double, 9>* dg = digitOf(n, a);
            if (!dg) continue;  // 懒 observe：未观察的动作没有分支明细
            for (int k = 0; k <= 8; ++k) {
                if ((*dg)[static_cast<std::size_t>(k)] <= 0.0L) continue;
                if (printed >= maxNodes) break;
                const int c = branchOf(n, static_cast<int>(i), k);
                for (int d = 0; d < depth + 2; ++d) os << "  ";
                os << "digit " << k << " prob=" << static_cast<double>((*dg)[static_cast<std::size_t>(k)])
                   << " -> ";
                if (c == kDominated) {
                    os << "dominated\n";
                } else if (c == kUnexp) {
                    os << "unexpanded\n";
                } else if (c >= 0 && c < static_cast<int>(t.nodes.size())) {
                    const Node& cn = t.nodes[static_cast<std::size_t>(c)];
                    os << "#" << c << " value=" << static_cast<double>(cn.value)
                       << " t=" << static_cast<double>(cn.t)
                       << " C=" << static_cast<double>(cn.C) << "\n";
                    rec(c, depth + 1);
                }
            }
        }
    };
    rec(0, 0);
    if (printed >= maxNodes) os << "... truncated (maxNodes=" << maxNodes << ") ...\n";
}

}  // namespace mss
