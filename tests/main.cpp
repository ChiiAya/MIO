// MIO 测试入口：运行 tests/ 下所有 MIO_TEST 注册的用例。
// 没有依赖外部测试框架：xmake build mio_tests && ./build/.../mio_tests

#include "framework.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int run = 0;
    for (const auto& tc : miotest::registry()) {
        if (!filter.empty() && tc.name.find(filter) == std::string::npos) continue;
        miotest::currentTest() = tc.name;
        std::cout << "[ RUN  ] " << tc.name << "\n";
        try {
            tc.fn();
        } catch (const std::exception& e) {
            miotest::fail(__FILE__, __LINE__,
                          std::string("未捕获异常: ") + e.what());
        } catch (...) {
            miotest::fail(__FILE__, __LINE__, "未捕获的未知异常");
        }
        ++run;
    }
    std::cout << "[ DONE ] 运行 " << run << " 个用例，检查 " << miotest::checks()
              << " 次，失败 " << miotest::failures() << " 次\n";
    return miotest::failures() == 0 ? 0 : 1;
}
