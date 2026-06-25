#include "core/log.h"
#include <cstdio>
namespace suji {
void log_info(const std::string& m){ std::fprintf(stderr, "[INFO] %s\n", m.c_str()); }
void log_err (const std::string& m){ std::fprintf(stderr, "[ERR ] %s\n", m.c_str()); }
}
