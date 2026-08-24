#pragma once

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <source_location>
#include <string_view>

namespace mss {

// 断言：失败时把消息 + 位置写入 mss_error.log 并退出。
// 用函数而非宏，以便拿到调用处的 source_location（C++20）。
inline void assert_(bool condition, std::string_view message,
                    const std::source_location location =
                        std::source_location::current()) {
    if (condition) return;

    static std::mutex logMutex;
    std::lock_guard lock(logMutex);
    std::ofstream log("mss_error.log", std::ios::app);
    if (log) {
        log << "[assert]\n"
            << "message: " << message << '\n'
            << "file: " << location.file_name() << '\n'
            << "line: " << location.line() << '\n'
            << "function: " << location.function_name() << "\n\n";
        log.flush();
        log.close();
    }

    std::exit(1);
}

// 警告：非致命，记录到 mss_error.log，继续执行。
inline void warn_(std::string_view message,
                  const std::source_location location =
                      std::source_location::current()) {
    static std::mutex logMutex;
    std::lock_guard lock(logMutex);
    std::ofstream log("mss_error.log", std::ios::app);
    if (log) {
        log << "[warn]\n"
            << "message: " << message << '\n'
            << "file: " << location.file_name() << '\n'
            << "line: " << location.line() << '\n'
            << "function: " << location.function_name() << "\n\n";
        log.flush();
        log.close();
    }
}

}  // namespace mss