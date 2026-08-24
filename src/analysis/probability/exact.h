#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// exact.h — 精确概率引擎。
//
// 把每个连通块的分布写成生成函数（Polynomial），乘起来得到所有连通块的
// 联合分布，再用组合数把"非前沿格"的雷数纳入，得到每个格子的精确雷概率。
// 无近似、无 rho；是残局/根节点的无偏参考实现。
//
// 内部细节（Polynomial、阶乘表、binom）全部为 Exact 私有，不暴露。
// ─────────────────────────────────────────────────────────────

struct Exact {
    // 精确分析：吃活组件 + 分布池，输出可查询的 Result。
    static Probability::Result analyze(const ObservedBoard& board,
                                       const Basic::Result& basic,
                                       const Structure::Result& structure,
                                       Distribution::DistPool& pool);

    // 观察：点开格子 cell 的结果分布（爆炸概率 + 数字 0..8 概率）。
    // 边界：cell 已标 Mine → explosion=1；已揭示/数字格 → 全 0。
    // 算法：对每活连通块 forEachAssignment 枚举 box 雷数分配，每分配对
    // "邻域雷数贡献"做 box 超几何卷积 → 二维块贡献 (h, r)，跨块卷积后
    // 按 T 格组合补足 → explosion / digit[0..8]。
    static Probability::ObserveResult observe(const ObservedBoard& board,
                                              const Basic::Result& basic,
                                              const Structure::Result& structure,
                                              Distribution::DistPool& pool, CellId cell);

private:
    // 阶乘对数表（线程局部，惰性扩展）。
    static std::vector<long double>& logFactorial();
    static void combiInit(int n);
    // 组合数（用阶乘对数避免溢出）。调用方保证 0<=k<=n；越界=调用 bug。
    static long double combLog(int n, int k);

    // 生成函数多项式：start 表示最低次幂，coeffs[i] 是 x^(start+i) 的系数。
    struct Polynomial {
        int start = 0;
        std::vector<long double> coeffs;

        Polynomial() = default;
        Polynomial(int s, std::vector<long double> c) : start(s), coeffs(std::move(c)) {}

        explicit Polynomial(const Distribution& dist);
        Polynomial operator*(const Polynomial& other) const;
        Polynomial operator/(const Polynomial& other) const;
    };

    // 候选方案数：把非前沿格的雷数组合也纳入。
    static long double denominator(const Polynomial& gf, int totalMines, int tSum);
};

// ── 实现区 ──

inline std::vector<long double>& Exact::logFactorial() {
    static thread_local std::vector<long double> table;
    return table;
}

inline void Exact::combiInit(int n) {
    auto& t = logFactorial();
    if (t.empty()) t.push_back(0.0);
    while (static_cast<int>(t.size()) <= n + 1)
        t.push_back(t.back() + std::log(static_cast<long double>(t.size())));
}

inline long double Exact::combLog(int n, int k) {
    // 调用方（denominator/analyze）均先检查 lightMines 在 [0, tSum] 内；
    // 越界到达这里 = 内部 bug。
    assert_(k >= 0 && k <= n, "Exact::combLog: 参数越界");
    if (k == 0 || k == n) return 1;
    combiInit(n);
    return std::exp(logFactorial()[static_cast<std::size_t>(n)] -
                    logFactorial()[static_cast<std::size_t>(k)] -
                    logFactorial()[static_cast<std::size_t>(n - k)]);
}

inline Exact::Polynomial::Polynomial(const Distribution& dist) {
    // 活组件的分布必有可行雷数（waySum > 0）；空分布 = shape/分布层 bug。
    assert_(!dist.entries.empty(), "Exact::Polynomial: 空分布");
    int minExp = dist.entries[0].mineCount;
    int maxExp = minExp;
    for (const auto& d : dist.entries) {
        minExp = std::min(minExp, d.mineCount);
        maxExp = std::max(maxExp, d.mineCount);
    }
    std::vector<long double> c(static_cast<std::size_t>(maxExp - minExp + 1), 0.0);
    for (const auto& d : dist.entries)
        c[static_cast<std::size_t>(d.mineCount - minExp)] = d.ways;
    *this = Polynomial(minExp, std::move(c));
}

inline Exact::Polynomial Exact::Polynomial::operator*(const Polynomial& other) const {
    const int start = this->start + other.start;
    const int size = static_cast<int>(coeffs.size()) +
                     static_cast<int>(other.coeffs.size()) - 1;
    std::vector<long double> res(static_cast<std::size_t>(size), 0.0);
    for (int i = 0; i < static_cast<int>(coeffs.size()); ++i)
        for (int j = 0; j < static_cast<int>(other.coeffs.size()); ++j)
            res[static_cast<std::size_t>(i + j)] +=
                coeffs[static_cast<std::size_t>(i)] *
                other.coeffs[static_cast<std::size_t>(j)];
    return Polynomial(start, std::move(res));
}

inline Exact::Polynomial Exact::Polynomial::operator/(const Polynomial& other) const {
    const int start = this->start - other.start;
    const int size = std::max(
        0, static_cast<int>(coeffs.size()) - static_cast<int>(other.coeffs.size()) + 1);
    std::vector<long double> res(static_cast<std::size_t>(size), 0.0);
    std::vector<long double> rem(coeffs);
    const long double otherLeading = other.coeffs.back();
    for (int i = static_cast<int>(rem.size()) - 1;
         i >= static_cast<int>(other.coeffs.size()) - 1; --i) {
        if (std::abs(rem[static_cast<std::size_t>(i)]) < 1e-12L) continue;
        const long double factor = rem[static_cast<std::size_t>(i)] / otherLeading;
        const int quotIdx = i - (static_cast<int>(other.coeffs.size()) - 1);
        res[static_cast<std::size_t>(quotIdx)] = factor;
        for (int j = 0; j < static_cast<int>(other.coeffs.size()); ++j)
            rem[static_cast<std::size_t>(i - j)] -=
                factor * other.coeffs[other.coeffs.size() - 1 - static_cast<std::size_t>(j)];
    }
    return Polynomial(start, std::move(res));
}

inline long double Exact::denominator(const Polynomial& gf, int totalMines, int tSum) {
    long double result = 0.0;
    for (int i = 0; i < static_cast<int>(gf.coeffs.size()); ++i) {
        const int heavyMines = gf.start + i;
        const int lightMines = totalMines - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum)
            result += gf.coeffs[static_cast<std::size_t>(i)] * combLog(tSum, lightMines);
    }
    return result;
}

inline Probability::Result Exact::analyze(const ObservedBoard& board,
                                          const Basic::Result& basic,
                                          const Structure::Result& structure,
                                          Distribution::DistPool& pool) {
    const int M = board.totalMines - basic.mineSum;
    const int tSum = basic.unknownSum;

    // 活组件（跳过墓碑）：收集分布，对齐到 Result.components（下标 = ComponentId）。
    Probability::Result result;
    result.components.resize(structure.components.size());

    std::vector<const Distribution*> distList;
    std::vector<ComponentId> aliveIds;
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        if (!inst.alive) continue;
        aliveIds.push_back(cid);
        // 确保分布存在：计算并缓存（同一 shape 幂等命中）。
        distList.push_back(Distribution::Solver::analyze(*inst.shape, pool));
    }

    // 全块联合生成函数。
    Polynomial pH(0, {1.0});
    for (const auto* d : distList) pH = pH * Polynomial(*d);

    const long double denom = denominator(pH, M, tSum);

    // Unknown 格是雷的概率：总雷数 - 1 分配给其余格子。
    long double lightProb = 0.0;
    for (int i = 0; i < static_cast<int>(pH.coeffs.size()); ++i) {
        const int heavyMines = pH.start + i;
        const int lightMines = M - 1 - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum - 1)
            lightProb += pH.coeffs[static_cast<std::size_t>(i)] * combLog(tSum - 1, lightMines);
    }
    lightProb /= denom;
    result.tCellProbability = lightProb;
    result.candidates = denom;

    // 每连通块：各分布的取到概率 → 每 box 的雷概率（均摊）。
    for (std::size_t ai = 0; ai < aliveIds.size(); ++ai) {
        const ComponentId cid = aliveIds[ai];
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        const Distribution& dist = *distList[ai];
        const Polynomial pi(dist);
        const Polynomial ti = pH / pi;  // 去掉第 i 块后的联合分布

        std::vector<long double> boxProb(dist.entries.size(), 0.0L);
        for (std::size_t j = 0; j < dist.entries.size(); ++j) {
            const int v = dist.entries[j].mineCount;
            const long double w = dist.entries[j].ways;
            long double numerator = 0.0L;
            for (int k = 0; k < static_cast<int>(ti.coeffs.size()); ++k) {
                const int tMines = ti.start + k;
                const int lightMines = M - v - tMines;
                if (lightMines >= 0 && lightMines <= tSum)
                    numerator += w * ti.coeffs[static_cast<std::size_t>(k)] *
                                 combLog(tSum, lightMines);
            }
            boxProb[j] = numerator / denom;
        }

        // 每 box 期望 × 取到概率 → 均摊到单格。
        auto& cr = result.components[static_cast<std::size_t>(cid)];
        const Structure::Shape& shape = *inst.shape;
        cr.boxProbs.resize(shape.boxes.size());
        for (std::size_t b = 0; b < shape.boxes.size(); ++b) {
            long double perCell = 0.0L;
            for (std::size_t j = 0; j < dist.entries.size(); ++j)
                perCell += boxProb[j] * dist.entries[j].perBoxExpectation[b];
            cr.boxProbs[b] = perCell / static_cast<long double>(shape.boxes[b].size);
        }
    }

    return result;
}

}  // namespace mss