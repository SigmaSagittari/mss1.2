#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/assert.h"
#include "ui/game_control.h"
#include "ui/http_server.h"
#include "ui/interactive.h"

#include <windows.h>
#include <shellapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif

namespace mss {

// UI 应用：本地 HTTP 服务 + 浏览器前端。
// 对外只暴露 run()；内部路由/JSON/静态文件全部私有。
class UiApp {
public:
    explicit UiApp(int port = 18080) : port_(port) {}

    int run() {
        server_.setHandler([this](const HttpRequest& req) { return handle(req); });
        if (!server_.start(port_)) {
            std::cerr << "无法在 127.0.0.1:" << port_ << " 启动服务（端口被占用？）\n";
            writeRunLog("failed: port " + std::to_string(port_) + " busy");
            return 1;
        }
        game_ = std::make_unique<GameController>(9, 9, 10, std::random_device{}());
        const std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/";
        std::cout << "MSS 扫雷 UI 已启动: " << url << "\n";
        std::cout << "按 Ctrl+C 退出。\n";
        writeRunLog("started: " + url);
        // 设置环境变量 MSS_NO_BROWSER=1 可跳过自动打开浏览器（无头测试用）
        if (!envFlagSet("MSS_NO_BROWSER")) openBrowser();
        server_.run();
        server_.stop();
        return 0;
    }

private:
    // ---------- 路由 ----------
    HttpResponse handle(const HttpRequest& req) {
        const std::string& p = req.path;
        if (p == "/" || p == "/index.html") return serveFile("index.html");
        if (p == "/style.css") return serveFile("style.css");
        if (p == "/main.js") return serveFile("main.js");
        if (p == "/favicon.ico") return {204, "text/plain", ""};

        if (p == "/api/state") return jsonState();
        if (p == "/api/new" && req.method == "POST") return jsonNew(req);
        if (p == "/api/reveal" && req.method == "POST") return jsonReveal(req);
        if (p == "/api/flag" && req.method == "POST") return jsonFlag(req);
        if (p == "/api/probability") return jsonProbability();
        if (p == "/api/detail") return jsonDetail(req);
        if (p == "/api/config") {
            if (req.method == "POST") return jsonConfig(req);
            return jsonConfigGet();
        }
        return {404, "text/plain; charset=utf-8", "not found"};
    }

    // ---------- 静态文件 ----------
    static std::string exeDirectory() {
        char buf[MAX_PATH];
        const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string path(buf, n > 0 ? n : 0);
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    }

    HttpResponse serveFile(const std::string& name) const {
        // 只服务源码树 src/ui/web（src 是唯一基准，不做构建期复制，杜绝配置间快照不一致）。
        // 沿 exe 目录向上找 src/ui/web：无论工作目录/输出布局怎么变都能命中。
        std::string dir = exeDirectory();
        for (int up = 0; up < 10 && !dir.empty(); ++up) {
            const std::string root = dir + "src/ui/web/";
            std::ifstream in(root + name, std::ios::binary);
            if (in) {
                std::string body((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                return {200, mimeOf(name), std::move(body)};
            }
            const size_t slash = dir.find_last_of("\\/", dir.size() - 2);
            dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash + 1);
        }
        return {404, "text/plain; charset=utf-8", "src/ui/web/" + name + " not found"};
    }

    static std::string mimeOf(const std::string& name) {
        if (name.ends_with(".html")) return "text/html; charset=utf-8";
        if (name.ends_with(".css")) return "text/css; charset=utf-8";
        if (name.ends_with(".js")) return "application/javascript; charset=utf-8";
        if (name.ends_with(".svg")) return "image/svg+xml";
        return "application/octet-stream";
    }

    // ---------- JSON 辅助 ----------
    static std::string countDigits(long double v) {
        assert_(std::isfinite(v) && v >= 0.0L, "UiApp::countDigits: 非有限候选数");
        std::ostringstream os;
        os << std::fixed << std::setprecision(0) << v;
        return os.str();
    }

    // 候选方案数人类可读形式：小值完整显示，大值科学计数法。
    // long double 是浮点不是高精度整数，超过一定量级直接科学计数，
    // 避免显示一长串无意义的精确数字。
    static std::string formatCount(long double v) {
        assert_(std::isfinite(v) && v >= 0.0L, "UiApp::formatCount: 非有限候选数");
        if (v < 1e6L) return countDigits(v);
        std::ostringstream os;
        os << std::setprecision(4) << v;
        return os.str();
    }

    static std::string jsonString(const std::string& s) {
        std::ostringstream os;
        os << '"';
        for (char ch : s) {
            switch (ch) {
                case '"': os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default: os << ch;
            }
        }
        os << '"';
        return os.str();
    }

    static HttpResponse json(const std::string& body) {
        return {200, "application/json; charset=utf-8", body};
    }

    static int bodyInt(const std::string& body, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        size_t pos = body.find(needle);
        if (pos == std::string::npos) return -1;
        size_t colon = body.find(':', pos);
        if (colon == std::string::npos) return -1;
        return std::atoi(body.c_str() + colon + 1);
    }

    static bool bodySeed(const std::string& body, const std::string& key, unsigned& out) {
        std::string needle = "\"" + key + "\"";
        size_t pos = body.find(needle);
        if (pos == std::string::npos) return false;
        size_t colon = body.find(':', pos);
        if (colon == std::string::npos) return false;
        out = static_cast<unsigned>(std::strtoul(body.c_str() + colon + 1, nullptr, 10));
        return true;
    }

    static int queryInt(const HttpRequest& req, const std::string& key, int def) {
        auto it = req.query.find(key);
        if (it == req.query.end()) return def;
        return std::atoi(it->second.c_str());
    }

    // ---------- 盘面 JSON ----------
    HttpResponse jsonState() const {
        const auto& gi = game_->info();
        std::ostringstream os;
        os << "{\"status\":\"" << statusText() << "\""
           << ",\"rows\":" << gi.rows << ",\"cols\":" << gi.cols
           << ",\"mines\":" << gi.mines
           << ",\"flagsRemaining\":" << gi.flagsRemaining
           << ",\"moves\":" << gi.moves << ",\"seed\":" << gi.seed << ",\"board\":[";
        for (int i = 1; i <= gi.rows; ++i) {
            if (i > 1) os << ',';
            os << '[';
            for (int j = 1; j <= gi.cols; ++j) {
                if (j > 1) os << ',';
                os << cellValue(i, j);
            }
            os << ']';
        }
        os << "]}";
        return json(os.str());
    }

    int cellValue(int x, int y) const {
        const auto& gi = game_->info();
        if (gi.status == GameController::Status::Lost && gi.layout[x][y]) {
            auto [ex, ey] = game_->exploded();
            return (x == ex && y == ey) ? -4 : -3;
        }
        if (gi.revealed[x][y]) return game_->adjacentMines(x, y);
        if (gi.flags[x][y]) return -2;
        return -1;
    }

    const char* statusText() const {
        switch (game_->info().status) {
            case GameController::Status::Playing: return "playing";
            case GameController::Status::Won: return "won";
            case GameController::Status::Lost: return "lost";
        }
        return "playing";
    }

    HttpResponse jsonNew(const HttpRequest& req) {
        int rows = std::clamp(bodyInt(req.body, "rows"), 2, 100);
        int cols = std::clamp(bodyInt(req.body, "cols"), 2, 100);
        int maxMines = rows * cols - 1;
        int mines = std::clamp(bodyInt(req.body, "mines"), 1, maxMines);
        unsigned seed;
        if (!bodySeed(req.body, "seed", seed)) seed = std::random_device{}();
        game_ = std::make_unique<GameController>(rows, cols, mines, seed);
        // 前端可随新局携带引擎模式（mode: "full"|"incremental"）。
        const std::string needle = "\"mode\"";
        const size_t pos = req.body.find(needle);
        if (pos != std::string::npos)
            game_->analysis().setEngine(
                req.body.find("incremental", pos) != std::string::npos
                    ? GameController::Analysis::Engine::Approx
                    : GameController::Analysis::Engine::Exact);
        return jsonState();
    }

    // 读当前引擎模式（前端初始化同步用）。
    HttpResponse jsonConfigGet() const {
        using Engine = GameController::Analysis::Engine;
        const std::string mode =
            game_->analysis().engine() == Engine::Approx ? "incremental" : "full";
        return json("{\"mode\":\"" + mode + "\"}");
    }

    // 写引擎模式（切换增量近似更新 / 全局重建）。
    HttpResponse jsonConfig(const HttpRequest& req) {
        using Engine = GameController::Analysis::Engine;
        game_->analysis().setEngine(
            req.body.find("incremental") != std::string::npos ? Engine::Approx : Engine::Exact);
        return jsonConfigGet();
    }

    HttpResponse jsonReveal(const HttpRequest& req) {
        int x = bodyInt(req.body, "x");
        int y = bodyInt(req.body, "y");
        if (x >= 1 && x <= game_->info().rows && y >= 1 && y <= game_->info().cols) {
            const auto t0 = std::chrono::steady_clock::now();
            game_->reveal(x, y);
            computedMs_ = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
        }
        return jsonState();
    }

    HttpResponse jsonFlag(const HttpRequest& req) {
        int x = bodyInt(req.body, "x");
        int y = bodyInt(req.body, "y");
        if (x >= 1 && x <= game_->info().rows && y >= 1 && y <= game_->info().cols) {
            game_->toggleFlag(x, y);
        }
        return jsonState();
    }

    // ---------- 概率 ----------
    HttpResponse jsonProbability() {
        const auto& gi = game_->info();
        const auto& analysis = game_->analysis();
        Grid<long double> grid = analysis.materializeProbability();

        std::ostringstream os;
        os << std::setprecision(12);
        os << "{\"candidates\":\"" << formatCount(analysis.candidates())
           << "\",\"tProb\":" << static_cast<double>(analysis.tCellProbability())
           << ",\"computedMs\":" << computedMs_ << ",\"prob\":[";
        for (int i = 1; i <= gi.rows; ++i) {
            if (i > 1) os << ',';
            os << '[';
            for (int j = 1; j <= gi.cols; ++j) {
                if (j > 1) os << ',';
                os << static_cast<double>(grid[i][j]);
            }
            os << ']';
        }
        os << "]}";
        return json(os.str());
    }

    HttpResponse jsonDetail(const HttpRequest& req) {
        int x = queryInt(req, "x", 0);
        int y = queryInt(req, "y", 0);
        const auto& gi = game_->info();
        if (x < 1 || x > gi.rows || y < 1 || y > gi.cols)
            return json("{\"text\":\"（悬停在格子上查看）\"}");
        return json("{\"text\":" + jsonString(getDetailInfo(x, y)) + "}");
    }

    std::string getDetailInfo(int x, int y) {
        const auto& gi = game_->info();
        std::vector<std::string> lines;
        lines.push_back("格子 (" + std::to_string(x) + ", " + std::to_string(y) + ")");

        std::string stateLine;
        if (gi.flags[x][y])
            stateLine = "已标旗";
        else if (gi.revealed[x][y])
            stateLine = "已翻开，数字 " + std::to_string(game_->adjacentMines(x, y));
        else
            stateLine = "未翻开";
        lines.push_back("状态: " + stateLine);

        long double p = game_->analysis().mineProbability(x, y);
        std::ostringstream ps;
        ps << std::setprecision(10) << "雷概率: " << static_cast<double>(p) << "  ("
           << std::fixed << std::setprecision(2) << static_cast<double>(p * 100) << "%)";
        lines.push_back(ps.str());

        // 近似引擎下，额外用精确引擎全量重算一遍做对比（UI 层，每次访问现算）。
        using Engine = GameController::Analysis::Engine;
        if (game_->analysis().engine() == Engine::Approx) {
            const auto& an = game_->analysis();
            Distribution::DistPool dists;
            Probability::Result exact =
                Exact::analyze(an.state(), an.basicMarks(), an.structure(), dists);
            const long double ep =
                exact.mineProbability(an.state().id(x, y), an.state(), an.basicMarks(),
                                      an.structure());
            const long double diff = (p - ep) * 100.0L;
            std::ostringstream eps;
            eps << std::setprecision(10) << "精确对比: " << static_cast<double>(ep)
                << "  (" << std::fixed << std::setprecision(2)
                << static_cast<double>(ep * 100) << "%, "
                << std::showpos << static_cast<double>(diff) << "%"
                << std::noshowpos << ")";
            lines.push_back(eps.str());
        }

        std::ostringstream cs;
        cs << std::setprecision(10)
           << "候选方案数: " << formatCount(game_->analysis().candidates());
        lines.push_back(cs.str());
        std::ostringstream tp;
        tp << std::setprecision(6)
           << "非前沿雷概率: " << static_cast<double>(game_->analysis().tCellProbability());
        lines.push_back(tp.str());
        lines.push_back("计算耗时: " + std::to_string(computedMs_) + " ms");

        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out += "\n";
            out += lines[i];
        }
        return out;
    }

    void openBrowser() const {
        std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/";
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    static bool envFlagSet(const char* name) {
        char buf[8] = {};
        return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0;
    }

    // 启动状态写盘（mss_run.log）：端口占用/崩溃等原因不明时方便排查。
    static void writeRunLog(const std::string& msg) {
        std::ofstream log("mss_run.log", std::ios::app);
        if (log) log << msg << '\n';
    }

    HttpServer server_;
    std::unique_ptr<GameController> game_;
    int port_ = 18080;
    int computedMs_ = 0;
};

}  // namespace mss