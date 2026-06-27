# fp16 Model Research for suji-asr GPU Acceleration

Date: 2026-06-26
Context: suji-asr (Windows C++ batch zh+en lecture transcription via sherpa-onnx).
Current model: `sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25` (int8 only).
Problem: ORT CUDA EP does not accelerate int8 (QDQ) ops, so RTX 2080 (Turing) /
3070 Ti (Ampere) tensor cores sit idle. We want an fp16 model so the GPU helps.

> Note: All web research below was performed with live WebSearch/WebFetch + the
> GitHub/HuggingFace public APIs. URLs are cited inline and verified (the release
> asset list was paginated through the GitHub API, not just read from docs).

---

## TL;DR / Top Recommendation

1. **A ready-made fp16 of the EXACT model (FireRedASR2-CTC v2) does NOT exist.**
   The k2-fsa `asr-models` release ships **only** `...fire-red-asr2-ctc-zh_en-int8-2026-02-25`.
   Paginated the full asset list via the GitHub API — there are exactly 4 fire-red
   assets and none is an fp16/fp32 CTC v2. The source ONNX lives on ModelScope and
   only the int8 was published to sherpa-onnx.

2. **BUT a ready-made fp16 zh+en FireRedASR DOES exist for v1:**
   **`sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16`** (offline AED, encoder
   1.55 GB + decoder 789 MB fp16, drop-in for sherpa-onnx). This is the same model
   family the int8 v2 descends from. **This is the fastest path to "GPU actually
   helps" with no Python work.**

3. **Best DIY path to keep the v2-CTC architecture:** convert the v2-CTC ONNX to fp16
   with `onnxconverter_common.float16` (`keep_io_types=True`), preserving the embedded
   sherpa metadata. The sherpa-onnx C++ CTC loader is **precision-agnostic** (verified
   in source): it loads whatever ONNX path you give and only reads metadata strings, so
   an fp16 file Just Works as long as you don't strip the metadata. The catch: you need
   the **non-int8 (fp32) v2-CTC ONNX** as the conversion input, which is not on the
   GitHub release; it must be re-exported/obtained from ModelScope first (NEEDS-HUMAN).

**Concrete "do this":**
- Step 1 (today, zero code): download the v1 **fp16** model, point suji-asr at
  `encoder.fp16.onnx`/`decoder.fp16.onnx`, run with `provider=cuda`, benchmark RTF
  vs the current int8 on the 2080/3070 Ti. If accuracy on your lectures is acceptable,
  ship it.
- Step 2 (optional, if you specifically want the faster v2-**CTC** topology on GPU):
  obtain the fp32 v2-CTC ONNX from ModelScope, run the float16 conversion snippet
  below, re-attach metadata, and swap the file in.

---

## Q1. Is there a non-int8 (fp32/fp16) FireRedASR2-CTC ONNX published?

**No (for v2-CTC).** Verified by paginating the entire `asr-models` GitHub release
asset list (release id 130628817) via the GitHub API. The only `fire-red` assets are:

| Asset | Precision | Type | zh+en | Offline |
|---|---|---|---|---|
| `sherpa-onnx-fire-red-asr-large-zh_en-2025-02-16.tar.bz2` | fp32 | v1 AED | yes | yes |
| `sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16.tar.bz2` | **fp16** | v1 AED | yes | yes |
| `sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25.tar.bz2` | **int8 only** | v2 CTC | yes | yes |
| `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2` | int8 only | v2 AED | yes | yes |

So: the v1 model has fp32 + fp16; the **v2 models (the CTC you currently use) ship int8 only.**

- Docs page (lists only int8 for v2):
  https://k2-fsa.github.io/sherpa/onnx/FireRedAsr/pretrained.html
- Release tag (full asset list): https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
- Docs PR/issue confirming only int8 for v2 was uploaded:
  https://github.com/k2-fsa/sherpa-onnx/issues/3228
- v1 fp16 HuggingFace mirror (confirmed `encoder.fp16.onnx` 1.55 GB + `decoder.fp16.onnx` 789 MB + tokens.txt):
  https://huggingface.co/csukuangfj/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25 (int8 v2)
  https://huggingface.co/csukuangfj/sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16 (fp16 v1) — verified file list.

**Architecture (confirmed):** FireRedASR2 is a **Conformer** encoder (conv subsampling +
stacked Conformer blocks). The "CTC" variant is a lightweight linear CTC projection head
trained post-hoc on top of the frozen Conformer encoder. The sherpa-onnx v2-CTC ships
only the encoder + CTC head (the attention decoder branch is dropped), which is why it is
"very fast on CPU". Source: FireRedASR2 paper https://arxiv.org/html/2603.10420v1 and
upstream repo https://github.com/FireRedTeam/FireRedASR2S . The v2-CTC ONNX is converted
from ModelScope `FireRedTeam/FireRedASR2-AED`
(https://www.modelscope.cn/models/FireRedTeam/FireRedASR2-AED).

---

## Q2. Conversion path (fp32 -> fp16) and sherpa-onnx compatibility

### Does sherpa-onnx accept an fp16 ONNX as-is?
**Yes.** Verified directly in the C++ loader
`sherpa-onnx/csrc/offline-fire-red-asr-ctc-model.cc`
(https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/master/sherpa-onnx/csrc/offline-fire-red-asr-ctc-model.cc):

- It builds the ORT session from whatever path `config.fire_red_asr_ctc.model` points to.
- It reads only **metadata strings**: `model_type` (must equal `fire-red-asr-2-ctc`),
  plus `subsampling_factor`, `vocab_size`, `cmvn_mean`, `cmvn_inv_stddev`.
- It does **not** inspect input/output tensor dtype anywhere.

So you can drop in an fp16 `model.onnx`, keep the same `OfflineRecognizerConfig`
(`model_config.fire_red_asr_ctc.model = "model.fp16.onnx"`), set `provider = "cuda"`,
and it loads. The **only hard requirement** is that the fp16 ONNX still carries the
sherpa metadata (`model_type`, cmvn, etc.). `float16.convert_float_to_float16` preserves
the `ModelProto.metadata_props`, so as long as you start from the metadata-bearing model
and `onnx.save` the result without stripping it, you're fine. If in doubt, re-run
sherpa's metadata script (`add-model-metadata.py`-style) after conversion.

### The conversion (NEEDS-HUMAN: requires the fp32 v2-CTC ONNX as input)
The GitHub release does not ship fp32 v2-CTC. To get the fp32 input you must obtain it
from the ModelScope export (the int8 was produced *from* an fp32 ONNX during sherpa's
conversion). Practical options, in order:
- (a) Ask the sherpa-onnx maintainer (csukuangfj) to also upload an fp16 v2-CTC — this is
  the cleanest fix and they already do fp16 for other models; an upstream issue is cheap.
- (b) Obtain the fp32 v2-CTC ONNX from the ModelScope conversion scripts / repo and convert
  locally.

Once you have the fp32 `model.onnx` with metadata:

```python
# pip install onnx onnxconverter-common
import onnx
from onnxconverter_common import float16

m = onnx.load("model.onnx")                      # fp32 v2-CTC with sherpa metadata
m_fp16 = float16.convert_float_to_float16(
    m,
    keep_io_types=True,        # keep model inputs/outputs fp32 -> no API/feature changes
    # op_block_list=[...],     # add ops here if accuracy regresses (see pitfalls)
)
onnx.save(m_fp16, "model.fp16.onnx")
# Verify metadata survived:
print({p.key: p.value for p in onnx.load("model.fp16.onnx").metadata_props})
```

If full-fp16 hurts accuracy, use mixed precision instead (needs a GPU + a sample input):

```python
from onnxconverter_common import auto_mixed_precision
import numpy as np
feed = {"x": np.random.randn(1, 1000, 80).astype(np.float32),
        "x_lens": np.array([1000], dtype=np.int64)}   # match real input names/shapes
m_mixed = auto_mixed_precision.auto_convert_mixed_precision(
    m, feed, rtol=0.01, atol=0.001, keep_io_types=True)
onnx.save(m_mixed, "model.fp16.onnx")
```

**Pitfalls (documented):**
- Some ops are not supported in fp16 on ORT; the converter inserts Cast nodes around the
  default block-list automatically, but layer-norm / softmax / certain reductions can lose
  precision — add them to `op_block_list` if CER regresses. The Conformer's LayerNorm and
  the CTC log-softmax are the usual suspects to keep in fp32.
- `keep_io_types=True` keeps the feature-input and logits-output fp32 so nothing else in
  suji-asr changes (recommended).
- Models > 2 GB with external data need the external-data path on save (the v2-CTC is
  ~740 MB int8, so the fp32 source is ~1.4 GB — under 2 GB, fine as a single file).

Refs:
- ORT fp16 guide: https://onnxruntime.ai/docs/performance/model-optimizations/float16.html
- `float16.py`: https://github.com/microsoft/onnxconverter-common/blob/master/onnxconverter_common/float16.py

---

## Q3. Does fp16 actually help on RTX 2080 / 3070 Ti for this architecture?

**Yes — and the premise is correct that int8 is the wrong precision for ORT-CUDA here.**

- **int8 on ORT CUDA EP is frequently slower than fp32**, not faster. Multiple ORT issues
  report QDQ/int8 models running far slower on the CUDA EP (e.g. 12 ms -> 232 ms on V100;
  T4 ResNet/BERT int8 slower than fp32) because the standard CUDA kernels have limited int8
  support and fall back / insert dequant casts. int8 tensor-core speedup on NVIDIA really
  needs the **TensorRT EP**, not the CUDA EP.
  - https://github.com/microsoft/onnxruntime/issues/6732
  - https://github.com/microsoft/onnxruntime/issues/10267
  - https://github.com/microsoft/onnxruntime/issues/12229
- **fp16 on ORT CUDA EP does use tensor cores** on Turing (RTX 2080, SM 7.5) and Ampere
  (RTX 3070 Ti, SM 8.6) — both have FP16 tensor cores. Reports consistently show fp16
  "performs well" on CUDA where int8 does not. Caveat: the CUDA EP's fp16 gain is real but
  more modest than TensorRT (TensorRT can be ~2-3x over CUDA EP); expect a solid speedup
  and lower VRAM, not a miracle. The big win here is simply *escaping the int8 GPU penalty*.
  - https://github.com/microsoft/onnxruntime/issues/4769 (fp16 not faster only when the
    model is cast-bound / IO-bound — mitigated by `keep_io_types` + a compute-heavy Conformer)
- sherpa-onnx's own guidance: for CUDA inference, **download the fp16 model and replace the
  int8 files** — i.e. the maintainers explicitly steer CUDA users to fp16, confirming the
  approach. (Stated on the FireRedASR/SenseVoice pretrained pages and GPU-support docs.)
  https://k2-fsa.github.io/sherpa/onnx/FireRedAsr/pretrained.html

Net: for a compute-bound Conformer/CTC encoder, fp16 on the 2080/3070 Ti should give a real
GPU throughput improvement over the current int8, which is the whole point.

---

## Q4. Alternative GPU-friendly zh+en models in sherpa-onnx

Searched the full `asr-models` release for fp16 ASR assets. The **offline** (batch, best
accuracy) fp16 zh options are limited; most fp16 zh models are *streaming* zipformers
(lower accuracy than offline, not ideal for lectures). Summary:

| Model | In sherpa-onnx | fp16 ready? | Offline? | zh+en lectures | Size (fp16) | Notes |
|---|---|---|---|---|---|---|
| **FireRedASR v1 large (AED)** `fire-red-asr-large-zh_en-fp16-2025-02-16` | yes | **yes (ready)** | yes | **excellent** zh+en (same family as current) | ~2.3 GB (enc 1.55 + dec 789 MB) | Encoder-decoder (beam search) => slower per-utt than CTC, but GPU-accelerated fp16. **Top alternative.** |
| **FireRedASR2 CTC (v2, current)** | yes | **no** (int8 only) | yes | excellent | n/a | Needs DIY fp16 conversion (Q2). Fastest topology if converted. |
| **SenseVoice** `sense-voice-zh-en-ja-ko-yue` | yes | **no** (int8/fp32 only) | yes | good zh, **mediocre EN**; weaker on en-heavy lecture content | ~230 MB int8 | Very fast, tiny, but no fp16 release and weaker English/code-switch than FireRed. |
| **Paraformer** `paraformer-zh` / `paraformer-trilingual-zh-cantonese-en` | yes | **no fp16** | yes | good zh, ok en | ~220 MB | No offline fp16 asset; would need conversion. zh-centric. |
| **Whisper large-v3** | yes (export script) | not as a release asset | yes | strong multilingual zh+en, robust | ~3 GB fp16 | fp16-friendly on GPU but heavy; high latency; ORT CUDA EP encoder-decoder. Export-it-yourself; good accuracy, slower. |
| **Zipformer-CTC zh** `zipformer-ctc-zh-fp16-2025-07-03` / `-small-` | yes | **yes (ready)** | yes | strong **zh**, **no English** | ~small | Ready fp16 + true CTC (fast on GPU) but zh-only — fails the en requirement for lectures. |
| **Streaming zipformer zh fp16** (several) | yes | yes | no (streaming) | lower accuracy | small | Not for batch lectures. |
| **NeMo Parakeet TDT 0.6B v2 fp16** | yes | yes | yes | **English only** | ~1.2 GB | Great EN, no Chinese. |
| **FunASR-nano fp16** `funasr-nano-fp16-2025-12-30` | yes | partly (llm.fp16 + int8 enc) | yes | zh + 7 dialects; LLM-based | ~1.5 GB | Heavy LLM decoder; encoder still int8; zh-dialect focused, not a clean CTC swap. |

Sources: release asset list (GitHub API), SenseVoice
https://k2-fsa.github.io/sherpa/onnx/sense-voice/pretrained.html , Paraformer
https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-paraformer/paraformer-models.html ,
Whisper https://k2-fsa.github.io/sherpa/onnx/pretrained_models/whisper/large-v3.html ,
FunASR-nano https://k2-fsa.github.io/sherpa/onnx/funasr-nano/pretrained.html .

**Key takeaway:** Among *ready-made, offline, zh+en* fp16 models in sherpa-onnx, the only
one that matches your accuracy bar is **FireRedASR v1 large fp16**. Everything else is
either zh-only (Zipformer-CTC), en-only (Parakeet), weaker on English (SenseVoice/Paraformer),
or heavy (Whisper/FunASR). So the realistic choices are: **(A) switch to FireRedASR v1 fp16
now**, or **(B) DIY-convert the v2-CTC to fp16** to keep the faster CTC topology.

---

## Q5. Ranked recommendation

Ranking by (practicality of getting fp16) x (GPU speedup) x (zh+en lecture accuracy):

1. **FireRedASR v1 large fp16 (ready-made) — DO THIS FIRST.**
   - Practicality: highest (one download, no Python).
   - GPU: fp16 -> tensor cores on both GPUs; escapes the int8 penalty.
   - Accuracy: same family, excellent zh+en for lectures.
   - Tradeoff: AED encoder-decoder is heavier per-utterance than the v2 CTC head, so
     per-clip latency is higher than a converted v2-CTC would be — but it actually uses the
     GPU, which int8-v2 does not. Validate RTF on your 2080/3070 Ti; with VAD-chunked
     lecture audio it should be fine for batch.
   - Download:
     ```bash
     curl -L -O https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16.tar.bz2
     tar xvf sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16.tar.bz2
     # gives encoder.fp16.onnx, decoder.fp16.onnx, tokens.txt, test_wavs/
     ```
   - Wire-up: this is the **AED** recognizer, not the CTC one. In suji-asr point the
     FireRedASR (encoder+decoder) config at `encoder.fp16.onnx` / `decoder.fp16.onnx`
     (config field `model_config.fire_red_asr.{encoder,decoder}`), set `provider="cuda"`.
     (Different config struct than the current `fire_red_asr_ctc.model` single-file path.)

2. **DIY fp16 of the current v2-CTC — DO THIS IF you want the fastest GPU topology and are
   willing to source the fp32 ONNX.**
   - Practicality: medium (needs the fp32 v2-CTC ONNX + a one-off Python convert).
   - GPU: fp16 CTC head on Conformer = best combination of GPU speedup + low decode cost.
   - Accuracy: identical to today's model (it *is* today's model, de-quantized).
   - Action: file an issue asking csukuangfj to publish `...fire-red-asr2-ctc-zh_en-fp16`,
     OR get the fp32 ONNX from ModelScope and run the Q2 snippet. Keep config unchanged
     except the filename (loader is precision-agnostic).

3. **Whisper large-v3 fp16** — fallback only if you need maximum robustness and can absorb
   latency; heavy, export-it-yourself, slowest.

4. **SenseVoice / Paraformer / Zipformer-CTC** — not recommended for this requirement:
   either no fp16, zh-only, or weaker English/code-switch than FireRed (lectures here are
   zh+en mixed).

### NEEDS-HUMAN flags
- **Download (account-free):** v1 fp16 tarball from GitHub releases — just `curl`/`tar`. No login.
- **Source the fp32 v2-CTC ONNX (option 2):** not on the GitHub release; obtain via ModelScope
  export or by requesting an fp16 upstream upload. Account/region may matter for ModelScope.
- **Run Python conversion (option 2):** `pip install onnx onnxconverter-common`, run the
  `float16` snippet, verify metadata survived. Requires the human to execute and eyeball CER.
- **License:** FireRedASR models carry the FireRedTeam license — verify it permits your
  batch/redistribution use before shipping. https://github.com/FireRedTeam/FireRedASR2S

---

## Appendix: how the asset list was verified
GitHub API, release `asr-models` (id 130628817), paginated `assets?per_page=100&page=1..10`,
filtered for `fire-red`, `fp16`, `sense-voice`, `paraformer`. Result: exactly 4 fire-red
assets (2 v1 fp32+fp16, 2 v2 int8); fp16 offline zh assets limited to zipformer-ctc-zh
(zh-only), funasr-nano (partly int8), fire-red v1, and parakeet (en-only).

## Sources
- https://k2-fsa.github.io/sherpa/onnx/FireRedAsr/pretrained.html
- https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
- https://github.com/k2-fsa/sherpa-onnx/issues/3228
- https://huggingface.co/csukuangfj/sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16
- https://www.modelscope.cn/models/FireRedTeam/FireRedASR2-AED
- https://github.com/FireRedTeam/FireRedASR2S
- https://arxiv.org/html/2603.10420v1
- https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/master/sherpa-onnx/csrc/offline-fire-red-asr-ctc-model.cc
- https://onnxruntime.ai/docs/performance/model-optimizations/float16.html
- https://github.com/microsoft/onnxconverter-common/blob/master/onnxconverter_common/float16.py
- https://github.com/microsoft/onnxruntime/issues/4769
- https://github.com/microsoft/onnxruntime/issues/6732
- https://github.com/microsoft/onnxruntime/issues/10267
- https://github.com/microsoft/onnxruntime/issues/12229
- https://k2-fsa.github.io/sherpa/onnx/sense-voice/pretrained.html
- https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-paraformer/paraformer-models.html
- https://k2-fsa.github.io/sherpa/onnx/pretrained_models/whisper/large-v3.html
- https://k2-fsa.github.io/sherpa/onnx/funasr-nano/pretrained.html
