# suji-asr 设计文档 — 中文讲课批量转写 · 可打包 GUI 应用

- **日期**:2026-06-25
- **状态**:架构已与用户确认;待生成实现计划(writing-plans)
- **作者**:Claude Code(基于用户提供的《FINAL ALLINONE Prompt》+ 已核实工具链事实)

---

## 1. 目标与场景

批量把**几小时长的中文现场讲课录像**转成可读文字文档,**离线、高吞吐**(并行多文件、远超播放速度),碾压现用豆包实时转录(一次一个、RTF≈1)。
**最终交付:一个可打包、带图形界面、可分发给同事的 Windows 桌面应用**;并能**因电脑而异自动调整并行度 / GPU-vs-CPU**(足够用的兼容)。

讲课内容特征:单一主讲为主(说话人分离默认关)、带轻微口音普通话(不含方言)、常夹英文术语(zh_en 双语模型)、单文件多小时(VAD 切段)。

衡量指标:**聚合吞吐 = 音频小时 / 墙钟小时** + 整批完成墙钟时间。

---

## 2. 锁定决策(用户已确认)

| 决策 | 选定 |
|---|---|
| 部署目标机 | **RTX 3070 Ti**(Ampere sm_86, 8GB);开发机为 **RTX 2080**(Turing sm_75, 8GB) |
| 运行机器范围 | 我 + 少量同事的 Windows PC(硬件各异,有的弱/旧 N 卡、有的无 GPU) |
| 计算策略 | **CUDA 优先 + 真 CPU 回退**,启动自动探测并选择;不做 AMD/Intel GPU 加速 |
| GUI | **Qt 6 Widgets**(原生 C++,单工具链) |
| 打包 | **all-in-one 离线安装包**(Inno Setup),~2.5–3GB,目标机零前置依赖 |
| ITN | **优先 sherpa-onnx 内置 FST 规则 ITN**,产品内零 Python;wetext 仅开发期对照 |
| FFmpeg | **子进程 + 管道直出 PCM**,bundle `ffmpeg.exe`,不链接 libav |
| sherpa-onnx | **预编译 CUDA C API**,不从源码编译 |

开发机实测(2026-06-25):RTX 2080 8GB(driver 591.86, CUDA 13.1-capable)· Ryzen 7 5800X (8c/16t) · ~40GB RAM · F: 124GB 空闲 · Python 3.13.14 · VS2022。**注意:实机 RAM ~40GB,远超原 spec 假设的 16GB → 在飞文件数约束放宽。**

---

## 3. 已核实技术事实(2026-06-25 联网核实,含对原 spec 的更正)

### 3.1 确认(✅)
- **sherpa-onnx v1.13.3**(2026-06-15)。Windows CUDA 资产:`sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda.tar.bz2`(310,731,099 B),基于 **onnxruntime 1.24.4**。
- GPU bundle **需另装 CUDA 12.x + cuDNN 9.x**(bundle 只带 exe + ORT CUDA provider DLL,不带 CUDA/cuDNN runtime)→ **打包时须 bundle CUDA/cuDNN 可再发行 DLL**。`--provider=cuda` 选 GPU(固定 GPU 0,用 `CUDA_VISIBLE_DEVICES` 重映射)。
- **FireRedASR2-CTC** 资产存在:`sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25.tar.bz2`,含 `model.int8.onnx`(740M)+ `tokens.txt`(77K)+ test_wavs。URL:`https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25.tar.bz2`。zh+en + 20 多口音/方言。**字级时间戳**(中文每字、英文每 word-piece)。
- CLI flag `--fire-red-asr-ctc=`;C-API 选择器字段 `fire_red_asr_ctc`(**区别于** encoder/decoder 变体 `fire_red_asr`)。
- **ITN**:`wetext`(纯 Python wheel)+ `kaldifst`(预编译 cp313 win_amd64 wheel)→ Windows/py3.13 **可一键 pip 装,无 pynini 编译**。另:sherpa-onnx 有**内置 FST/规则 ITN**(`rule_fsts`/`rule_fars`)。
- CT-Transformer 标点(独立步骤):`sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12[-int8].tar.bz2`(tag `punctuation-models`)。
- Silero VAD 模型 `silero_vad.onnx` / `.int8.onnx`(tag `asr-models`)。

### 3.2 更正 / 待实测(⚠️ 不盲信原 spec)
- `DecodeStreams` → **真名 `SherpaOnnxDecodeMultipleOfflineStreams`**(无 `DecodeStreams` 符号)。
- CPU RTF 0.058(~17×)→ **未验证**,必须在 5800X 上实测。
- GPU `num_threads=1` 是否**必须** → **未验证**(示例用了,但无文档说是 GPU 硬性要求)→ Phase 0 用 `--provider=cuda` 变 `--num-threads` 实测。
- Silero VAD `max_speech_duration` 默认 ~20s → 字段名已确认(`max_speech_duration`,**无 `_s` 后缀**),**默认值未验证** → 读 `silero-vad-model-config.cc` 或实测确认。
- Turing sm_75 支持 → **仅推断**(ORT CUDA 12.x EP 支持,但 Turing 处于较旧端)→ **Phase 0 必须在 2080 上实跑 `--provider=cuda` 验证**。
- bundle 内 DLL 清单 → **未枚举**,解压后核实(`onnxruntime_providers_cuda.dll` 等 + 哪些 CUDA/cuDNN DLL 需自带)。

### 3.3 已核实 C API 签名(verbatim,实现以此为准)
```c
// 批量解码(吞吐核心)
void SherpaOnnxDecodeMultipleOfflineStreams(
    const SherpaOnnxOfflineRecognizer *recognizer,
    const SherpaOnnxOfflineStream **streams, int32_t n);

const SherpaOnnxOfflineRecognizer *SherpaOnnxCreateOfflineRecognizer(
    const SherpaOnnxOfflineRecognizerConfig *config);

typedef struct SherpaOnnxOfflineRecognizerConfig {
  SherpaOnnxFeatureConfig feat_config; SherpaOnnxOfflineModelConfig model_config;
  SherpaOnnxOfflineLMConfig lm_config; const char *decoding_method;
  int32_t max_active_paths; const char *hotwords_file; float hotwords_score;
  const char *rule_fsts; const char *rule_fars;   // ← ITN 内置规则挂这里
  float blank_penalty; SherpaOnnxHomophoneReplacerConfig hr;
} SherpaOnnxOfflineRecognizerConfig;

typedef struct SherpaOnnxOfflineFireRedAsrCtcModelConfig { const char *model; }
    SherpaOnnxOfflineFireRedAsrCtcModelConfig;
// SherpaOnnxOfflineModelConfig 内选择器字段: SherpaOnnxOfflineFireRedAsrCtcModelConfig fire_red_asr_ctc;

const SherpaOnnxOfflineStream *SherpaOnnxCreateOfflineStream(const SherpaOnnxOfflineRecognizer *recognizer);
void SherpaOnnxAcceptWaveformOffline(const SherpaOnnxOfflineStream *stream, int32_t sample_rate, const float *samples, int32_t n);
void SherpaOnnxDecodeOfflineStream(const SherpaOnnxOfflineRecognizer *recognizer, const SherpaOnnxOfflineStream *stream);
const SherpaOnnxOfflineRecognizerResult *SherpaOnnxGetOfflineStreamResult(const SherpaOnnxOfflineStream *stream);

typedef struct SherpaOnnxOfflineRecognizerResult {
  const char *text; float *timestamps; int32_t count;   // timestamps 可能为 NULL,读前 null-check
  const char *tokens; const char *const *tokens_arr; const char *json;
  const char *lang; const char *emotion; const char *event; float *durations; float *ys_log_probs;
  const float *segment_timestamps; const float *segment_durations;
  const char *segment_texts; const char *const *segment_texts_arr; int32_t segment_count;
} SherpaOnnxOfflineRecognizerResult;
const char *SherpaOnnxGetOfflineStreamResultAsJson(const SherpaOnnxOfflineStream *stream);
void SherpaOnnxDestroyOfflineRecognizerResult(const SherpaOnnxOfflineRecognizerResult *r);

// VAD
typedef struct SherpaOnnxSileroVadModelConfig {
  const char *model; float threshold; float min_silence_duration;
  float min_speech_duration; int32_t window_size; float max_speech_duration;  // 无 _s 后缀
} SherpaOnnxSileroVadModelConfig;
typedef struct SherpaOnnxVadModelConfig {
  SherpaOnnxSileroVadModelConfig silero_vad; int32_t sample_rate; int32_t num_threads;
  const char *provider; int32_t debug; SherpaOnnxTenVadModelConfig ten_vad;
} SherpaOnnxVadModelConfig;
const SherpaOnnxVoiceActivityDetector *SherpaOnnxCreateVoiceActivityDetector(
    const SherpaOnnxVadModelConfig *config, float buffer_size_in_seconds);  // 注意第二参

// 标点(独立步骤,verbatim 来自 agent;打包前再核实字段位置)
typedef struct SherpaOnnxOfflinePunctuationModelConfig { const char *ct_transformer; int32_t num_threads; int32_t debug; const char *provider; } SherpaOnnxOfflinePunctuationModelConfig;
typedef struct SherpaOnnxOfflinePunctuationConfig { SherpaOnnxOfflinePunctuationModelConfig model; } SherpaOnnxOfflinePunctuationConfig;
const SherpaOnnxOfflinePunctuation *SherpaOnnxCreateOfflinePunctuation(const SherpaOnnxOfflinePunctuationConfig *config);
const char *SherpaOnnxOfflinePunctuationAddPunct(const SherpaOnnxOfflinePunctuation *punctuation, const char *text);
```
> 编码前仍按纪律 #1/#5 复核实际头文件(尤其标点结构体字段位置)。

---

## 4. 总体架构:一套核心,三个外壳

```
                ┌────────────────────────── suji_core (C++ 库) ──────────────────────────┐
                │ HW Probe → 调度策略(provider/batch/in-flight/threads, OOM 自适应)        │
  输入(文件/夹) │ FFmpeg(子进程)→ VAD 切段 → 批量 CTC(DecodeStreams)→ 标点 → ITN → 段落重建 │ → SRT/VTT/JSON/MD
                │ 任务队列 · 错误隔离 · 断点续跑 · 进度/ETA · 取消                            │
                └──────────────▲───────────────────────────────────▲──────────────────────┘
                               │ C++ API(submit/progress cb/cancel) │
                       ┌───────┴────────┐                   ┌────────┴────────┐
                       │ suji_cli (exe) │                   │ suji_gui (Qt)   │
                       │ 开发/benchmark  │                   │ 同事用仪表盘     │
                       └────────────────┘                   └─────────────────┘
```

**`suji_core` 公共 API(草案,实现计划细化)**:`Engine`(创建/销毁、按 `EngineConfig` 初始化 provider+模型)、`submitBatch(paths, options)`、进度回调(每文件:段数/已转秒/状态/错误;全局:聚合吞吐/ETA)、`cancel()`、结果落盘。UI 仅消费回调,不含业务逻辑。

**模块边界**(各自单一职责、可独立测试):`hw_probe` / `media_decode`(ffmpeg)/ `vad` / `asr_batch`(recognizer + DecodeStreams)/ `punct` / `itn` / `segment_merge` / `output_writers` / `orchestrator`(队列+并发+续跑)/ `scheduler`(自适应调参)。

---

## 5. 引擎流水线

1. **媒体 I/O**:`ffmpeg.exe -i <in> -vn -ar 16000 -ac 1 -f s16le -`,从 stdout 管道读 PCM(免临时 WAV);转 float32 喂 ASR。
2. **VAD**:Silero VAD 切段(≤ `max_speech_duration`,带全局起止偏移);段内字级时间戳 + 段 start → **全局单调时间戳**。
3. **批量 ASR**:复用单个 `recognizer` 实例;每段建 `OfflineStream`+`AcceptWaveformOffline`,凑 batch → `SherpaOnnxDecodeMultipleOfflineStreams` → `GetOfflineStreamResult`;结果按 `file_id` 路由回收集器。
4. **标点**:`SherpaOnnxOfflinePunctuationAddPunct`(CTC 输出无标点)。
5. **ITN**:内置 FST(见 §6)。
6. **段落重建**:按停顿/时长把碎 VAD 段并成可读段落(不直接堆 VAD 段)。
7. **输出**:SRT/VTT(字级→字幕条,中文每行 ~15–20 字可配)+ JSON + Markdown(带时间戳锚点)。**统一 UTF-8 无 BOM**。docx 走可选 pandoc 后置,不在 C++ 硬写。

---

## 6. ITN 策略(产品内零 Python)

- **默认**:sherpa-onnx 内置 FST 规则 ITN(`rule_fsts`/`rule_fars`,如 `itn_zh_number.fst`),在 recognizer 配置里挂载,纯 C++。
- **开发期验证**:用 `wetext`(`Normalizer(lang='zh', operator='itn')`)对同一批输出做对照,判断内置 FST 是否够用("十一点""二零二六年"等)。够 → 不进产品;不够 → 再议轻量补丁(不轻易引入 Python 依赖)。
- **不盲信 FunASR 开源 ITN**。

---

## 7. 硬件自适应与 provider 回退("因电脑而异"的核心)

启动期 `hw_probe`:NVIDIA GPU 数 / 各卡可用 VRAM(`cudaMemGetInfo`)/ CPU 逻辑核数 / 总+可用 RAM。`scheduler` 决策:

- **GPU 模式**(有 N 卡 + 空闲 VRAM ≥ ~3GB + CUDA runtime 加载成功):`provider=cuda`、`num_threads=1`(Phase 0 验证)、batch 起 8 按空闲 VRAM 自适应(留 ~1.5GB 余量;若驱动显示器再留 0.5–1GB)、**运行时 OOM 自动减半**;在飞文件数按 RAM。
- **CPU 模式**(无可用 N 卡 / CUDA 加载失败 / VRAM 不足):`provider=cpu`、ASR worker 池按核数、batch 较小、在飞文件数按 RAM(40GB 机高、弱机低)。
- 退化判定要稳:CUDA DLL 缺失/初始化异常 → 捕获并干净回退 CPU,不崩。
- GUI「设置」可手动覆盖(provider、batch、在飞数、线程),**默认全自动**。

---

## 8. 输出与编码
- 格式:SRT / VTT / JSON / Markdown(默认全开,GUI 可选)。
- **全部 UTF-8 无 BOM**(Windows 上防 locale 写成 GBK 乱码;SRT 尤其)。
- 命名:按输入文件名落指定输出目录(`<输入名>.srt/.vtt/.json/.md`),**防重名覆盖**(冲突加后缀)。

---

## 9. 打包与分发(all-in-one 离线 installer)
- 工具:**Inno Setup**。
- 内容:`suji_gui.exe` + Qt DLL(windeployqt)+ sherpa-onnx/ORT DLL + **CUDA 12.x/cuDNN 9.x 可再发行 runtime DLL** + `ffmpeg.exe` + 模型(FireRedASR2-CTC int8 + silero_vad + CT punct int8)+ ITN FST。
- 结果:~2.5–3GB,目标机零前置依赖、离线可用;有 N 卡走 GPU,无则 CPU。
- 在干净 Windows VM/机器 + 3070 Ti 上各验证一次。

---

## 10. 阶段计划(Phase 0–6;每阶段末 Stop:小结+待确认+已验证,等确认再继续;RUNLOG 全程维护)

- **Phase 0 环境+冒烟**(进行中):装 ffmpeg/cmake;下载解压 sherpa CUDA bundle + 模型;核实 bundle/DLL 清单;**CUDA 冒烟**(`--provider=cuda` 生效?`timestamps` 非空?**2080/Turing 能跑?**`num_threads` 影响?VRAM 基线?);许可证(代码 vs 权重)。
- **Phase 1 单文件端到端**(`suji_core`+`suji_cli`,正确性优先):ffmpeg→VAD→CTC→标点→ITN→段落→SRT/VTT/JSON/MD;时间戳全局化;UTF-8 无 BOM 验证。
- **Phase 2 GPU 批量 + 自适应调度**:生产者-消费者(CPU 多线程喂数 / 单 GPU 消费者独占 recognizer);`DecodeStreams` 批量;队尾超时 flush;(可选)长度分桶;`hw_probe`+`scheduler` 自动调参;OOM 减半。
- **Phase 3 批处理编排**:任务队列+并发度;**错误隔离**(单文件失败不影响其余);断点续跑(完成前校验输出非空再跳过);进度/聚合吞吐/ETA;输出防重名覆盖。
- **Phase 4 Benchmark+调优**:真实聚合吞吐(音频h/墙钟h)vs 豆包;扫 batch(8/16/32)/在飞数画吞吐+显存曲线;CER eval(CTC vs Paraformer 备选);显存/内存压测。**2080 数据 provisional,最终以 3070 Ti 为准。**
- **Phase 5 Qt GUI**:仪表盘接 `suji_core`(拖拽、队列表、进度/ETA、设置持久化 QSettings、日志/错误、打开输出);取消;后台线程不卡 UI。
- **Phase 6 打包**:Inno Setup all-in-one;干净机/3070 Ti 实测验证;README + 安装说明。

---

## 11. 非目标(YAGNI)
- 说话人分离(diarization)**默认关**(单主讲);仅"需区分学生提问"时可选开。
- **不维护课程术语表 / 不用热词**(FireRedASR 不支持热词;口音+常用专名已够;日后实测某专名反复错再挂 HomophoneReplacer 拼音替换)。
- 不做 AMD/Intel GPU 加速(CPU 回退覆盖非 N 卡)。
- 不在 C++ 硬写 docx(可选 pandoc 后置)。
- LLM 摘要仅留集成点,不阻塞主流程。
- 排除模型:FireRedASR2-AED(时间戳空)、FireRedASR v1(慢)、FireRedASR2-LLM(需 32GB 显存)。

---

## 12. 待核实清单(写代码/打包前必须落实)
1. sherpa CUDA bundle 实际 DLL 清单 + 是否含可链接的 `c-api.h`/导入库;哪些 CUDA/cuDNN DLL 需自带。
2. 2080/Turing sm_75 上 `--provider=cuda` 实跑通过?(冒烟)
3. GPU 下 `num_threads` 是否必须为 1 / 对吞吐影响。
4. Silero VAD `max_speech_duration` 真实默认值。
5. 标点 C 结构体字段确切布局(复核 `c-api.h`)。
6. 内置 FST ITN 是否够用(对照 wetext)。
7. CPU RTF / 吞吐实测(5800X);3070 Ti 吞吐实测。
8. 各 tar.bz2 压缩包实际大小(F: 空间规划)。
9. ModelScope/HF 国内镜像是否存在(若 GitHub 慢)。
10. `kaldifst` cp313 win_amd64 wheel 本机可装(`pip install wetext` 验证)。

---

## 13. 许可证(逐项核实记 RUNLOG)
Qt(LGPL,动态链接 OK)· FFmpeg(选 LGPL build)· FireRedASR **代码 vs 权重条款分别核实**(权重转自 ModelScope FireRedTeam/FireRedASR2-AED)· CUDA/cuDNN 可再发行条款 · sherpa-onnx(Apache-2.0)。商用前确认。

---

## 14. 交付物
1. C++ 流水线:`suji_core` + `suji_cli` + `suji_gui`(模块清晰)+ CMake 构建说明。
2. benchmark 脚手架:真实聚合吞吐、扫 batch/在飞数曲线、vs 豆包。
3. (建议)自有讲课 CER eval。
4. 样例输出:同一讲课 SRT/JSON/MD。
5. all-in-one installer(Inno Setup)。
6. `RUNLOG.md` + README(依赖/模型/运行/调优/已知限制)+ 各 Phase 设计小结。
