#pragma once
#include <functional>
#include <string>
namespace suji {
void log_info(const std::string& m);
void log_err (const std::string& m);

using LogSink = std::function<void(const std::string& level, const std::string& msg)>;
void set_log_sink(LogSink sink);  // pass {} to clear
} // namespace suji
