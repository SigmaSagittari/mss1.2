#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
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
    ~UiApp() { stopSearch(); }

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
        if (p == "/analyzer.js") return serveFile("analyzer.js");
        if (p == "/favicon.ico") return {204, "text/plain", ""};

        if (p == "/api/state") return jsonState();
        if (p == "/api/new" && req.method == "POST") return jsonNew(req);
        if (p == "/api/reveal" && req.method == "POST") return jsonReveal(req);
        if (p == "/api/flag" && req.method == "POST") return jsonFlag(req);
        if (p == "/api/probability") return jsonProbability();
        if (p == "/api/detail") return jsonDetail(req);
        if (p == "/api/analyzer" && req.method == "POST") return jsonAnalyzer(req);
        if (p == "/api/edit" && req.method == "POST") return jsonEdit(req);
        if (p == "/api/analyze" && req.method == "POST") return jsonAnalyzeStart(req);
        if (p == "/api/analyze/start" && req.method == "POST") return jsonAnalyzeStart(req);
        if (p == "/api/analyze/stop" && req.method == "POST") return jsonAnalyzeStop();
        if (p == "/api/analyze/status") return jsonAnalyzeStatus();
        if (p == "/api/analyze/tree") return jsonAnalyzeTree(req);
        if (p == "/api/analyze/moves") return jsonAnalyzeMoves();
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
        // 新局丢弃分析会话（编辑快照/分析态随旧局失效）。
        editSaved_.reset();
        analyzerActive_ = false;
        stopSearch();
        return jsonState();
    }

    // 读当前结构处理方式（前端初始化同步用）。概率恒全量。
    HttpResponse jsonConfigGet() const {
        using SM = GameController::Analysis::StructureMode;
        const std::string mode =
            game_->analysis().structureMode() == SM::Rebuild ? "rebuild" : "update";
        return json("{\"structMode\":\"" + mode + "\"}");
    }

    // 写结构处理方式（rebuild = 全量重建 / update = 增量）。概率恒全量。
    HttpResponse jsonConfig(const HttpRequest& req) {
        using SM = GameController::Analysis::StructureMode;
        game_->analysis().setStructureMode(
            req.body.find("rebuild") != std::string::npos ? SM::Rebuild : SM::Update);
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
        auto& an = game_->analysis();
        const Grid<long double> grid = Interactive::materializeProbability(an);

        std::ostringstream os;
        os << std::setprecision(12);
        os << "{\"candidates\":\"" << formatCount(Interactive::candidates(an))
           << "\",\"tProb\":" << static_cast<double>(Interactive::tCellProbability(an))
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

        long double p = Interactive::mineProbability(game_->analysis(), x, y);
        std::ostringstream ps;
        ps << std::setprecision(5) << "雷概率: "
           << static_cast<double>(p) << "  (" << static_cast<double>(p * 100) << "%)";
        lines.push_back(ps.str());

        // 点开结果分布（observe）：爆炸 + 各数字概率（仅未翻开格有意义）。
        // 全部数字保留五位有效数字（有效数字，不是小数点后）。
        if (!gi.revealed[x][y]) {
            const Probability::ObserveResult obr = Interactive::observe(game_->analysis(), x, y);
            auto fmtPct = [](long double v) {
                std::ostringstream os;
                os << std::setprecision(5) << static_cast<double>(v * 100.0L) << '%';
                return os.str();
            };
            lines.push_back("点开: 爆炸 " + fmtPct(obr.explosion));
            for (int k = 0; k <= 8; ++k) {
                lines.push_back("数字 " + std::to_string(k) + ": " + fmtPct(obr.digit[k]));
            }
        }

        std::ostringstream cs;
        cs << "候选方案数: " << formatCount(Interactive::candidates(game_->analysis()));
        lines.push_back(cs.str());
        std::ostringstream tp;
        tp << std::setprecision(5)
           << "非前沿雷概率: " << static_cast<double>(Interactive::tCellProbability(game_->analysis()));
        lines.push_back(tp.str());
        lines.push_back("计算耗时: " + std::to_string(computedMs_) + " ms");

        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) out += "\n";
            out += lines[i];
        }
        return out;
    }

    // ---------- 全局分析（分析模式） ----------
    // 分析模式：进入时快照原始盘面（退出还原），编辑直接改分析视图的 ObservedBoard；
    // 每次点「开始分析」从（可能被编辑过的）盘面全量重构 basic/structure/probability，
    // 合法性 → 候选数（精确）→ 低于暴力阈值则残局求解。
    // 与当前引擎无关（候选数/暴力均基于 Exact 精确视图）。

    // 进入/退出分析模式：进入保存盘面快照，退出还原（编辑不污染真实游戏盘面）。
    HttpResponse jsonAnalyzer(const HttpRequest& req) {
        const bool active = req.body.find("true") != std::string::npos;
        auto& state = game_->analysis().state();
        if (active) {
            if (!analyzerActive_) editSaved_ = std::make_unique<ObservedBoard>(state);
            analyzerActive_ = true;
        } else {
            if (analyzerActive_ && editSaved_) {
                state = std::move(*editSaved_);
                game_->analysis().initFromState();
            }
            editSaved_.reset();
            analyzerActive_ = false;
            stopSearch();
        }
        return json("{\"ok\":true}");
    }

    // 编辑：改分析视图的盘面格值（v=0..8 数字，9=盖上 Hidden）。仅分析模式生效。
    HttpResponse jsonEdit(const HttpRequest& req) {
        if (!analyzerActive_) return json("{\"ok\":true}");
        const int x = bodyInt(req.body, "x");
        const int y = bodyInt(req.body, "y");
        const int v = bodyInt(req.body, "v");
        auto& state = game_->analysis().state();
        if (x >= 1 && x <= state.rows && y >= 1 && y <= state.cols && v >= 0 && v <= 9) {
            state.board[x][y] = (v == 9) ? Cell::Hidden : static_cast<Cell>(v);
            // 编辑直接改分析视图，必须重建 basic/structure/probability，
            // 否则 /api/probability 返回的是编辑前的旧概率。
            game_->analysis().initFromState();
        }
        return json("{\"ok\":true}");
    }

    // 概率网格 JSON 尾部（"tProb","prob" 两字段，接在已有 JSON 后）。
    static std::string gridJson(const Grid<long double>& grid, long double tProb) {
        std::ostringstream os;
        os << std::setprecision(12) << ",\"tProb\":" << static_cast<double>(tProb)
           << ",\"prob\":[";
        for (int i = 1; i <= grid.rows(); ++i) {
            if (i > 1) os << ',';
            os << '[';
            for (int j = 1; j <= grid.cols(); ++j) {
                if (j > 1) os << ',';
                os << static_cast<double>(grid[i][j]);
            }
            os << ']';
        }
        os << ']';
        return os.str();
    }

    // ---------- 后台分析线程 ----------
    struct AnalysisSnapshot {
        bool valid = false;
        bool running = false;
        bool midgame = false;
        bool bruteforce = false;
        std::string reason;
        std::string candidates;
        int firstX = 0, firstY = 0;
        int depth = 0, nodes = 0, observes = 0;
        int total = 0, wins = 0, ms = 0;
        long long totalMs = 0;
        long long memLimit = 0;   // 树内存上限（字节），0 = 未设
        double value = 0;
        double winRate = 0;
        double nodeRate = 0;
        double tProb = 0;
    };

    AnalysisSnapshot snapshotFromSession(const MidgameSearch::Session& s, bool running) {
        AnalysisSnapshot snap;
        snap.valid = s.valid;
        snap.running = running;
        snap.reason = s.reason;
        snap.candidates = formatCount(s.prob.candidates);
        snap.tProb = static_cast<double>(s.prob.tCellProbability);
        snap.memLimit = s.config.maxMemBytes;
        if (!s.valid) return snap;
        const bool brute = s.prob.candidates <= static_cast<long double>(kMaxBruteforceCount);
        snap.bruteforce = brute;
        snap.midgame = !brute;
        if (!brute) {
            const MidgameSearch::Answer ans = MidgameSearch::getAnswer(s.tree);
            snap.firstX = ans.x;
            snap.firstY = ans.y;
            snap.depth = ans.depth;
            snap.nodes = ans.nodes;
            snap.observes = ans.observes;
            snap.value = static_cast<double>(ans.value);
        }
        return snap;
    }

    void dumpTreeLog(const MidgameSearch::Tree& t) {
        std::ofstream log("search_tree_1000.log", std::ios::trunc);
        if (log) MidgameSearch::dumpTree(t, log, 4, 300);
    }

    void searchLoop() {
        for (;;) {
            AnalysisSnapshot snap;
            bool noProgress = false;
            bool brute = false;
            MidgameSearch::Session* sessionPtr = nullptr;
            bool midgame = false;
            {
                std::unique_lock lk(searchMutex_);
                if (searchStop_ || !searchSession_) break;
                sessionPtr = searchSession_.get();
                MidgameSearch::Session& s = *sessionPtr;
                brute = s.prob.candidates <= static_cast<long double>(kMaxBruteforceCount);
                midgame = !brute;
            }
            if (midgame) {
                const auto t0 = std::chrono::steady_clock::now();
                int before = 0;
                {
                    std::lock_guard lk(searchMutex_);
                    before = sessionPtr->tree.statsNodes;
                }
                int lastAfter = before;
                constexpr int kChunkNodes = 4;
                while (lastAfter - before < kChunkNodes) {
                    {
                        std::unique_lock lk(searchMutex_);
                        if (searchStop_ || !searchSession_) break;
                        MidgameSearch::Session& s = *sessionPtr;
                        const int beforeOne = lastAfter;
                        MidgameSearch::grow(s, 1);
                        lastAfter = s.tree.statsNodes;
                        if (lastAfter == beforeOne) break;
                        if (!searchTreeLogged_ && s.tree.statsNodes >= 1000) {
                            searchTreeLogged_ = true;
                            dumpTreeLog(s.tree);
                        }
                    }
                }
                const auto t1 = std::chrono::steady_clock::now();
                MidgameSearch::Session& s = *sessionPtr;
                snap = snapshotFromSession(s, true);
                const int chunkMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                snap.ms = chunkMs;
                searchTotalMs_ += chunkMs;
                snap.totalMs = searchTotalMs_;
                const auto rateNow = std::chrono::steady_clock::now();
                rateSamples_.push_back({rateNow, s.tree.statsNodes});
                while (!rateSamples_.empty() &&
                       rateNow - rateSamples_.front().first > std::chrono::seconds(3))
                    rateSamples_.pop_front();
                if (rateSamples_.size() >= 2) {
                    const double dt =
                        std::chrono::duration<double>(rateNow - rateSamples_.front().first).count();
                    const double dn = static_cast<double>(
                        s.tree.statsNodes - rateSamples_.front().second);
                    if (dt > 0) snap.nodeRate = dn / dt;
                }
                if (lastAfter == before) noProgress = true;
            }

            if (brute) {
                MidgameSearch::Session& s = *sessionPtr;
                const auto t0 = std::chrono::steady_clock::now();
                Grid<long double> grid = Interactive::materializeProbability(s);
                const EndgameBruteforce::Result r = EndgameBruteforce::solveEndgame(
                    s.board, s.basic, s.structure, s.pool, grid);
                const auto t1 = std::chrono::steady_clock::now();
                snap = snapshotFromSession(s, false);
                snap.bruteforce = true;
                snap.midgame = false;
                if (!r.result.empty()) {
                    snap.firstX = r.result[0].x;
                    snap.firstY = r.result[0].y;
                    snap.wins = r.result[0].wins;
                }
                snap.total = r.totalPossibilities;
                snap.nodes = static_cast<int>(r.nodes);
                snap.winRate = r.totalPossibilities > 0
                                   ? static_cast<double>(r.result.empty() ? 0 : r.result[0].wins) /
                                         r.totalPossibilities * 100.0
                                   : 0.0;
                snap.ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                snap.totalMs = snap.ms;
                {
                    std::lock_guard lk(searchMutex_);
                    if (searchStop_) break;
                }
                noProgress = true;
            }

            {
                std::lock_guard lk(snapMutex_);
                searchSnap_ = snap;
            }
            if (noProgress) {
                std::lock_guard lk(searchMutex_);
                searchRunning_ = false;
                snap.running = false;
                std::lock_guard sl(snapMutex_);
                searchSnap_ = snap;
                break;
            }
        }
    }
    void startSearch() {
        stopSearch();
        std::lock_guard lk(searchMutex_);
        searchStop_ = false;
        searchRunning_ = false;
        searchTreeLogged_ = false;
        searchTotalMs_ = 0;
        rateSamples_.clear();
        searchSession_ = std::make_unique<MidgameSearch::Session>();
        if (memLimitMbOverride_ > 0) {
            searchSession_->config.maxMemBytes =
                static_cast<long long>(memLimitMbOverride_) * 1024LL * 1024LL;
        }
        const ObservedBoard& state = game_->analysis().state();
        if (!MidgameSearch::build(*searchSession_, state)) {
            AnalysisSnapshot snap;
            snap.valid = false;
            snap.reason = searchSession_->reason;
            snap.candidates = "0";
            {
                std::lock_guard sl(snapMutex_);
                searchSnap_ = snap;
            }
            searchSession_.reset();
            return;
        }
        searchRunning_ = true;
        AnalysisSnapshot snap = snapshotFromSession(*searchSession_, true);
        {
            std::lock_guard sl(snapMutex_);
            searchSnap_ = snap;
        }
        searchThread_ = std::thread([this] { searchLoop(); });
    }

    void stopSearch() {
        std::thread t;
        {
            std::lock_guard lk(searchMutex_);
            searchStop_ = true;
            t = std::move(searchThread_);
        }
        if (t.joinable()) t.join();
        std::lock_guard lk(searchMutex_);
        searchRunning_ = false;
        searchSession_.reset();
        searchStop_ = false;
        { std::lock_guard sl(snapMutex_); searchSnap_.running = false; }
    }

    static std::string buildTreeNodeJson(const MidgameSearch::Tree& t, int nodeId,
                                         std::size_t maxActions = 200) {
        const MidgameSearch::Node& n = t.nodes[static_cast<std::size_t>(nodeId)];
        std::ostringstream os;
        os << std::setprecision(6);
        os << "{\"id\":" << nodeId
           << ",\"parent\":" << n.parent
           << ",\"depth\":" << n.depth
           << ",\"cell\":" << n.cell
           << ",\"digit\":" << n.digit
           << ",\"value\":" << static_cast<double>(n.value)
           << ",\"dacc\":" << static_cast<double>(n.dacc)
           << ",\"tLocal\":" << static_cast<double>(n.tLocal)
           << ",\"t\":" << static_cast<double>(n.t)
           << ",\"C\":" << static_cast<double>(n.C)
           << ",\"expanded\":" << (n.expanded ? "true" : "false")
           << ",\"nodes\":" << t.statsNodes
           << ",\"actions\":[";
        std::vector<std::size_t> order(n.actions.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
            const double sl = lhs < n.score.size() ? static_cast<double>(n.score[lhs]) : 0.0;
            const double sr = rhs < n.score.size() ? static_cast<double>(n.score[rhs]) : 0.0;
            if (sl != sr) return sl > sr;
            const double pl = static_cast<double>(n.actions[lhs].p);
            const double pr = static_cast<double>(n.actions[rhs].p);
            if (pl != pr) return pl < pr;
            return lhs < rhs;
        });
        for (std::size_t oi = 0; oi < order.size() && oi < maxActions; ++oi) {
            const std::size_t i = order[oi];
            if (oi > 0) os << ',';
            const MidgameSearch::Action& a = n.actions[i];
            const int x = a.cell / (t.cols + 1);
            const int y = a.cell % (t.cols + 1);
            const double r = i < n.r.size() ? static_cast<double>(n.r[i]) : 0.0;
            const double score = i < n.score.size() ? static_cast<double>(n.score[i]) : 0.0;
            os << "{\"cell\":" << a.cell
               << ",\"x\":" << x
               << ",\"y\":" << y
               << ",\"p\":" << static_cast<double>(a.p)
               << ",\"mult\":" << static_cast<double>(a.mult)
               << ",\"r\":" << r
               << ",\"score\":" << score
               << ",\"branches\":[";
            bool firstBranch = true;
            const std::array<double, 9>* dg = MidgameSearch::digitOf(n, a);
            if (dg) {
                for (int k = 0; k <= 8; ++k) {
                    if ((*dg)[static_cast<std::size_t>(k)] <= 0.0L) continue;
                    if (!firstBranch) os << ',';
                    firstBranch = false;
                    const int c = MidgameSearch::branchOf(n, static_cast<int>(i), k);
                    const char* state = c == MidgameSearch::kUnexp    ? "unexpanded"
                                        : c == MidgameSearch::kDominated ? "dominated"
                                                                         : "node";
                    double val = 0.0, ct = 0.0, cc = 0.0;
                    if (c >= 0) {
                        const MidgameSearch::Node& cn = t.nodes[static_cast<std::size_t>(c)];
                        val = static_cast<double>(cn.value);
                        ct = static_cast<double>(cn.t);
                        cc = static_cast<double>(cn.C);
                    }
                    os << "{\"digit\":" << k
                       << ",\"prob\":" << static_cast<double>((*dg)[static_cast<std::size_t>(k)])
                       << ",\"state\":\"" << state
                       << "\",\"child\":" << c
                         << ",\"childNodes\":" << (c >= 0 && static_cast<std::size_t>(c) < t.subtreeNodes.size() ? t.subtreeNodes[static_cast<std::size_t>(c)] : 0)
                       << ",\"value\":" << val
                       << ",\"t\":" << ct
                       << ",\"C\":" << cc << "}";
                }
            }
            os << "]}";
        }
        if (n.actions.size() > maxActions) os << "],\"actionsTruncated\":true}";
        else os << "]}";
        return os.str();
    }


    HttpResponse jsonAnalyzeStart(const HttpRequest& req) {
        if (!analyzerActive_) return json("{\"ok\":false,\"reason\":\"not in analyzer mode\"}");
        // 可选设置：?memLimit=MB 覆盖搜索树内存上限（0 = 用默认）。
        const int mb = queryInt(req, "memLimit", 0);
        if (mb > 0) memLimitMbOverride_ = mb;
        startSearch();
        return jsonAnalyzeStatus();
    }

    HttpResponse jsonAnalyzeStop() {
        stopSearch();
        return json("{\"ok\":true}");
    }

    HttpResponse jsonAnalyzeStatus() {
        AnalysisSnapshot snap;
        {
            std::lock_guard lk(snapMutex_);
            snap = searchSnap_;
        }
        std::ostringstream os;
        os << std::setprecision(6);
        os << "{\"valid\":" << (snap.valid ? "true" : "false")
           << ",\"running\":" << (snap.running ? "true" : "false")
           << ",\"reason\":" << jsonString(snap.reason)
           << ",\"candidates\":" << jsonString(snap.candidates);
        if (snap.midgame) os << ",\"midgame\":true";
        if (snap.bruteforce) os << ",\"bruteforce\":true";
        if (snap.firstX > 0) os << ",\"firstMove\":[" << snap.firstX << "," << snap.firstY << "]";
        os << ",\"searchDepth\":" << snap.depth
           << ",\"searchNodes\":" << snap.nodes
           << ",\"searchObserves\":" << snap.observes
           << ",\"value\":" << snap.value
           << ",\"total\":" << snap.total
           << ",\"wins\":" << snap.wins
           << ",\"winRate\":" << snap.winRate
           << ",\"ms\":" << snap.ms
             << ",\"nodeRate\":" << snap.nodeRate
             << ",\"totalMs\":" << snap.totalMs
             << ",\"memLimit\":" << snap.memLimit
           << ",\"tProb\":" << snap.tProb
           << "}";
        return json(os.str());
    }

    HttpResponse jsonAnalyzeTree(const HttpRequest& req) {
        const int nodeId = queryInt(req, "node", 0);
        std::lock_guard lk(searchMutex_);
        if (!searchSession_ || !searchSession_->valid) {
            return json("{\"error\":\"no search session\"}");
        }
        const MidgameSearch::Tree& t = searchSession_->tree;
        if (nodeId < 0 || nodeId >= static_cast<int>(t.nodes.size())) {
            return json("{\"error\":\"bad node id\"}");
        }
        return json(buildTreeNodeJson(t, nodeId));
    }

    // 全量候选招法的棋盘标注数据：质量（存活概率）/ 争议度（搜索树 tLocal）/ 子树节点数。
    // 供棋盘"招法质量"显示（替换概率），一次性返回根节点所有候选，不截断。
    HttpResponse jsonAnalyzeMoves() {
        std::lock_guard lk(searchMutex_);
        if (!searchSession_ || !searchSession_->valid)
            return json("{\"error\":\"no search session\"}");
        const MidgameSearch::Tree& t = searchSession_->tree;
        const MidgameSearch::Node& root = t.nodes[0];
        if (!root.expanded) return json("{\"moves\":[]}");
        const std::size_t na = root.actions.size();

        std::vector<long double> v(na, 0.0L);
        for (std::size_t i = 0; i < na; ++i) {
            const MidgameSearch::Action& a = root.actions[i];
            long double av = a.p;   // 爆炸分支贡献 p×1
            const std::array<double, 9>* d = MidgameSearch::digitOf(root, a);
            if (!d) {
                const long double L = 1.0L - (1.0L - root.dacc) * (1.0L - a.p);
                av = a.p + (1.0L - a.p) * L;
            } else {
                for (int k = 0; k <= 8; ++k) {
                    const double dk = (*d)[static_cast<std::size_t>(k)];
                    if (dk <= 0.0) continue;
                    const int c = MidgameSearch::branchOf(root, static_cast<int>(i), k);
                    long double cv;
                    if (c == MidgameSearch::kDominated)
                        cv = 1.0L;
                    else if (c == MidgameSearch::kUnexp)
                        cv = 1.0L - (1.0L - root.dacc) * (1.0L - a.p);
                    else
                        cv = t.nodes[static_cast<std::size_t>(c)].value;
                    av += dk * cv;
                }
            }
            v[i] = av;
        }

        std::ostringstream os;
        os << std::setprecision(6);
        os << "{\"moves\":[";
        for (std::size_t i = 0; i < na; ++i) {
            const MidgameSearch::Action& a = root.actions[i];
            if (i > 0) os << ',';
            int nodes = 0;
            for (int k = 0; k <= 8; ++k) {
                const int c = MidgameSearch::branchOf(root, static_cast<int>(i), k);
                if (c >= 0) nodes += t.subtreeNodes[static_cast<std::size_t>(c)];
            }
            // 该格子争议度 = 点开它之后各 digit 分支局面的链式争议 t 按 observe 加权：
            //   tO = Σ digit_k × t(子_k)；未展开局面 t 按 1（未知=最大争议），
            //   被支配局面 t 按 0（已判定无需再探）。
            // 与搜索树 refreshNode 里 score = r × cO × tO / mult 用的同一个 tO。
            long double tO = 0.0L;
            const std::array<double, 9>* dg = MidgameSearch::digitOf(root, a);
            if (!dg) {
                tO = 1.0L - static_cast<long double>(a.p);   // 全未观察：Σdigit×1 = 1-p
            } else {
                for (int k = 0; k <= 8; ++k) {
                    const double dk = (*dg)[static_cast<std::size_t>(k)];
                    if (dk <= 0.0) continue;
                    const int c = MidgameSearch::branchOf(root, static_cast<int>(i), k);
                    if (c == MidgameSearch::kDominated) continue;
                    if (c == MidgameSearch::kUnexp) {
                        tO += dk;
                    } else {
                        tO += dk * t.nodes[static_cast<std::size_t>(c)].t;
                    }
                }
            }
            const auto [x, y] =
                std::pair<int, int>{a.cell / (t.cols + 1), a.cell % (t.cols + 1)};
            os << "{\"cell\":" << a.cell << ",\"x\":" << x << ",\"y\":" << y
               << ",\"qual\":" << static_cast<double>(1.0L - v[i])
               << ",\"tO\":" << static_cast<double>(tO)
               << ",\"nodes\":" << nodes << "}";
        }
        os << "]}";
        return json(os.str());
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
    std::unique_ptr<ObservedBoard> editSaved_;  // 分析模式进入时的盘面快照（退出还原）
    std::unique_ptr<MidgameSearch::Session> searchSession_;  // 后台分析线程专用（受 searchMutex_ 保护）
    std::thread searchThread_;
    std::mutex searchMutex_;
    std::mutex snapMutex_;
    std::condition_variable searchCv_;
    bool searchStop_ = false;
    bool searchRunning_ = false;
    bool searchTreeLogged_ = false;
    long long searchTotalMs_ = 0;
    std::deque<std::pair<std::chrono::steady_clock::time_point, long long>> rateSamples_;
    AnalysisSnapshot searchSnap_;
    bool analyzerActive_ = false;
    int port_ = 18080;
    int computedMs_ = 0;
    int memLimitMbOverride_ = 0;  // /api/analyze/start?memLimit= 设置的搜索内存上限（MB），0 = 用默认
};

}  // namespace mss
