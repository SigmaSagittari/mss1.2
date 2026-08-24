#pragma once

#include <cstdint>
#include <utility>

#include "core/utility/grid.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// types.h — core 层基础类型。
//
// 本项目所有跨层共享的"身份"与"state"都定义在这里：
//   - Cell：盘面格子的取值（数字 0-8 / Hidden）
//   - 整数 ID：CellId / ComponentId / BoxId / ShapeId / DistributionId
//   - CellLocation：格子 → 连通块 + 单位格 的归属
//   - ObservedBoard：流水线第一段 "state"（观察到的盘面）
//
// 设计约定：
//   - 身份一律是稠密 int 句柄，不用裸指针、不用平行数组 + idx 手工同步。
//     句柄生命周期由所属 Pool/Store 管理（ShapePool/DistCache 只增不删，
//     ComponentStore 用可逆日志支持 checkpoint/rollback）。
//   - core 层不含分析概念（连通块 / 分布 / 概率在 analysis 层），
//     也不含游戏规则（雷位布局 / 翻开逻辑在 game 层）。
// ─────────────────────────────────────────────────────────────

// 盘面格子：0-8 为已翻开的数字，Hidden 为未翻开。
enum class Cell : int {
    Num0 = 0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Hidden = 9,
};

inline bool isNumber(Cell c) {
    int v = static_cast<int>(c);
    return v >= 0 && v <= 8;
}

inline int numberValue(Cell c) {
    return static_cast<int>(c);
}

// ── 整数身份 ──
// 稠密整数句柄，全部以 vector 下标形式存储与传递，索引越界即 bug。
using CellId = int;          // 棋盘格：x*(cols+1)+y（与 Grid 内部下标一致，可直接索引 cellLoc）
using ComponentId = int;     // 连通块实例（ComponentStore 句柄）
using BoxId = int;           // 单位格（ComponentShape 内局部下标，0..boxes.size()-1）
using ShapeId = int;         // interned 不可变结构（ShapePool 句柄）
using DistributionId = int;  // 分布缓存句柄（DistCache 句柄）

// 格子 → (所属连通块, shape 内单位格下标)。
// 不在任何连通块的格子（Safe/Mine/Unknown）component = -1。
struct CellLocation {
    ComponentId component = -1;
    BoxId box = -1;
};

// 遍历 (x, y) 的 8 个邻居，fn 收到的都是合法坐标。
template <typename Func>
inline void forEachAdjacent(int x, int y, int rows, int cols, Func&& fn) {
    bool up = x > 1;
    bool down = x < rows;
    bool left = y > 1;
    bool right = y < cols;

    if (up && left) fn(x - 1, y - 1);
    if (up) fn(x - 1, y);
    if (up && right) fn(x - 1, y + 1);
    if (left) fn(x, y - 1);
    if (right) fn(x, y + 1);
    if (down && left) fn(x + 1, y - 1);
    if (down) fn(x + 1, y);
    if (down && right) fn(x + 1, y + 1);
}

// state：流水线第一段。观察到的盘面（数字/Hidden）+ 总雷数。
// game 层从真实雷位生成它，analysis 层只消费它，不感知游戏规则。
// 注意：flags（玩家标旗）是游戏层 UI 猜测，分析一律视作未揭示，不进这里。
struct ObservedBoard {
    int rows = 0;
    int cols = 0;
    int totalMines = 0;
    Grid<Cell> board;

    ObservedBoard() = default;

    ObservedBoard(int r, int c, int m)
        : rows(r), cols(c), totalMines(m), board(r, c, Cell::Hidden) {}

    // 坐标 → 稠密 CellId。x*(cols+1)+y 与 Grid 存储布局一致，
    // 因此 CellId 可直接作为 cellLoc 等数组的下标。
    CellId id(int x, int y) const {
        return x * (cols + 1) + y;
    }

    // CellId → 坐标（x*(cols+1)+y 的反解）。
    std::pair<int, int> pos(CellId cell) const {
        return {cell / (cols + 1), cell % (cols + 1)};
    }
};

}  // namespace mss