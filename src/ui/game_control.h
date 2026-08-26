#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability.h"
#include "analysis/probability/exact.h"
#include "analysis/rational.h"
#include "analysis/structure.h"
#include "core/types.h"
#include "core/utility/rng.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// game_control.h — 游戏控制器。
//
// 一个游戏 = 一个对象：持有真实雷位布局 + 分析视图，每次操作后驱动
// 分析管线刷新。控制"打开之后游戏显示什么"：reveal、泛洪、标旗、胜负。
//
// 对外只暴露 info()/reveal()/toggleFlag() 等游戏操作；
// 分析结果通过 analysis() 访问（getter 惰性刷新）。
// ─────────────────────────────────────────────────────────────

struct GameController {
    enum class Status { Playing, Won, Lost };

    // 游戏信息视图（引用挂载，随调用方生命周期有效）。
    struct game_info {
        const Grid<char>& layout;     // truth：雷位
        const Grid<char>& revealed;
        const Grid<char>& flags;
        int rows = 0, cols = 0, mines = 0, moves = 0, flagsRemaining = 0;
        Status status = Status::Playing;
        unsigned seed = 0;
    };

    // ── 分析管线 ──
    // 持有 ObservedBoard（数字/Hidden 分析视图）+ 各层结果 + 池 + 概率引擎。
    // 概率总是全量（Exact::analyze，热池 ~1.5μs）；结构层可选增量/全量。
    struct Analysis {
        // 结构处理方式：Update = 受影响块增量重建（默认）；Rebuild = 全量重分析。
        enum class StructureMode { Update, Rebuild };

        Analysis(int rows, int cols, int mines) : state_(rows, cols, mines) {}

        // 首次全量建 basic + structure + probability（初始全隐藏盘面）。
        void initFromState() {
            basic_ = Basic::Analyzer::analyze(state_);
            structure_ = Structure::Analyzer::analyze(state_, basic_, shapes_);
            rebuild();
        }

        // 一次揭示后刷新全链：basic → structure → probability。
        // updates 是本次翻开产生的值事件。
        void update(const std::vector<Basic::Update>& updates) {
            Basic::Updater::update(state_, basic_, updates);
            if (structureMode_ == StructureMode::Rebuild)
                structure_ = Structure::Analyzer::analyze(state_, basic_, shapes_);
            else
                Structure::Updater::update(state_, basic_, structure_, shapes_, updates);
            prob_ = Exact::analyze(state_, basic_, structure_, dists_);
        }

        void setStructureMode(StructureMode m) { structureMode_ = m; }
        StructureMode structureMode() const { return structureMode_; }

        const Basic::Result& basicMarks() const { return basic_; }
        const Structure::Result& structure() const { return structure_; }
        const Probability::Result& probability() const { return prob_; }

        // 池访问（RhoRational 惰性记忆化需要可变引用；只读查询也走这里）。
        Distribution::DistPool& dists() { return dists_; }
        RhoRational::Pool& rationals() { return rationals_; }

        ObservedBoard& state() { return state_; }
        const ObservedBoard& state() const { return state_; }

    private:
        // 全量初始化概率状态（开局 / 编辑后）。
        void rebuild() { prob_ = Exact::analyze(state_, basic_, structure_, dists_); }

        ObservedBoard state_;                  // 分析视图（数字/Hidden），经 update 维护
        Basic::Result basic_;
        Structure::Result structure_;
        Structure::ShapePool shapes_;          // 结构池（本游戏生命周期）
        Distribution::DistPool dists_;         // 分布池（本游戏生命周期）
        RhoRational::Pool rationals_;          // 系数池（本游戏生命周期）
        Probability::Result prob_;             // Exact 产物（数值视图）
        StructureMode structureMode_ = StructureMode::Update;
    };

    // ── 构造：开局即分析 ──
    GameController(int rows, int cols, int mines, unsigned seed)
        : analysis_(rows, cols, mines),
          layout_(rows, cols, 0), revealed_(rows, cols, 0), flags_(rows, cols, 0),
          rng_(seed) {
        rows_ = rows; cols_ = cols; mines_ = mines; seed_ = seed;
        flagsRemaining_ = mines;
        generate();
        // 初始盘面（全隐藏）：建一次 basic + structure（空结构）。
        analysis_.initFromState();
    }
    ~GameController() = default;
    GameController(const GameController&) = delete;
    GameController& operator=(const GameController&) = delete;

    game_info info() const {
        return {layout_, revealed_, flags_, rows_, cols_, mines_,
                moves_, flagsRemaining_, status_, seed_};
    }

    // false = 踩雷结束
    bool reveal(int x, int y) {
        if (status_ != Status::Playing) return false;
        if (x < 1 || x > rows_ || y < 1 || y > cols_) return false;
        if (revealed_[x][y]) { chord(x, y); return status_ != Status::Lost; }
        if (flags_[x][y]) return false;

        ++moves_; ++revision_;
        if (firstMove_) {
            firstMove_ = false;
            if (layout_[x][y]) relocateMine(x, y);
        }

        std::vector<Basic::Update> updates;
        openCell(x, y, updates);

        if (status_ == Status::Lost) return false;
        checkWin();
        if (!updates.empty()) analysis_.update(updates);
        return true;
    }

    void toggleFlag(int x, int y) {
        if (status_ != Status::Playing) return;
        if (revealed_[x][y]) return;
        flags_[x][y] ^= 1;
        flagsRemaining_ += flags_[x][y] ? -1 : 1;
        ++revision_;
    }

    int revision() const { return revision_; }
    std::pair<int, int> exploded() const { return {explodedX_, explodedY_}; }

    int adjacentMines(int x, int y) const {
        int cnt = 0;
        forEachAdjacent(x, y, rows_, cols_, [&](int nx, int ny) { cnt += layout_[nx][ny]; });
        return cnt;
    }

    // 分析结果访问（getter，ui 层只读）。
    const Analysis& analysis() const { return analysis_; }
    Analysis& analysis() { return analysis_; }

private:
    // ── 游戏 ──
    Analysis analysis_;            // 分析管线（构造即初始化，游戏生命周期内持有）
    Grid<char> layout_;
    Grid<char> revealed_;
    Grid<char> flags_;
    int rows_ = 0, cols_ = 0, mines_ = 0, moves_ = 0, flagsRemaining_ = 0;
    Status status_ = Status::Playing;
    unsigned seed_ = 0;
    Rng rng_;
    bool firstMove_ = true;
    int revealedCount_ = 0;
    int explodedX_ = 0, explodedY_ = 0;
    int revision_ = 0;

    // ── 分析 ──
    // Analysis 持有全部分析状态；GameController 只负责游戏规则并驱动它。

    void generate() {
        const int total = rows_ * cols_;
        std::vector<int> cells(total);
        for (int i = 0; i < total; ++i) cells[i] = i;
        for (int i = total - 1; i > 0; --i) {
            const std::uint64_t j = rng_.next() % static_cast<std::uint64_t>(i + 1);
            std::swap(cells[i], cells[static_cast<int>(j)]);
        }
        for (int k = 0; k < mines_ && k < total; ++k)
            layout_.at(cells[k] / cols_ + 1, cells[k] % cols_ + 1) = 1;
    }

    void chord(int x, int y) {
        if (status_ != Status::Playing || !revealed_[x][y]) return;
        const int num = adjacentMines(x, y);
        if (num == 0) return;
        int flagCount = 0;
        forEachAdjacent(x, y, rows_, cols_, [&](int nx, int ny) { flagCount += flags_[nx][ny]; });
        if (flagCount != num) return;

        std::vector<Basic::Update> updates;
        bool any = false;
        forEachAdjacent(x, y, rows_, cols_, [&](int nx, int ny) {
            if (!flags_[nx][ny] && !revealed_[nx][ny]) {
                any = true;
                openCell(nx, ny, updates);
            }
        });
        if (any) { ++moves_; ++revision_; }
        checkWin();
        if (!updates.empty()) analysis_.update(std::move(updates));
    }

    void openCell(int x, int y, std::vector<Basic::Update>& updates) {
        if (revealed_[x][y] || flags_[x][y]) return;
        if (layout_[x][y]) {
            status_ = Status::Lost;
            explodedX_ = x; explodedY_ = y;
            return;
        }
        revealFlood(x, y, updates);
    }

    void revealFlood(int x, int y, std::vector<Basic::Update>& updates) {
        if (revealed_[x][y] || layout_[x][y]) return;
        revealed_[x][y] = 1;
        ++revealedCount_;
        const Cell v = static_cast<Cell>(adjacentMines(x, y));
        analysis_.state().board[x][y] = v;
        updates.push_back(Basic::Update{analysis_.state().id(x, y), v});
        if (v == Cell::Num0)
            forEachAdjacent(x, y, rows_, cols_, [&](int nx, int ny) { revealFlood(nx, ny, updates); });
    }

    void relocateMine(int x, int y) {
        std::vector<std::pair<int, int>> empty;
        for (int i = 1; i <= rows_; ++i)
            for (int j = 1; j <= cols_; ++j)
                if (!layout_[i][j] && !(i == x && j == y)) empty.emplace_back(i, j);
        if (empty.empty()) return;
        const std::uint64_t pick = rng_.next() % empty.size();
        const auto [nx, ny] = empty[static_cast<int>(pick)];
        layout_[x][y] = 0;
        layout_[nx][ny] = 1;
    }

    void checkWin() {
        if (status_ != Status::Playing || revealedCount_ != rows_ * cols_ - mines_) return;
        status_ = Status::Won;
        for (int i = 1; i <= rows_; ++i)
            for (int j = 1; j <= cols_; ++j)
                if (layout_[i][j]) flags_[i][j] = 1;
        flagsRemaining_ = 0;
    }
};

}  // namespace mss