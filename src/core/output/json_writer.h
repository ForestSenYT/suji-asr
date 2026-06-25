#pragma once
#include "core/types.h"
#include <string>
namespace suji { std::string to_json(const Transcript& t); std::string json_escape(const std::string& s); }
