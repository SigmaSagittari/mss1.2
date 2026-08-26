#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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
        long double s = 0.1L;      // log-生存尺度
        long double eps = 0.01L;   // 停流阈值：t_local ≤ eps → 子树出局
        long double alpha = 0.5L;  // 需求度子项阻尼指数
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

    // 一个候选动作（盘面节点的孩子；机会节点隐含在分支转移里）。
    struct Action {
        CellId cell = -1;
        long double mult = 1.0L;               // 非前沿乘系数（只作用于分流分数）
        long double p = 0;                     // 雷概率（observe explosion）
        std::array<long double, 9> digit{};    // 观测分布
        std::array<int, 9> child = {kUnexp, kUnexp, kUnexp, kUnexp, kUnexp,
                                    kUnexp, kUnexp, kUnexp, kUnexp};  // 分支盘面节点
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
    static constexpr std::array<long double, 4> kBinMult = {1.0L, 1.05L, 1.1L, 1.2L};

    // ── 会话 ──
    // 构建会话：全量管线 + 合法性检查 + 展开根。失败返回 false（s.reason 说明）。
    static bool build(Session& s, const ObservedBoard& board);
    // 会话是否匹配当前盘面（指纹一致）。
    static bool matches(const Session& s, const ObservedBoard& board);
    // anytime 增长：追加 n 个节点（内部停流自动提前终止）。
    static void grow(Session& s, int n);
    // 任意时刻查询最优落子（在公共深度 d_min 上比较各动作价值）。
    static Answer getAnswer(const Tree& t);

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
    static int expandChild(Tree& t, int parentId, int ai, int k, const Ctx& ctx);
    // 增量刷新单个节点的求值（读子节点已存的值）。
    static void refreshNode(Tree& t, int nodeId, const Config& cfg);

    // ── 查询 ──
    static long double valueAtDepth(const Tree& t, int nodeId, int d);
    static long double actionValueAtDepth(const Tree& t, const Node& n, const Action& a, int d);
    static int subtreeMax(const Tree& t, int nodeId);
    static int actionReach(const Tree& t, const Node& n, const Action& a);
    static int sampleByWeight(const long double* w, int n, long double wSum, Rng& rng);
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
    bool seenBin[4] = {false, false, false, false};
    CellId rep[4] = {-1, -1, -1, -1};
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) {
            if (board.board[i][j] != Cell::Hidden) continue;
            if (basic.marks[i][j] == Mark::Mine) continue;
            if (basic.marks[i][j] == Mark::Frontier ||
                basic.marks[i][j] == Mark::Safe) {
                Action a;
                a.cell = board.id(i, j);
                out.push_back(a);
            } else {  // Unknown：按 8 连通 Unknown 数分档，每档取代表
                const int bin = binOf(board, basic, i, j);
                if (!seenBin[bin]) {
                    seenBin[bin] = true;
                    rep[bin] = board.id(i, j);
                }
            }
        }
    for (int bin = 0; bin < 4; ++bin)
        if (seenBin[bin]) {
            Action a;
            a.cell = rep[bin];
            a.mult = kBinMult[static_cast<std::size_t>(bin)];
            out.push_back(a);
        }
}

inline void MidgameSearch::expand(Tree& t, Node& n, const Materialized& m, const Ctx& ctx) {
    n.expanded = true;
    candidates(m.board, m.basic, n.actions);
    for (Action& a : n.actions) {
        const Probability::ObserveResult obr =
            Exact::observe(m.board, m.basic, m.structure, m.prob, ctx.pool, a.cell);
        ++t.statsObserves;
        a.p = obr.explosion;
        a.digit = obr.digit;
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

inline int MidgameSearch::expandChild(Tree& t, int parentId, int ai, int k, const Ctx& ctx) {
    Node& parent = t.nodes[static_cast<std::size_t>(parentId)];
    Action& a = parent.actions[static_cast<std::size_t>(ai)];

    const long double childDacc = 1.0L - (1.0L - parent.dacc) * (1.0L - a.p);
    const U128 path = pathExtend(parent.path, a.cell, k);

    // 换位表：同 path 已出现过且累积死亡更低 → 本分支当 100% 死丢弃。
    if (const int* found = t.seen.find(path)) {
        const Node& exist = t.nodes[static_cast<std::size_t>(*found)];
        if (exist.dacc <= childDacc + 1e-15L) {
            a.child[static_cast<std::size_t>(k)] = kDominated;
            return -2;
        }
    }

    // 物化父状态 → 应用翻开 → 增量维护 → 重算概率。
    Materialized m = materialize(t, parentId, ctx);
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
    child.L = parent.L + std::log(std::max(1e-15L, 1.0L - std::min(1.0L - 1e-15L, a.p)));
    child.dacc = childDacc;
    child.basicDelta = std::move(bd);
    child.structureDelta = std::move(sd);

    const int childId = static_cast<int>(t.nodes.size());
    expand(t, child, m, ctx);
    t.nodes.push_back(std::move(child));
    t.seen[path] = childId;
    ++t.statsNodes;
    // 写回父分支（push_back 后引用失效，重新取）。
    t.nodes[static_cast<std::size_t>(parentId)]
        .actions[static_cast<std::size_t>(ai)]
        .child[static_cast<std::size_t>(k)] = childId;
    // 增量刷新：新节点 → 根。
    for (int id = childId; id >= 0; id = t.nodes[static_cast<std::size_t>(id)].parent)
        refreshNode(t, id, ctx.config);
    return childId;
}

// 求值：读子节点已存的值，只重算本节点。子变化时必须自底向上沿路径调用。
inline void MidgameSearch::refreshNode(Tree& t, int nodeId, const Config& cfg) {
    Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    if (!n.expanded) return;
    const std::size_t na = n.actions.size();
    n.r.assign(na, 0.0L);
    n.score.assign(na, 0.0L);
    if (na == 0) return;

    // 动作当前价值（子节点存 value；未展开 = 叶子值；被支配 = 100% 死）。
    auto childValue = [&](const Action& a, int k) -> long double {
        const int c = a.child[static_cast<std::size_t>(k)];
        if (c == kDominated) return 1.0L;
        if (c == kUnexp) return 1.0L - (1.0L - n.dacc) * (1.0L - a.p);
        return t.nodes[static_cast<std::size_t>(c)].value;
    };
    auto childT = [&](const Action& a, int k) -> long double {
        const int c = a.child[static_cast<std::size_t>(k)];
        if (c == kDominated) return 0.0L;
        if (c == kUnexp) return 1.0L;
        return t.nodes[static_cast<std::size_t>(c)].t;
    };
    auto childC = [&](const Action& a, int k) -> long double {
        const int c = a.child[static_cast<std::size_t>(k)];
        if (c == kDominated) return 0.0L;
        if (c == kUnexp) return 1.0L;
        return t.nodes[static_cast<std::size_t>(c)].C;
    };

    std::vector<long double> v(na, 0.0L);
    std::vector<long double> tO(na, 0.0L);
    std::vector<long double> cO(na, 0.0L);
    long double v1 = 1e30L, v2 = 1e30L;
    for (std::size_t i = 0; i < na; ++i) {
        const Action& a = n.actions[i];
        long double av = a.p;   // 爆炸分支贡献 p×1
        for (int k = 0; k <= 8; ++k) {
            if (a.digit[static_cast<std::size_t>(k)] <= 0.0L) continue;
            av += a.digit[static_cast<std::size_t>(k)] * childValue(a, k);
            tO[i] += a.digit[static_cast<std::size_t>(k)] * childT(a, k);
            cO[i] += a.digit[static_cast<std::size_t>(k)] * childC(a, k);
        }
        v[i] = av;
        if (av < v1) {
            v2 = v1;
            v1 = av;
        } else if (av < v2) {
            v2 = av;
        }
    }
    n.value = v1;

    // ΔL（log-生存空间）；v→1 时 log(0) 守卫。
    const long double safe = 1e-15L;
    const long double logV1 = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, v1)));
    for (std::size_t i = 0; i < na; ++i) {
        const long double lv = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, v[i])));
        n.r[i] = 1.0L / (1.0L + std::max(0.0L, logV1 - lv) / cfg.s);
    }
    long double rSum = 0;
    for (std::size_t i = 0; i < na; ++i) rSum += n.r[i];
    if (rSum > 0) {
        for (std::size_t i = 0; i < na; ++i) n.r[i] /= rSum;
    } else {
        for (std::size_t i = 0; i < na; ++i) n.r[i] = 1.0L / static_cast<long double>(na);
    }

    // t_local：最优 vs 次优的 log-生存差。
    if (na >= 2) {
        const long double logV2 = std::log(std::max(safe, 1.0L - std::min(1.0L - safe, v2)));
        n.tLocal = 1.0L / (1.0L + std::max(0.0L, logV1 - logV2) / cfg.s);
    } else {
        n.tLocal = 1.0L;
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
            n.score[i] = n.r[i] * cO[i] * tO[i] / n.actions[i].mult;
            n.scoreSum += n.score[i];
        }
    }
}

inline long double MidgameSearch::actionValueAtDepth(const Tree& t, const Node& n,
                                                     const Action& a, int d) {
    long double v = a.p;   // 爆炸分支贡献 p×1
    for (int k = 0; k <= 8; ++k) {
        if (a.digit[static_cast<std::size_t>(k)] <= 0.0L) continue;
        long double childV;
        const int c = a.child[static_cast<std::size_t>(k)];
        if (c == kDominated)
            childV = 1.0L;  // 被支配路径 = 100% 死
        else if (c == kUnexp)
            childV = 1.0L - (1.0L - n.dacc) * (1.0L - a.p);  // 未展开：叶子值
        else
            childV = valueAtDepth(t, c, d);
        v += a.digit[static_cast<std::size_t>(k)] * childV;
    }
    return v;
}

inline long double MidgameSearch::valueAtDepth(const Tree& t, int nodeId, int d) {
    const Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    if (n.depth >= d || !n.expanded) return n.dacc;
    long double best = 1e30L;
    for (const Action& a : n.actions)
        best = std::min(best, actionValueAtDepth(t, n, a, d));
    return best;
}

inline int MidgameSearch::subtreeMax(const Tree& t, int nodeId) {
    const Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
    int m = n.depth;
    for (const Action& a : n.actions)
        for (int k = 0; k <= 8; ++k)
            if (a.child[static_cast<std::size_t>(k)] >= 0)
                m = std::max(m, subtreeMax(t, a.child[static_cast<std::size_t>(k)]));
    return m;
}

inline int MidgameSearch::actionReach(const Tree& t, const Node& n, const Action& a) {
    int m = n.depth;
    for (int k = 0; k <= 8; ++k)
        if (a.child[static_cast<std::size_t>(k)] >= 0)
            m = std::max(m, subtreeMax(t, a.child[static_cast<std::size_t>(k)]));
    return m;
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

inline int MidgameSearch::applyNode(Tree& t, int entry, const Ctx& ctx) {
    if (t.nodes.empty() || !t.nodes[0].expanded) return -1;
    const Config& cfg = ctx.config;
    for (int attempt = 0; attempt < 64; ++attempt) {
        int cur = entry;
        int res = -1;
        for (;;) {
            const Node& n = t.nodes[static_cast<std::size_t>(cur)];
            if (n.scoreSum <= 0) return -1;  // 停流
            const int ai = sampleByWeight(n.score.data(), static_cast<int>(n.score.size()),
                                          n.scoreSum, ctx.rng);
            if (ai < 0) return -1;
            const Action& a = n.actions[static_cast<std::size_t>(ai)];
            // 观测转移：分支权重 = digit × C(子) × t(子)。
            std::array<long double, 9> bw{};
            long double bwSum = 0;
            for (int k = 0; k <= 8; ++k) {
                if (a.digit[static_cast<std::size_t>(k)] <= 0) continue;
                const int c = a.child[static_cast<std::size_t>(k)];
                long double ct = (c == kDominated)
                                     ? 0.0L
                                     : (c == kUnexp) ? 1.0L
                                                     : t.nodes[static_cast<std::size_t>(c)].t;
                long double cc = (c == kDominated)
                                     ? 0.0L
                                     : (c == kUnexp) ? 1.0L
                                                     : t.nodes[static_cast<std::size_t>(c)].C;
                if (ct <= 0 || cc <= 0) continue;
                bw[static_cast<std::size_t>(k)] = a.digit[static_cast<std::size_t>(k)] * ct * cc;
                bwSum += bw[static_cast<std::size_t>(k)];
            }
            const int k = sampleByWeight(bw.data(), 9, bwSum, ctx.rng);
            if (k < 0) return -1;
            if (a.child[static_cast<std::size_t>(k)] >= 0) {
                cur = a.child[static_cast<std::size_t>(k)];  // 下潜到已展开子树
                continue;
            }
            res = expandChild(t, cur, ai, k, ctx);  // 未展开分支：创建并展开
            break;
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
    ++t.statsNodes;
    refreshNode(t, 0, ctx.config);
}

inline MidgameSearch::Answer MidgameSearch::getAnswer(const Tree& t) {
    Answer ans;
    if (t.nodes.empty() || !t.nodes[0].expanded) return ans;
    const Node& root = t.nodes[0];
    if (root.actions.empty()) return ans;
    // 公共比较深度：各动作可达深度的最小值。
    int dMin = 1000000000;
    for (const Action& a : root.actions) dMin = std::min(dMin, actionReach(t, root, a));
    if (dMin == 1000000000) dMin = 0;
    ans.depth = dMin;
    long double best = 1e30L;
    for (const Action& a : root.actions) {
        const long double v = actionValueAtDepth(t, root, a, dMin);
        if (v < best) {
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

}  // namespace mss