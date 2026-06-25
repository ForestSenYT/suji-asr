#include "core/pipeline.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/asr.h"
#include "core/punctuation.h"
#include "core/segment_merge.h"

namespace suji {

bool transcribe_file(const EngineConfig& cfg, const std::string& input,
                     Transcript& out, std::string& err) {
    // 1. Decode audio to PCM
    AudioBuffer audio;
    if (!decode_to_pcm(cfg.ffmpeg_path, input, audio, err)) return false;

    // 2. VAD — mandatory
    Vad vad(cfg);
    if (!vad.ok()) { err = "VAD init failed"; return false; }

    // 3. ASR — mandatory
    Asr asr(cfg);
    if (!asr.ok()) { err = "ASR init failed"; return false; }

    // 4. Punctuator — non-fatal; passthrough if !ok
    Punctuator punct(cfg);

    // 5. VAD segmentation
    auto segs = vad.segment(audio);

    // 6. ASR per segment; build flat global token list
    std::vector<Token> global_tokens;
    for (const auto& s : segs) {
        auto r = asr.transcribe(s.samples.data(), static_cast<int>(s.samples.size()));
        double base = static_cast<double>(s.start_sample) / 16000.0;
        for (size_t i = 0; i < r.tokens.size() && i < r.timestamps.size(); ++i) {
            Token tk;
            tk.text  = r.tokens[i];
            tk.start = base + r.timestamps[i];
            global_tokens.push_back(tk);
        }
    }

    // 7. Merge tokens into segments
    out.segments = merge_tokens(global_tokens, cfg.merge_gap, cfg.merge_max_dur);

    // 8. Punctuate each segment text; build full_text
    out.full_text.clear();
    for (auto& seg : out.segments) {
        seg.text = punct.add(seg.text);
        out.full_text += seg.text;
    }

    return true;
}

} // namespace suji
