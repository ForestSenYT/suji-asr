#include "doctest/doctest.h"
#include "core/log.h"
#include <string>
#include <vector>

using namespace suji;

TEST_CASE("set_log_sink receives log_info messages") {
    struct Entry { std::string level; std::string msg; };
    std::vector<Entry> captured;

    set_log_sink([&](const std::string& lvl, const std::string& m) {
        captured.push_back({lvl, m});
    });

    log_info("hello");
    log_err("oops");

    CHECK(captured.size() == 2);
    CHECK(captured[0].level == "INFO");
    CHECK(captured[0].msg   == "hello");
    CHECK(captured[1].level == "ERR");
    CHECK(captured[1].msg   == "oops");

    // After clearing, no more messages captured
    set_log_sink({});
    log_info("invisible");
    CHECK(captured.size() == 2);  // still 2, not 3
}
