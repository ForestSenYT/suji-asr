#pragma once
#include "core/types.h"
#include "core/config.h"
#include <string>
namespace suji {
bool transcribe_file(const EngineConfig& cfg, const std::string& input, Transcript& out, std::string& err);
} // namespace suji
