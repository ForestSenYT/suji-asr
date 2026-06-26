#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace suji {
struct AudioBuffer { std::vector<float> samples; int sample_rate = 16000; };
struct SpeechSeg   { int64_t start_sample = 0; std::vector<float> samples; }; // VAD 输出,start 为采样点
struct AsrResult   { std::string text; std::vector<std::string> tokens; std::vector<double> timestamps; }; // 单段本地时间(秒)
struct Token       { std::string text; double start = 0.0; double end = 0.0; }; // 全局秒; end 由 merge_tokens 从 token 流计算
struct Segment     { double start = 0.0; double end = 0.0; std::string text; std::vector<Token> tokens; };
struct Transcript  { std::vector<Segment> segments; std::string full_text; };
} // namespace suji
