#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// rational.h — rho 参数化概率函数层（RhoRational）。
//
// 把连通块的分布压缩成"以全局密度 rho 为参数"的每 box 概率函数：
//   boxProb[b](rho) = num_b(rho) / den(rho)
// 系数只依赖 shape 分布的 ways/perBoxExpectation，不依赖 rho ——
// structure/distribution 不变则系数不变；rho 变了直接 eval，O(maxD)。
//
// 这是 distribution 的下游缓存层（shape → 系数，同 DistPool 只增不删，
// 稳定地址）。它让近似引擎做到"O(1) 传输 rho、单格查询概率"的增量更新，
// 不必每次全量落格。exact 不用这个层（它全量重算，直接填数值视图）。
//
// 数据类：Box / Pool；算法类：Solver（纯空壳，从分布算系数）。
// ─────────────────────────────────────────────────────────────

struct RhoRational {
    // 每 box 概率作为 rho 的有理函数：num(rho)/den(rho)。
    // 系数已收缩到支持集 [lo, hi]（生成时约掉公共因子 rho^lo·(1-rho)^(maxD-hi)）：
    //   num/den 的下标 i 对应基 rho^i·(1-rho)^(supportLen-1-i)。
    // 收缩后两端系数必非零，eval 无 0/0 分支。
    struct Box {
        std::vector<long double> num;  // 分子基系数（长度 = 支持集长度）
        std::vector<long double> den;  // 分母基系数（长度 = 支持集长度）
        int lo = 0;                    // 支持集最低次幂（原基的下标）
    };

    // shape → boxRational 系数池。只增不删，unique_ptr 稳定地址。
    struct Pool {
        const std::vector<Box>* get(const Structure::Shape* shape);
        const std::vector<Box>* get(const Structure::Shape* shape) const;
        const std::vector<Box>* insert(const Structure::Shape* shape,
                                       std::vector<Box> boxes);

    private:
        struct Hash {
            std::size_t operator()(const Structure::Shape* p) const noexcept {
                return SplitMix64Hash{}(reinterpret_cast<std::uintptr_t>(p));
            }
        };
        std::vector<std::unique_ptr<std::vector<Box>>> boxes_;  // 稳定地址
        FlatHashTable<const Structure::Shape*, const std::vector<Box>*, Hash> index_;
    };

    // 算法类：从分布算每 box 的系数。
    struct Solver {
        // 计算并缓存（同一 shape 幂等命中）。返回池中稳定指针。
        static const std::vector<Box>* analyze(const Structure::Shape& shape,
                                               const Distribution& dist, Pool& pool);
    };

    // ── 查询（单格概率）──

    // 单 box 纯求值：num(rho)/den(rho)。O(maxD)。
    static long double eval(const Box& box, long double rho);

    // 盘面坐标查询：封装 cell → cellLoc → (cid, boxId) → 系数 → eval(rho)。
    // 非连通块：Mine→1 / Unknown→rho / Safe→0；约束数字格（已揭示）→0。
    // 惰性记忆化：首次查询某 shape 时现算分布 + 系数并缓存（幂等命中），
    // 缓存由本层自己维护，调用方无需预热。
    // 这是 UI / 搜索层拿"某格雷概率"的统一入口，引擎无关。
    static long double eval(int x, int y, const ObservedBoard& board,
                            const Basic::Result& basic, const Structure::Result& structure,
                            Distribution::DistPool& dists, Pool& pool, long double rho);
};

// ── 实现区 ──

inline long double RhoRational::eval(const Box& box, long double rho) {
    // 系数生成时已收缩：两端（i=0 和 i=len-1）必非零，无 0/0。
    const std::size_t len = box.num.size();
    assert_(len > 0 && box.den.size() == len, "RhoRational::eval: 空系数");

    // rp[k] = rho^k，op[k] = (1-rho)^k。基 = rho^k·(1-rho)^(len-1-k)。
    std::vector<long double> rp(len, 1.0L), op(len, 1.0L);
    for (std::size_t k = 1; k < len; ++k) {
        rp[k] = rp[k - 1] * rho;
        op[k] = op[k - 1] * (1.0L - rho);
    }
    long double n = 0.0L, d = 0.0L;
    for (std::size_t i = 0; i < len; ++i) {
        const long double base = rp[i] * op[len - 1 - i];
        n += box.num[i] * base;
        d += box.den[i] * base;
    }
    // den 两端非零，中间项权重非负，d 恒 > 0（除非 rho 精确在退化为单点的
    // 端点仍非零，因为两端系数非零）。这是算法不变量。
    assert_(d > 0.0L, "RhoRational::eval: 分母退化");
    return n / d;
}

inline long double RhoRational::eval(int x, int y, const ObservedBoard& board,
                                     const Basic::Result& basic,
                                     const Structure::Result& structure,
                                     Distribution::DistPool& dists, Pool& pool,
                                     long double rho) {
    const CellLocation loc = structure.cellLoc[static_cast<std::size_t>(board.id(x, y))];
    if (loc.component == -1) {
        if (basic.marks[x][y] == Basic::Mark::Mine) return 1.0L;
        if (basic.marks[x][y] == Basic::Mark::Unknown) return rho;
        return 0.0L;  // Safe / 已揭示数字
    }
    if (loc.box == -1) return 0.0L;  // 约束数字格（已揭示）
    const Structure::Instance& inst =
        structure.components[static_cast<std::size_t>(loc.component)];
    // 惰性记忆化：首次查询该 shape 时现算分布 + 系数并缓存（幂等命中即返回）。
    const Distribution* dist = Distribution::Solver::analyze(*inst.shape, dists);
    const std::vector<Box>* boxes = Solver::analyze(*inst.shape, *dist, pool);
    const Box& box = (*boxes)[static_cast<std::size_t>(loc.box)];
    // num 存的是"box 期望雷数 × 权重"，eval 得期望雷数；除以 box 大小得单格概率。
    const long double perCell = RhoRational::eval(box, rho);
    return perCell /
           static_cast<long double>(inst.shape->boxes[static_cast<std::size_t>(loc.box)].size);
}

inline const std::vector<RhoRational::Box>* RhoRational::Pool::get(
    const Structure::Shape* shape) {
    if (const std::vector<Box>** found = index_.find(shape)) return *found;
    return nullptr;
}

inline const std::vector<RhoRational::Box>* RhoRational::Pool::get(
    const Structure::Shape* shape) const {
    if (const std::vector<Box>* const* found = index_.find(shape)) return *found;
    return nullptr;
}

inline const std::vector<RhoRational::Box>* RhoRational::Pool::insert(
    const Structure::Shape* shape, std::vector<Box> boxes) {
    if (const std::vector<Box>* found = get(shape)) return found;
    boxes_.push_back(std::make_unique<std::vector<Box>>(std::move(boxes)));
    index_.emplace(shape, boxes_.back().get());
    return boxes_.back().get();
}

inline const std::vector<RhoRational::Box>* RhoRational::Solver::analyze(
    const Structure::Shape& shape, const Distribution& dist, Pool& pool) {
    if (const std::vector<Box>* cached = pool.get(&shape)) return cached;

    assert_(!dist.entries.empty(), "RhoRational::Solver::analyze: 空分布");
    int maxD = 0;
    for (const auto& e : dist.entries) maxD = std::max(maxD, e.mineCount);
    const std::size_t len = static_cast<std::size_t>(maxD) + 1;

    std::vector<Box> boxes(shape.boxes.size());
    // 先按全范围 len 累加。
    std::vector<std::vector<long double>> num(shape.boxes.size(),
                                              std::vector<long double>(len, 0.0L));
    std::vector<long double> den(len, 0.0L);

    // 每个 entry（雷数 m）贡献到基 i=m：
    //   ways·rho^m·(1-rho)^(maxD-m) = ways·(rho/(1-rho))^m·(1-rho)^maxD
    // 与 transfer 的 rho 加权一致，只是把 (rho/(1-rho))^m 显式写成基。
    for (const auto& e : dist.entries) {
        const std::size_t i = static_cast<std::size_t>(e.mineCount);
        den[i] += e.ways;
        for (std::size_t bb = 0; bb < shape.boxes.size(); ++bb)
            num[bb][i] += e.ways * e.perBoxExpectation[bb];
    }

    // 生成时收缩：按 den 的支持集 [denLo, hi] 约掉公共因子
    // rho^lo·(1-rho)^(maxD-hi)，紧凑存 base rho^(i-lo)·(1-rho)^(hi-i)。
    // 两端系数必非零，eval 无 0/0。
    int denLo = 0;
    while (denLo < static_cast<int>(len) && den[static_cast<std::size_t>(denLo)] == 0.0L)
        ++denLo;
    assert_(denLo < static_cast<int>(len), "RhoRational::Solver::analyze: 分母全零");
    for (std::size_t bb = 0; bb < shape.boxes.size(); ++bb) {
        int hi = maxD;
        while (hi >= denLo && den[static_cast<std::size_t>(hi)] == 0.0L) --hi;
        const int lo = denLo;  // den 支持集与 num 一致（都来自 ways），denLo 即最低
        const std::size_t sLen = static_cast<std::size_t>(hi - lo + 1);
        Box b;
        b.lo = lo;
        b.num.resize(sLen);
        b.den.resize(sLen);
        for (std::size_t k = 0; k < sLen; ++k) {
            b.num[k] = num[bb][static_cast<std::size_t>(lo + k)];
            b.den[k] = den[static_cast<std::size_t>(lo + k)];
        }
        boxes[bb] = std::move(b);
    }

    return pool.insert(&shape, std::move(boxes));
}

}  // namespace mss