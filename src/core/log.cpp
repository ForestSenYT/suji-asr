#include "core/log.h"
#include <cstdio>
#include <mutex>
namespace suji {

namespace {
static std::mutex   g_sink_mutex;
static LogSink      g_sink;
} // namespace

void set_log_sink(LogSink sink) {
    std::lock_guard<std::mutex> lk(g_sink_mutex);
    g_sink = std::move(sink);
}

static void fire_sink(const std::string& level, const std::string& msg) {
    LogSink copy;
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        copy = g_sink;
    }
    if (copy) copy(level, msg);
}

void log_info(const std::string& m) {
    std::fprintf(stderr, "[INFO] %s\n", m.c_str());
    fire_sink("INFO", m);
}
void log_err(const std::string& m) {
    std::fprintf(stderr, "[ERR ] %s\n", m.c_str());
    fire_sink("ERR", m);
}
} // namespace suji
