#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

namespace mss {

// ─────────────────────────────────────────────────────────────
// http_server.h — 极简单线程 HTTP 服务器（127.0.0.1 本机用）。
//
// 只支持 GET/POST、application/json 与静态文件，无 keep-alive。
// 每个请求一个连接、读完即关；够本地 UI 用。
// ─────────────────────────────────────────────────────────────

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType;
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer() { stop(); }

    void setHandler(Handler h) { handler_ = std::move(h); }

    // 启动监听。返回 false = 端口被占/初始化失败。
    bool start(int port) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<u_short>(port));
        if (bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        if (listen(listenSock_, 8) != 0) return false;
        return true;
    }

    void stop() {
        if (listenSock_ != INVALID_SOCKET) {
            closesocket(listenSock_);
            listenSock_ = INVALID_SOCKET;
        }
        WSACleanup();
    }

    // 阻塞接受循环（Ctrl+C 或 stop 退出）。
    // 单线程：一个连接处理完才接下一个。任何异常都不允许把服务带崩。
    void run() {
        running_ = true;
        while (running_) {
            SOCKET client = accept(listenSock_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (WSAGetLastError() == WSAEINTR) continue;  // 被信号打断：继续监听
                break;
            }
            try {
                handleClient(client);
            } catch (...) {
                // 单个连接出错不影响服务存活。
            }
            closesocket(client);
        }
    }

    void requestStop() { running_ = false; }

private:
    std::function<HttpResponse(const HttpRequest&)> handler_;
    SOCKET listenSock_ = INVALID_SOCKET;
    bool running_ = false;

    void handleClient(SOCKET client) {
        // 5 秒读超时：客户端只连不发（预连/半开）也不至于永久阻塞服务。
        {
            DWORD tv = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&tv), sizeof(tv));
        }
        // 读请求头（简单按行读，最多 64KB）。
        std::string raw;
        char buf[4096];
        int received = 0;
        bool headerDone = false;
        while (!headerDone && received < 65536) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<std::size_t>(n));
            received += n;
            const size_t sep = raw.find("\r\n\r\n");
            headerDone = (sep != std::string::npos) ||
                         (raw.find("\n\n") != std::string::npos);
        }

        // 找请求行 + header 结束。
        size_t headerEnd = raw.find("\r\n\r\n");
        const char* nl = "\r\n";
        if (headerEnd == std::string::npos) {
            headerEnd = raw.find("\n\n");
            nl = "\n";
        }
        if (headerEnd == std::string::npos) return;

        const std::string head = raw.substr(0, headerEnd);
        // nl="\r\n"（终止符 \r\n\r\n，4 字节）时 bodyStart = headerEnd + 4；
        // nl="\n" （终止符 \n\n，   2 字节）时 bodyStart = headerEnd + 2。
        const size_t bodyStart = headerEnd + (nl[1] == '\n' ? 4 : 2);

        // 解析请求行。
        std::istringstream lineStream(head);
        std::string requestLine;
        std::getline(lineStream, requestLine);
        std::istringstream rl(requestLine);
        HttpRequest req;
        std::string version;
        if (!(rl >> req.method >> req.path >> version)) return;

        // 解析 query string。
        const size_t qPos = req.path.find('?');
        if (qPos != std::string::npos) {
            const std::string qs = req.path.substr(qPos + 1);
            req.path = req.path.substr(0, qPos);
            std::istringstream qss(qs);
            std::string pair;
            while (std::getline(qss, pair, '&')) {
                const size_t eq = pair.find('=');
                if (eq == std::string::npos) continue;
                req.query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }

        // Content-Length。
        int contentLength = 0;
        std::istringstream hs(head);
        std::string line;
        while (std::getline(hs, line)) {
            if (line.rfind("Content-Length:", 0) == 0) {
                contentLength = std::atoi(line.c_str() + 15);
            } else if (line.rfind("content-length:", 0) == 0) {
                contentLength = std::atoi(line.c_str() + 15);
            }
        }

        // 补齐 body。
        while (received < static_cast<int>(bodyStart) + contentLength && received < 65536) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<std::size_t>(n));
            received += n;
        }
        if (static_cast<int>(raw.size()) >= bodyStart + contentLength)
            req.body = raw.substr(bodyStart, static_cast<std::size_t>(contentLength));

        // 分发 + 回包。
        HttpResponse res = handler_ ? handler_(req) : HttpResponse{404, "text/plain", "no handler"};
        if (res.body.empty() && res.status == 200) res.status = 204;

        std::ostringstream out;
        out << "HTTP/1.1 " << res.status << " "
            << statusText(res.status) << "\r\n"
            << "Content-Type: " << res.contentType << "\r\n"
            << "Content-Length: " << res.body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Connection: close\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "\r\n"
            << res.body;
        const std::string response = out.str();
        send(client, response.data(), static_cast<int>(response.size()), 0);
    }

    static std::string urlDecode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                out += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
                i += 2;
            } else if (s[i] == '+') {
                out += ' ';
            } else {
                out += s[i];
            }
        }
        return out;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    static const char* statusText(int code) {
        switch (code) {
            case 200: return "OK";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            default: return "OK";
        }
    }
};

}  // namespace mss