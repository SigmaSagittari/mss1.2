#include "ui/ui_app.h"

// 临时性能剖析入口（profile 完撤）：从 main 直接跑结构层剖析后退出。
void runStructPerfTest();

int main() {
    runStructPerfTest();
    return mss::UiApp().run();
}