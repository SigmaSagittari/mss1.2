#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// structure.h — 盘面图论结构。
//
// 类型全部嵌套在 Structure 命名空间下：
//   数据类：Shape / Instance / Result / Delta
//   池：    ShapePool（interned 不可变 Shape，只增不删）
//   算法类：Analyzer / Updater（纯空壳，无成员、零开销）
//
// 核心拆分：
//   - Shape：interned、不可变、无棋盘坐标，hash = 身份键。
//     同构连通块跨盘面共享一份，分布按 shape 去重（Distribution 层挂
//     第二个池 const Shape* → Distribution，不进 Instance）。
//   - Instance：每份 Result 各一份，持 Shape 观察指针 + 本盘面位置数据。
//
// 层间（吃 DAG，非只吃上层 delta）：Analyzer/Updater 直接读
// board + Basic::Result 当前状态；Updater 就地改 Result 只碰脏块，
// 返回 Delta 供 undo journal。不做 swap-pop，ComponentId 墓碑式保留。
// ─────────────────────────────────────────────────────────────

struct Structure {
    // ── 数据类 ──

    // interned 不可变连通块形状。无棋盘坐标，hash 是内容指纹（身份键）。
    struct Shape {
        // 单位格规格：数字邻域集合相同的隐藏格组。只含 size，无位置。
        struct Box {
            int size = 0;
        };

        // 约束：一个数字格，要求其邻接单位格的雷数总和等于 sum。
        // boxIds 是 shape 内局部下标（0..boxes.size()-1）。
        struct Constraint {
            int sum = 0;
            std::vector<BoxId> boxIds;
        };

        std::vector<Box> boxes;
        std::vector<Constraint> constraints;
        U128 hash = {};  // 内容指纹（ShapePool 去重依据）
    };

    // 每盘面的连通块实例：引用 interned shape + 本盘面位置数据。
    struct Instance {
        // 扁平存储的性能优化：所有 box 的格子压进一个数组，用 boxOf 前缀和
        // 定位各 box 区间。避免 vector<vector<CellId>> 的嵌套分配与间接寻址。
        // 语义等价 vector<vector<CellId>>；对外只暴露 box 区间访问。
        struct Boxes {
            std::vector<CellId> cells;  // 全部格子，按 box 顺序扁平排列
            // boxOf 前缀和：第 b 个 box 的格子区间是 [boxOf[b], boxOf[b+1])。
            // 名字取 "offsets of boxes" 之义。
            std::vector<std::uint16_t> boxOf;

            std::size_t count() const { return boxOf.empty() ? 0 : boxOf.size() - 1; }
            std::size_t cellCount(std::size_t b) const {
                return boxOf[b + 1] - boxOf[b];
            }
        };

        const Shape* shape = nullptr;  // interned 形状（观察指针，池只增不删不悬垂）
        Boxes boxes;                   // 本盘面单位格的格子
        // 约束数字格，顺序与 shape.constraints 一致。
        std::vector<CellId> constraintCells;
        bool alive = true;  // false = 墓碑：已从结构摘除，数据保留（undo 可恢复）
    };

    // structure 段输出：全部连通块实例 + 格子 → 位置映射。
    struct Result {
        std::vector<Instance> components;   // 下标即 ComponentId
        std::vector<CellLocation> cellLoc;  // 按 CellId 索引
    };

    // 一次增量更新的变更集合（供 UndoJournal / UI 增量消费）。
    struct Delta {
        std::vector<ComponentId> removed;  // 被摘除的连通块（墓碑保留，不移动）
        std::vector<ComponentId> added;    // 新重建的连通块
    };

    // ── 池 ──

    // 结构池：按 U128 hash 去重，只增不删。unique_ptr 保证 Shape 地址稳定，
    // 观察指针永不悬垂（池生命周期 = AnalysisContext 生命周期）。
    struct ShapePool {
        // 已存在同 hash 结构时返回既有指针；否则插入并返回新指针。
        const Shape* intern(Shape shape);

    private:
        // unique_ptr 保证 Shape 堆地址稳定；index_ 按 hash 反查 ShapeId。
        std::vector<std::unique_ptr<Shape>> shapes_;
        FlatHashTable<U128, ShapeId, U128Hash> index_;
    };

    // ── 算法类 ──

    struct Analyzer {
        // 全量构建：从 board + basic 标记推导全部连通块，intern 形状，写回 cellLoc。
        static Result analyze(const ObservedBoard& board, const Basic::Result& basic,
                              ShapePool& pool);
    };

    struct Updater {
        // 增量更新：就地修改 result（只碰受影响连通块，无整盘拷贝）。
        // 前置条件：board 与 basic 已被外部更新为揭示后的状态，
        // updates 说明哪些格子变了、变为什么（定位脏区）。
        static Delta update(const ObservedBoard& board, const Basic::Result& basic,
                            Result& result, ShapePool& pool,
                            const std::vector<Basic::Update>& updates);
    };

    // ── 实现区 ──

private:
    // 收集一个连通块的所有格子（数字格 + 前沿格）。
    static void collectComponent(int x, int y, const ObservedBoard& state,
                                 const Basic::Result& basic, Grid<char>& vis,
                                 std::vector<std::pair<int, int>>& cells);

    // 根据一个连通块的格子列表，构造它的 shape + 实例（intern 进 pool）。
    static Instance buildComponent(const std::vector<std::pair<int, int>>& cells,
                                   const ObservedBoard& state,
                                   const Basic::Result& basic, Grid<U128>& cellHash,
                                   ShapePool& pool);

    // 连通块结构哈希（128 位）：以 boxes / constraints 内容为指纹。
    static U128 computeHash(const Shape& shape);
};

// ── 实现区 ──

inline const Structure::Shape* Structure::ShapePool::intern(Shape shape) {
    shape.hash = Structure::computeHash(shape);
    if (const ShapeId* found = index_.find(shape.hash))
        return shapes_[static_cast<std::size_t>(*found)].get();
    const ShapeId id = static_cast<ShapeId>(shapes_.size());
    shapes_.push_back(std::make_unique<Shape>(std::move(shape)));
    index_.emplace(shapes_[static_cast<std::size_t>(id)]->hash, id);
    return shapes_[static_cast<std::size_t>(id)].get();
}

inline Structure::Result Structure::Analyzer::analyze(const ObservedBoard& state,
                                                      const Basic::Result& basic,
                                                      ShapePool& pool) {
    using Mark = Basic::Mark;
    const int rows = state.rows;
    const int cols = state.cols;
    Result result;
    result.cellLoc.assign(static_cast<std::size_t>(rows + 1) * (cols + 1),
                          CellLocation{});

    // 线程局部复用缓冲区，避免反复分配。
    thread_local Grid<char> vis;
    thread_local Grid<U128> cellHash;
    thread_local std::vector<std::pair<int, int>> cells;
    if (vis.rows() != rows || vis.cols() != cols) {
        vis.resize(rows, cols, 0);
        cellHash.resize(rows, cols, U128{});
        cells.reserve(static_cast<std::size_t>(rows * cols / 2));
    } else {
        vis.fill(0);
        cellHash.fill(U128{});
    }

    // 1. 给数字格周围的未开格累加哈希值：哈希相同 = 属于同一单位格。
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (isNumber(state.board[i][j])) {
                const std::uint64_t pos =
                    static_cast<std::uint64_t>(i) * (cols + rows + 3) + j;
                const U128 seed{splitmix64(pos),
                                splitmix64(pos + 0x9e3779b97f4a7c15ULL)};
                forEachAdjacent(i, j, rows, cols, [&](int nx, int ny) {
                    cellHash[nx][ny] += seed;
                });
            }

    // 2. 从未访问的前沿格出发，收集所有连通块。
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (basic.marks[i][j] == Mark::Frontier && !vis[i][j]) {
                cells.clear();
                collectComponent(i, j, state, basic, vis, cells);
                result.components.push_back(buildComponent(cells, state, basic, cellHash, pool));
            }

    // 3. 回填格子 → 位置映射。
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(result.components.size());
         ++cid) {
        const Instance& inst = result.components[static_cast<std::size_t>(cid)];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[static_cast<std::size_t>(inst.boxes.cells[k])] =
                    CellLocation{cid, static_cast<BoxId>(b)};
        for (CellId c : inst.constraintCells)
            result.cellLoc[static_cast<std::size_t>(c)] = CellLocation{cid, -1};
    }

    return result;
}

inline void Structure::collectComponent(int x, int y, const ObservedBoard& state,
                                        const Basic::Result& basic, Grid<char>& vis,
                                        std::vector<std::pair<int, int>>& cells) {
    using Mark = Basic::Mark;
    auto dfs = [&](auto&& self, int cx, int cy) -> void {
        if (vis[cx][cy]) return;
        vis[cx][cy] = 1;
        cells.emplace_back(cx, cy);

        if (isNumber(state.board[cx][cy])) {
            forEachAdjacent(cx, cy, state.rows, state.cols, [&](int nx, int ny) {
                if (basic.marks[nx][ny] == Mark::Frontier) self(self, nx, ny);
            });
        }
        if (basic.marks[cx][cy] == Mark::Frontier) {
            forEachAdjacent(cx, cy, state.rows, state.cols, [&](int nx, int ny) {
                if (isNumber(state.board[nx][ny])) self(self, nx, ny);
            });
        }
    };
    dfs(dfs, x, y);
}

inline Structure::Instance Structure::buildComponent(
    const std::vector<std::pair<int, int>>& cells, const ObservedBoard& state,
    const Basic::Result& basic, Grid<U128>& cellHash, ShapePool& pool) {
    using Mark = Basic::Mark;
    Instance inst;
    const int rows = state.rows;
    const int cols = state.cols;

    // 1. 收集所有不同的哈希值 → 单位格（哈希相同 = 同一单位格）。
    std::vector<U128> hashList;
    hashList.reserve(cells.size());
    for (auto [x, y] : cells) hashList.push_back(cellHash[x][y]);
    std::sort(hashList.begin(), hashList.end());
    hashList.erase(std::unique(hashList.begin(), hashList.end()), hashList.end());

    std::vector<int> hashUsed(hashList.size(), -1);
    std::vector<BoxId> boxOfCells(cells.size(), -1);
    Shape shape;

    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const auto [x, y] = cells[ci];
        if (basic.marks[x][y] != Mark::Frontier) continue;
        const int hv = static_cast<int>(
            std::lower_bound(hashList.begin(), hashList.end(), cellHash[x][y]) -
            hashList.begin());
        if (hashUsed[hv] == -1) {
            hashUsed[hv] = static_cast<int>(shape.boxes.size());
            shape.boxes.push_back({0});
        }
        const BoxId boxId = static_cast<BoxId>(hashUsed[hv]);
        shape.boxes[static_cast<std::size_t>(boxId)].size++;
        boxOfCells[ci] = boxId;
        // 复用 cellHash：改为保存 格子 → 单位格 id。
        cellHash[x][y] = U128{static_cast<std::uint64_t>(boxId), 0};
    }

    // 2. 按 box 顺序扁平收集格子（桶收集，O(C)，替代 box×cells 双循环）。
    std::vector<std::vector<CellId>> buckets(shape.boxes.size());
    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const BoxId b = boxOfCells[ci];
        if (b == -1) continue;  // 数字格无单位格归属
        buckets[static_cast<std::size_t>(b)].push_back(
            state.id(cells[ci].first, cells[ci].second));
    }
    inst.boxes.boxOf.push_back(0);
    for (std::size_t b = 0; b < buckets.size(); ++b) {
        inst.boxes.cells.insert(inst.boxes.cells.end(), buckets[static_cast<std::size_t>(b)].begin(),
                                buckets[static_cast<std::size_t>(b)].end());
        inst.boxes.boxOf.push_back(static_cast<std::uint16_t>(inst.boxes.cells.size()));
    }

    // 3. 数字格 → 约束。
    std::vector<char> boxUsed(shape.boxes.size(), 0);
    for (auto [x, y] : cells) {
        if (!isNumber(state.board[x][y])) continue;
        Shape::Constraint c;
        c.sum = numberValue(state.board[x][y]);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (basic.marks[nx][ny] == Mark::Mine) c.sum--;
            if (basic.marks[nx][ny] == Mark::Frontier) {
                const BoxId boxId = static_cast<BoxId>(cellHash[nx][ny].lo);
                if (!boxUsed[static_cast<std::size_t>(boxId)]) {
                    boxUsed[static_cast<std::size_t>(boxId)] = 1;
                    c.boxIds.push_back(boxId);
                }
            }
        });
        for (BoxId id : c.boxIds) boxUsed[static_cast<std::size_t>(id)] = 0;
        shape.constraints.push_back(std::move(c));
        inst.constraintCells.push_back(state.id(x, y));
    }

    // 4. intern 形状。
    inst.shape = pool.intern(std::move(shape));
    return inst;
}

inline U128 Structure::computeHash(const Shape& shape) {
    U128Hasher h;
    for (const auto& box : shape.boxes)
        h.mix(static_cast<std::uint64_t>(box.size));
    for (const auto& limit : shape.constraints) {
        h.mix(static_cast<std::uint64_t>(limit.sum));
        for (BoxId id : limit.boxIds)
            h.mix(static_cast<std::uint64_t>(id) + 0x9e3779b9ULL);
    }
    return h.finalize();
}

inline Structure::Delta Structure::Updater::update(const ObservedBoard& state,
                                                   const Basic::Result& basic,
                                                   Result& result, ShapePool& pool,
                                                   const std::vector<Basic::Update>& updates) {
    using Mark = Basic::Mark;
    Delta delta;
    const int rows = state.rows;
    const int cols = state.cols;

    // 线程局部复用缓冲区：尺寸变化时 resize；否则由函数末尾手动清零。
    static thread_local Grid<char> dirty;
    static thread_local Grid<char> vis;
    static thread_local Grid<U128> cellHash;
    static thread_local std::vector<std::pair<int, int>> dirtyCells;
    static thread_local std::vector<std::pair<int, int>> cells;
    if (dirty.rows() != rows || dirty.cols() != cols) {
        dirty.resize(rows, cols, 0);
        vis.resize(rows, cols, 0);
        cellHash.resize(rows, cols, U128{});
        cells.reserve(static_cast<std::size_t>(rows * cols / 2));
    }

    // 标记某格为脏：去重，并记录位置到 dirtyCells。
    auto markDirty = [&](int x, int y) {
        if (dirty[x][y]) return;
        dirty[x][y] = 1;
        dirtyCells.emplace_back(x, y);
    };
    // 摘除某格的结构归属。
    auto clearCellLoc = [&](int x, int y) {
        result.cellLoc[static_cast<std::size_t>(state.id(x, y))] = CellLocation{};
    };

    // 1. 值事件格 + 八邻域全部标脏（唯一的脏信号源）。
    for (const Basic::Update& u : updates) {
        const auto [x, y] = state.pos(u.cell);
        markDirty(x, y);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) { markDirty(nx, ny); });
    }

    // 2. 通过 cellLoc 反查脏格所属连通块，整个连通块作废（墓碑摘除）。
    //    dirtyCells 在本步骤开始前的内容只来自值事件和八邻域；
    //    遍历固定前缀，避免 markDirty 追加元素时使迭代器失效。
    const std::size_t initialDirtyCount = dirtyCells.size();
    for (std::size_t i = 0; i < initialDirtyCount; ++i) {
        const auto [x, y] = dirtyCells[i];
        const CellLocation loc = result.cellLoc[static_cast<std::size_t>(state.id(x, y))];
        if (loc.component == -1) continue;
        Instance& inst = result.components[static_cast<std::size_t>(loc.component)];
        if (!inst.alive) continue;

        // 连通块是不可拆分的更新单位：命中一格，整块纳入重建。
        // 先标脏并清掉旧归属，避免后续 dirtyCells 读到已摘除的组件。
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [cx, cy] = state.pos(inst.boxes.cells[k]);
                markDirty(cx, cy);
                clearCellLoc(cx, cy);
            }
        for (CellId c : inst.constraintCells) {
            const auto [cx, cy] = state.pos(c);
            markDirty(cx, cy);
            clearCellLoc(cx, cy);
        }
        inst.alive = false;  // 墓碑保留，数据不删，undo 可恢复
        delta.removed.push_back(loc.component);
    }

    // 3. 重建脏区域：只遍历 dirtyCells，从每个未访问的前沿脏格出发重建连通块。
    //    cellHash 只给本连通块涉及的格子算：格子向周围数字"索取"种子
    //    （与 build() 的贡献方向相反、结果一致），不再整盘扫描数字。
    auto hashAt = [&](int x, int y) {
        U128 h;
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (isNumber(state.board[nx][ny])) {
                const std::uint64_t pos =
                    static_cast<std::uint64_t>(nx) * (cols + rows + 3) + ny;
                h += U128{splitmix64(pos), splitmix64(pos + 0x9e3779b97f4a7c15ULL)};
            }
        });
        return h;
    };
    int newIdx = static_cast<int>(result.components.size());
    for (auto [x, y] : dirtyCells) {
        if (basic.marks[x][y] != Mark::Frontier || vis[x][y]) continue;
        cells.clear();
        collectComponent(x, y, state, basic, vis, cells);
        for (auto [cx, cy] : cells) cellHash[cx][cy] = hashAt(cx, cy);
        result.components.push_back(buildComponent(cells, state, basic, cellHash, pool));
        delta.added.push_back(newIdx);

        // 回填 cellLoc。
        const Instance& inst = result.components[static_cast<std::size_t>(newIdx)];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[static_cast<std::size_t>(inst.boxes.cells[k])] =
                    CellLocation{newIdx, static_cast<BoxId>(b)};
        for (CellId c : inst.constraintCells)
            result.cellLoc[static_cast<std::size_t>(c)] = CellLocation{newIdx, -1};
        ++newIdx;
    }

    // 4. 手动清零工作区（只清被用过的格子），下次调用无需整盘初始化。
    for (auto [x, y] : dirtyCells) {
        dirty[x][y] = 0;
        vis[x][y] = 0;
        cellHash[x][y] = U128{};
    }
    dirtyCells.clear();

    return delta;
}

}  // namespace mss