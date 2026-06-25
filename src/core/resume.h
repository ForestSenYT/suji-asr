#pragma once
#include "core/config.h"
#include <string>
namespace suji {
// true iff out_base's enabled outputs all exist and content is non-empty (segments>0).
// Criterion: each enabled extension file exists; if out_srt then <base>.srt must be size>0
//   (empty SRT = no segments); else if out_json then .json contains "start":;
//   else if out_md then .md contains "**["; else if out_vtt then .vtt contains "-->";
//   nothing enabled -> false.
bool transcript_complete(const std::string& out_base, const EngineConfig& cfg);
}
