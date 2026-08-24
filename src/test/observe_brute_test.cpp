#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "ui/game_control.h"

using namespace mss;
using ld = long double;

static ld comb(int n, int k) {
    if (k < 0 || k > n) return 0.0L;
    return std::exp(std::lgamma((ld)n + 1) - std::lgamma((ld)k + 1) -
                    std::lgamma((ld)(n - k) + 1));
}

static int gFail = 0;
static long long gCheck = 0;
static long long gCells = 0;
// 特殊路径覆盖统计
static long long covFixed = 0;    // x 有 Mine 标记邻居（digit 平移）
static long long covSize1 = 0;    // x 在 size-1 box
static long long covSafe = 0;     // x 为 Safe 标记的隐藏格
static long long covT = 0;        // x 为 T 格（Unknown）
static long long covBox = 0;      // x 在前沿 box
static long long covUT = 0;       // x 有 T 邻居（uT>0）
static long long covMineCell = 0; // x 为 Mine 标记（explosion=1）

static void check(bool ok, const char* what) {
    ++gCheck;
    if (!ok) {
        ++gFail;
        std::printf("  FAIL: %s\n", what);
    }
}

struct Counts {
    std::array<long long, 9> digit{};
    long long explosion = 0;
    long long total = 0;
};

// 对单个隐藏格：记录覆盖统计。
static void cover(const ObservedBoard& board, const Basic::Result& basic,
                  const Structure::Result& structure, int x, int y) {
    const CellId c = board.id(x, y);
    const auto& marks = basic.marks;
    const auto loc = structure.cellLoc[static_cast<std::size_t>(c)];
    if (marks[x][y] == Basic::Mark::Mine) ++covMineCell;
    else if (marks[x][y] == Basic::Mark::Unknown) ++covT;
    else if (loc.component >= 0) {
        ++covBox;
        const auto& inst = structure.components[static_cast<std::size_t>(loc.component)];
        if (inst.shape->boxes[static_cast<std::size_t>(loc.box)].size == 1) ++covSize1;
    } else ++covSafe;
    {
        bool hasMineN = false, hasTN = false;
        forEachAdjacent(x, y, board.rows, board.cols, [&](int nx, int ny) {
            if (marks[nx][ny] == Basic::Mark::Mine) hasMineN = true;
            if (marks[nx][ny] == Basic::Mark::Unknown) hasTN = true;
        });
        if (hasMineN) ++covFixed;
        if (hasTN) ++covUT;
    }
}

// Enumerate all mine subsets of size totalMines consistent with the observed
// numbers on a small board.
static void bruteSmall(const ObservedBoard& board, std::vector<Counts>& counts) {
    const int rows = board.rows, cols = board.cols;
    const int n = rows * cols;
    std::vector<CellId> cells;
    cells.reserve(static_cast<std::size_t>(n));
    for (int x = 1; x <= rows; ++x)
        for (int y = 1; y <= cols; ++y)
            cells.push_back(board.id(x, y));

    std::vector<char> sel(n, 0);
    for (int i = n - board.totalMines; i < n; ++i) sel[static_cast<std::size_t>(i)] = 1;
    do {
        Grid<char> mine(rows, cols, 0);
        bool bad = false;
        for (int i = 0; i < n; ++i) {
            if (!sel[static_cast<std::size_t>(i)]) continue;
            const int x = cells[static_cast<std::size_t>(i)] / (cols + 1);
            const int y = cells[static_cast<std::size_t>(i)] % (cols + 1);
            if (board.board[x][y] != Cell::Hidden) { bad = true; break; }
            mine[x][y] = 1;
        }
        if (bad) continue;
        bool ok = true;
        for (int x = 1; x <= rows && ok; ++x)
            for (int y = 1; y <= cols && ok; ++y) {
                if (board.board[x][y] == Cell::Hidden) continue;
                int cnt = 0;
                forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) { cnt += mine[nx][ny]; });
                if (cnt != numberValue(board.board[x][y])) ok = false;
            }
        if (!ok) continue;
        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y) {
                if (board.board[x][y] != Cell::Hidden) continue;
                Counts& ct = counts[static_cast<std::size_t>(board.id(x, y))];
                ++ct.total;
                if (mine[x][y]) { ++ct.explosion; continue; }
                int cnt = 0;
                forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) { cnt += mine[nx][ny]; });
                ++ct.digit[static_cast<std::size_t>(cnt)];
            }
    } while (std::next_permutation(sel.begin(), sel.end()));
}

// 对整盘做"observe vs 穷举"核对。
static void verifyObserved(const ObservedBoard& board) {
    const Basic::Result basic = Basic::Analyzer::analyze(board);
    if (!basic.valid) return;
    Structure::ShapePool shapes;
    const Structure::Result structure = Structure::Analyzer::analyze(board, basic, shapes);
    Distribution::DistPool pool;
    const Probability::Result prob = Exact::analyze(board, basic, structure, pool);

    std::vector<Counts> counts(
        static_cast<std::size_t>((board.rows + 1) * (board.cols + 1)));
    bruteSmall(board, counts);

    for (int x = 1; x <= board.rows; ++x)
        for (int y = 1; y <= board.cols; ++y) {
            if (board.board[x][y] != Cell::Hidden) continue;
            cover(board, basic, structure, x, y);
            const CellId c = board.id(x, y);
            const auto r = Exact::observe(board, basic, structure, prob, pool, c);
            const Counts& ct = counts[static_cast<std::size_t>(c)];
            if (ct.total == 0) continue;
            char buf[160];
            std::snprintf(buf, sizeof buf, "(%d,%d) explosion (total=%lld)", x, y, ct.total);
            check(std::abs(r.explosion - static_cast<ld>(ct.explosion) / ct.total) < 1e-9L, buf);
            for (int k = 0; k <= 8; ++k) {
                std::snprintf(buf, sizeof buf, "(%d,%d) digit[%d]", x, y, k);
                check(std::abs(r.digit[static_cast<std::size_t>(k)] -
                               static_cast<ld>(ct.digit[static_cast<std::size_t>(k)]) /
                                   ct.total) < 1e-9L, buf);
            }
            ++gCells;
        }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ── 1. 手工构造板 ──
    std::printf("== 1. 手工板 ==\n");
    {
        // 4x4 / 4，全部隐藏（解析超几何对照）
        ObservedBoard b(4, 4, 4);
        verifyObserved(b);
        // 4x4 / 4，中心开 1
        ObservedBoard b2(4, 4, 4);
        b2.board[2][2] = Cell::Num1;
        verifyObserved(b2);
        // 4x4 / 5，中心开 3（周边三雷，有 Mine 标记路径）
        ObservedBoard b3(4, 4, 5);
        b3.board[2][2] = Cell::Num3;
        verifyObserved(b3);
        // 5x5 / 6，两个相邻数字（共享前沿格：box + 双约束）
        ObservedBoard b4(5, 5, 6);
        b4.board[2][2] = Cell::Num2;
        b4.board[2][3] = Cell::Num3;
        verifyObserved(b4);
        // 5x5 / 4，两个分离组件
        ObservedBoard b5(5, 5, 4);
        b5.board[2][2] = Cell::Num1;
        b5.board[4][4] = Cell::Num1;
        verifyObserved(b5);
        // 6x6 / 8，三数字链
        ObservedBoard b6(6, 6, 8);
        b6.board[2][2] = Cell::Num2;
        b6.board[3][2] = Cell::Num1;
        b6.board[3][3] = Cell::Num2;
        verifyObserved(b6);
        // 5x5 / 5：Safe 标记路径（(1,1)=3 逼出 3 个 Mine，(1,2)=2 已满足 → (1,3)(2,3) Safe）
        ObservedBoard b7(5, 5, 5);
        b7.board[1][1] = Cell::Num3;
        b7.board[1][2] = Cell::Num2;
        verifyObserved(b7);
    }

    // ── 2. 随机布局生成的观测盘（保证一致性）──
    std::printf("== 2. 随机板 ==\n");
    std::mt19937_64 rng(12345);
    for (int trial = 0; trial < 60; ++trial) {
        const int rows = 4 + static_cast<int>(rng() % 2);   // 4..5
        const int cols = 4 + static_cast<int>(rng() % 2);   // 4..5
        const int mines = 3 + static_cast<int>(rng() % 4);  // 3..6
        if (mines > rows * cols / 2) continue;
        ObservedBoard board(rows, cols, mines);
        Grid<char> mine(rows, cols, 0);
        std::vector<CellId> all;
        all.reserve(static_cast<std::size_t>(rows * cols));
        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                all.push_back(board.id(x, y));
        std::shuffle(all.begin(), all.end(), rng);
        for (int i = 0; i < mines; ++i) {
            const int x = all[static_cast<std::size_t>(i)] / (cols + 1);
            const int y = all[static_cast<std::size_t>(i)] % (cols + 1);
            mine[x][y] = 1;
        }
        std::vector<CellId> safe;
        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (!mine[x][y]) safe.push_back(board.id(x, y));
        std::shuffle(safe.begin(), safe.end(), rng);
        const int revealCount = 1 + static_cast<int>(rng() % 4);  // 1..4
        for (int i = 0; i < revealCount && i < static_cast<int>(safe.size()); ++i) {
            const int x = safe[static_cast<std::size_t>(i)] / (cols + 1);
            const int y = safe[static_cast<std::size_t>(i)] % (cols + 1);
            int cnt = 0;
            forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) { cnt += mine[nx][ny]; });
            board.board[x][y] = static_cast<Cell>(cnt);
        }
        verifyObserved(board);
    }

    // ── 3. 真实对局中盘（all_distribute 全枚举对照）──
    std::printf("== 3. 真实对局 ==\n");
    {
        std::mt19937_64 rng2(777);
        for (int trial = 0; trial < 30; ++trial) {
            GameController gc(9, 9, 10, static_cast<unsigned>(rng2()));
            const int pick = static_cast<int>(rng2() % 81u);
            gc.reveal(pick / 9 + 1, pick % 9 + 1);
            for (int step = 0; step < 8; ++step) {
                const auto& gi = gc.info();
                if (gi.status != GameController::Status::Playing) break;
                const auto& an = gc.analysis();
                const auto& board = an.state();
                const auto& basic = an.basicMarks();
                const auto& structure = an.structure();
                // 廉价上界：所有可放雷格（T 格 + 活组件格）任选 M 个
                const int M = board.totalMines - basic.mineSum;
                int placeable = basic.unknownSum;
                for (const auto& inst : structure.components)
                    if (inst.alive) placeable += static_cast<int>(inst.boxes.cells.size());
                const ld coarse = comb(placeable, M);
                if (coarse > 300000.0L || placeable < M) break;
                // 统计方案数
                long long count = 0;
                Distribution::Solver::all_distribute(
                    board, basic, structure, [&](const std::vector<CellId>&) { ++count; });
                if (count == 0 || count > 300000) break;
                // 聚合每格
                std::vector<Counts> counts(
                    static_cast<std::size_t>((board.rows + 1) * (board.cols + 1)));
                Distribution::Solver::all_distribute(
                    board, basic, structure, [&](const std::vector<CellId>& placed) {
                        Grid<char> mine(board.rows, board.cols, 0);
                        for (int x = 1; x <= board.rows; ++x)
                            for (int y = 1; y <= board.cols; ++y)
                                if (basic.marks[x][y] == Basic::Mark::Mine) mine[x][y] = 1;
                        for (CellId c : placed) {
                            const auto [x, y] = board.pos(c);
                            mine[x][y] = 1;
                        }
                        for (int x = 1; x <= board.rows; ++x)
                            for (int y = 1; y <= board.cols; ++y) {
                                if (board.board[x][y] != Cell::Hidden) continue;
                                Counts& ct = counts[static_cast<std::size_t>(board.id(x, y))];
                                ++ct.total;
                                if (mine[x][y]) { ++ct.explosion; continue; }
                                int cnt = 0;
                                forEachAdjacent(x, y, board.rows, board.cols,
                                                [&](int nx, int ny) { cnt += mine[nx][ny]; });
                                ++ct.digit[static_cast<std::size_t>(cnt)];
                            }
                    });
                Distribution::DistPool pool;
                const Probability::Result prob = Exact::analyze(board, basic, structure, pool);
                for (int x = 1; x <= board.rows; ++x)
                    for (int y = 1; y <= board.cols; ++y) {
                        if (board.board[x][y] != Cell::Hidden) continue;
                        cover(board, basic, structure, x, y);
                        const CellId c = board.id(x, y);
                        const auto r = Exact::observe(board, basic, structure, prob, pool, c);
                        const Counts& ct = counts[static_cast<std::size_t>(c)];
                        char buf[160];
                        std::snprintf(buf, sizeof buf, "g t%02d s%02d (%d,%d) explosion", trial,
                                      step, x, y);
                        check(std::abs(r.explosion - static_cast<ld>(ct.explosion) / ct.total) <
                                  1e-9L, buf);
                        for (int k = 0; k <= 8; ++k) {
                            std::snprintf(buf, sizeof buf, "g t%02d s%02d (%d,%d) digit[%d]",
                                          trial, step, x, y, k);
                            check(std::abs(r.digit[static_cast<std::size_t>(k)] -
                                           static_cast<ld>(ct.digit[static_cast<std::size_t>(k)]) /
                                               ct.total) < 1e-9L, buf);
                        }
                        ++gCells;
                    }
                std::vector<std::pair<int, int>> cand;
                for (int x = 1; x <= 9; ++x)
                    for (int y = 1; y <= 9; ++y)
                        if (!gi.revealed[x][y] && !gi.layout[x][y])
                            cand.emplace_back(x, y);
                if (cand.empty()) break;
                const auto [x, y] = cand[static_cast<std::size_t>(rng2() % cand.size())];
                gc.reveal(x, y);
            }
        }
    }

    std::printf("核对 %lld 格；共 %lld 项检查，失败 %d 项\n", gCells, gCheck, gFail);
    std::printf("覆盖: T格=%lld box格=%lld size1box=%lld Safe格=%lld Mine标记格=%lld "
                "有Mine邻居=%lld 有T邻居=%lld\n",
                covT, covBox, covSize1, covSafe, covMineCell, covFixed, covUT);
    return gFail == 0 ? 0 : 1;
}