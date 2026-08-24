#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace mss {

// 1-based 坐标的二维网格：有效坐标范围 [1, rows] × [1, cols]。
// 内部多保留一行/列作为边框，这样遍历邻居时不需要做边界分支。
template <typename T>
class Grid {
public:
    Grid() = default;

    // 分配 rows × cols 的有效区（外加一圈边框），全部填充 value
    Grid(int rows, int cols, const T& value) { resize(rows, cols, value); }

    void resize(int rows, int cols, const T& value) {
        rows_ = rows;
        cols_ = cols;
        data_.assign(static_cast<std::size_t>(rows + 1) * (cols + 1), value);
    }

    void fill(const T& value) { std::fill(data_.begin(), data_.end(), value); }

    int rows() const { return rows_; }
    int cols() const { return cols_; }

    bool inBounds(int x, int y) const {
        return x >= 1 && x <= rows_ && y >= 1 && y <= cols_;
    }

    T& at(int x, int y) {
        return data_[index(x, y)];
    }

    const T& at(int x, int y) const {
        return data_[index(x, y)];
    }

    // 支持 grid[i][j] 的写法，语义等价于 at(i, j)
    auto operator[](int i) {
        struct Row {
            Grid& grid;
            int row;
            T& operator[](int j) { return grid.at(row, j); }
        };
        return Row{*this, i};
    }

    auto operator[](int i) const {
        struct Row {
            const Grid& grid;
            int row;
            const T& operator[](int j) const { return grid.at(row, j); }
        };
        return Row{*this, i};
    }

private:
    std::size_t index(int x, int y) const {
        // 内部步长为 cols+1（含边框列）
        return static_cast<std::size_t>(x) * (cols_ + 1) + y;
    }

    int rows_ = 0;
    int cols_ = 0;
    std::vector<T> data_;
};

}  // namespace mss
