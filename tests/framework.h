#pragma once
// ============================================================================
// MIO 最小测试框架（无外部依赖；主 agent 冻结，子任务只添加自己的 test_*.cpp）
//
// 用法：
//     #include "framework.h"
//     MIO_TEST(我的用例) {
//         CHECK_TRUE(1 + 1 == 2);
//         CHECK_EQ(2, 1 + 1);
//     }
//
// 约束：不得用真实用户记录或付费 API 验证；调度测试使用可注入时钟，
//       摘要和插件使用 fake，数据库使用临时目录。
// ============================================================================

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace miotest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline int& checks() {
    static int c = 0;
    return c;
}

inline std::string& currentTest() {
    static std::string s;
    return s;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back(TestCase{name, std::move(fn)});
    }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "  [FAIL] %s:%d (%s) %s\n", file, line,
                 currentTest().c_str(), what.c_str());
}

template <typename T>
std::string show(const T& v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

inline std::string show(bool v) { return v ? "true" : "false"; }

} // namespace miotest

#define MIO_TEST(test_name)                                                  \
    static void test_name();                                                 \
    static ::miotest::Registrar mio_reg_##test_name(#test_name, test_name);  \
    static void test_name()

#define CHECK_TRUE(cond)                                                     \
    do {                                                                     \
        ++::miotest::checks();                                               \
        if (!(cond)) ::miotest::fail(__FILE__, __LINE__, "CHECK_TRUE(" #cond ")"); \
    } while (0)

#define CHECK_FALSE(cond)                                                    \
    do {                                                                     \
        ++::miotest::checks();                                               \
        if ((cond)) ::miotest::fail(__FILE__, __LINE__, "CHECK_FALSE(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++::miotest::checks();                                               \
        auto&& mio_a = (a);                                                  \
        auto&& mio_b = (b);                                                  \
        if (!(mio_a == mio_b)) {                                             \
            ::miotest::fail(__FILE__, __LINE__,                              \
                            std::string("CHECK_EQ(" #a ", " #b ") 期望=") +  \
                                ::miotest::show(mio_b) + " 实际=" +          \
                                ::miotest::show(mio_a));                     \
        }                                                                    \
    } while (0)

#define CHECK_NE(a, b)                                                       \
    do {                                                                     \
        ++::miotest::checks();                                               \
        auto&& mio_a = (a);                                                  \
        auto&& mio_b = (b);                                                  \
        if ((mio_a == mio_b)) {                                              \
            ::miotest::fail(__FILE__, __LINE__, "CHECK_NE(" #a ", " #b ")"); \
        }                                                                    \
    } while (0)
