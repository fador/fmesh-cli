// minitest: a minimal header-only C++ test framework.
// Supports TEST, TEST_F, ASSERT_*/EXPECT_* macros, SUCCEED().
// Usage: #include "minitest.h"  and link with tests/test_main.cpp.
#pragma once

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace minitest {

struct TestCase {
    std::string name;
    void (*func)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> reg;
    return reg;
}

inline bool& failed_flag() {
    thread_local bool f = false;
    return f;
}

inline int run() {
    int passed = 0, failed = 0;
    for (auto& tc : registry()) {
        std::printf("[ RUN      ] %s\n", tc.name.c_str());
        failed_flag() = false;
        try {
            tc.func();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  EXCEPTION: %s\n", e.what());
            failed_flag() = true;
        } catch (...) {
            std::fprintf(stderr, "  UNKNOWN EXCEPTION\n");
            failed_flag() = true;
        }
        if (failed_flag()) {
            ++failed;
            std::printf("[  FAILED  ] %s\n", tc.name.c_str());
        } else {
            ++passed;
            std::printf("[       OK ] %s\n", tc.name.c_str());
        }
    }
    std::printf("[==========] %d tests ran.\n", passed + failed);
    std::printf("[  PASSED  ] %d tests.\n", passed);
    if (failed) std::printf("[  FAILED  ] %d tests.\n", failed);
    return failed ? 1 : 0;
}

} // namespace minitest

// ---- assertion macros ------------------------------------------------

#define MT_FAIL_AT(file, line, msg) do { \
    std::fprintf(stderr, "  %s:%d: FAIL: %s\n", file, line, msg); \
    minitest::failed_flag() = true; \
} while(0)

#define ASSERT_TRUE(cond)  do { if (!(cond)) { MT_FAIL_AT(__FILE__,__LINE__,"ASSERT_TRUE("#cond")"); return; } } while(0)
#define ASSERT_FALSE(cond) do { if  (cond)  { MT_FAIL_AT(__FILE__,__LINE__,"ASSERT_FALSE("#cond")"); return; } } while(0)
#define ASSERT_EQ(a,b)     do { if (!((a)==(b))){ MT_FAIL_AT(__FILE__,__LINE__,"ASSERT_EQ"); return; } } while(0)
#define ASSERT_NE(a,b)     do { if (!((a)!=(b))){ MT_FAIL_AT(__FILE__,__LINE__,"ASSERT_NE"); return; } } while(0)
#define ASSERT_GT(a,b)     do { if (!((a)>(b))) { MT_FAIL_AT(__FILE__,__LINE__,"ASSERT_GT"); return; } } while(0)

#define EXPECT_TRUE(cond)  do { if (!(cond))  { MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_TRUE("#cond")");  } } while(0)
#define EXPECT_FALSE(cond) do { if  (cond)    { MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_FALSE("#cond")"); } } while(0)
#define EXPECT_EQ(a,b)     do { if (!((a)==(b))){ MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_EQ"); } } while(0)
#define EXPECT_NE(a,b)     do { if (!((a)!=(b))){ MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_NE"); } } while(0)
#define EXPECT_GT(a,b)     do { if (!((a)>(b))) { MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_GT"); } } while(0)
#define EXPECT_FLOAT_EQ(a,b) do { \
    if (std::fabs(static_cast<double>(a)-static_cast<double>(b)) > 1e-6) \
        { MT_FAIL_AT(__FILE__,__LINE__,"EXPECT_FLOAT_EQ"); } \
} while(0)

#define SUCCEED() do {} while(0)

// ---- TEST(suite, name) -----------------------------------------------
// Uses suite+name concatenation for unique identifiers (suite and name
// are literal tokens from the macro call site, no macro expansion needed).

#define MT_CAT3(a,b,c) a##b##c

#define TEST(suite, name)                                             \
    static void MT_CAT3(mt_body_, suite, name)();                     \
    namespace {                                                       \
        struct MT_CAT3(mt_reg_, suite, name) {                        \
            MT_CAT3(mt_reg_, suite, name)() {                         \
                minitest::registry().push_back(                       \
                    {#suite "." #name,                               \
                     MT_CAT3(mt_body_, suite, name)});               \
            }                                                         \
        } MT_CAT3(mt_reg_inst_, suite, name);                         \
    }                                                                 \
    static void MT_CAT3(mt_body_, suite, name)()

// ---- TEST_F(fixture, name) ------------------------------------------

#define TEST_F(fixture, name)                                         \
    struct MT_CAT3(mt_fix_, fixture, name) : public fixture           \
        { void TestBody(); };                                         \
    static void MT_CAT3(mt_run_, fixture, name)() {                   \
        MT_CAT3(mt_fix_, fixture, name) t;                            \
        t.SetUp();                                                    \
        t.TestBody();                                                 \
        t.TearDown();                                                 \
    }                                                                 \
    namespace {                                                       \
        struct MT_CAT3(mt_reg_, fixture, name) {                      \
            MT_CAT3(mt_reg_, fixture, name)() {                       \
                minitest::registry().push_back(                       \
                    {#fixture "." #name,                             \
                     MT_CAT3(mt_run_, fixture, name)});              \
            }                                                         \
        } MT_CAT3(mt_reg_inst_, fixture, name);                       \
    }                                                                 \
    void MT_CAT3(mt_fix_, fixture, name)::TestBody()
