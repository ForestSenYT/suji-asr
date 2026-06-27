# MISSION PROMPT — suji-asr 性能优化(榨干 CPU+GPU 吞吐)2026-06-26

> 交给自主编码 agent 端到端执行。本文档即全部上下文。严格按"执行纪律";每个改动**必须 benchmark 前后对比**,无提升或回退就回滚。不 push(push = NEEDS-HUMAN)。

## 1. 背景与现状(已实测,别推翻)
suji-asr = Windows C++ 批量中文转写(ffmpeg→Silero VAD→FireRedASR2-CTC **int8**→标点→SRT/VTT/JSON/MD)。已实现**异构 CPU+GPU 引擎**(`transcribe_batch_files_hetero`,work-stealing 双消费者)。

**dev 机:RTX 2080(8GB)+ Ryzen 5800X(16 线程)+ 40GB。部署机:RTX 3070 Ti。**

**已实测的硬事实(`BENCHMARK.md` + 决策日志):**
- **CPU(16线程,长文件)≈ 4.8-4.95×;GPU(2080,int8)≈ 1.8-2.85×;异构(长文件)≈ 5.8×**(CPU 64%/GPU 35%,异构最快)。
- **int8 在 ORT-CUDA EP 上加速差**(int8 是 CPU 的 VNNI 优化;GPU 上 int8 常退化为 fp32 + quant/dequant + D2H/H2D 拷贝)。**这是 GPU 半边弱的根因。**
- **线程分配已是最优**:实测扫描 `cpu_asr_threads ∈ {11,13,14,15,16}`,**11(=C-P-gpu_feed)就是峰值**,往上加反而更慢(单 ONNX session 的 intra-op 次线性扩展 + 抢 GPU host 核)。**别再调线程数当主优化。**
- 当前 `fill_hetero`:`P=clamp(C/4,2,6)=4` 生产者,`cpu_asr_threads=11`,`gpu_feed=1`,`gpu_batch=clamp((vram_free-2000)/150,8,32)`。`--cpu-threads`/`--cpu-batch`/`--gpu-batch` 可手动覆盖。

**结论:2080 + int8 的天花板就在 ~6×。要再快,核心是让 GPU 真正发力——而 GPU 对 int8 无能为力,所以最大杠杆是 fp16 模型。**

## 2. 本次目标
在**不破坏异构引擎并发正确性、不回退现有吞吐**的前提下,把聚合吞吐往上抬。按"预期收益 × 可行性"排序做下面的任务,每个都**实测验证**。

## 3. 任务清单(按优先级)

### P1 —— 【最大杠杆,部分 NEEDS-HUMAN】fp16 模型,让 GPU 真正发力
- **假设**:GPU 对 fp16 有张量核加速,fp16 的 GPU 半边吞吐会远高于现在的 int8(可能从 1.8× 跳到 5×+),异构 margin 随之大涨;3070 Ti 上更甚。
- **做法**:
  1. **获取 fp16 模型(NEEDS-HUMAN 决策来源)**:① 看 sherpa-onnx / FireRedASR 作者是否提供现成 fp16/fp32 ONNX;② 否则拿 ModelScope 上 FireRedASR 的**原始 fp32** 权重,用 `onnxruntime` 导出 fp32 ONNX 再 `python -m onnxconverter_common.float16`(或 `float16.convert_float_to_float16`)转 fp16。**禁止从 int8 反推 fp16**(精度已丢,劣化)。把 fp16 `.onnx` 放进 `models/`。
  2. **代码接线(自主可做)**:引擎对模型格式无所谓(sherpa 加载任意 ONNX)。加 `--asr-model <path>` / GUI 选项让用户指定模型;在 hetero 里允许 **GPU 半边用 fp16 模型、CPU 半边继续用 int8**(int8 CPU 最快 + fp16 GPU 最快 = 各取所长)——即两个 recognizer 用**不同模型文件**。`EngineConfig` 加 `asr_model_gpu`(可选,默认 = `asr_model`)。
  3. **benchmark**:同一长语料,测 ① fp16 单 GPU ② int8 单 CPU ③ 异构(int8-CPU + fp16-GPU)。
- **验收**:fp16-GPU 明显快于 int8-GPU;异构(混合精度)> 现在的 5.8×。若 fp16 模型拿不到 → 标 blocked,做后续任务。
- **NEEDS-HUMAN**:模型权重来源(我无法凭空生成);商用许可证。

### P2 —— 【自主,中等收益】VAD 与转写重叠,消除单文件死等
- **假设**:单个长文件时,生产者 `decode → vad.segment(整文件,一次返回所有段) → push all`,所以转写开始前有 ~10-30s 的"解码+VAD"死等(两个消费者全闲)。让 VAD **边切边喂**,消费者能早 10-30s 开工。
- **做法**:把 `Vad::segment` 改成**流式/分块**:VAD 处理一段窗口产出 segment 就立即 `push` 到队列(不等整文件),消费者随即开始转写。`batch_engine.cpp` 生产者循环相应改造。**保持取消可中断**(R6 的 seg_pending 计数不能破)。
- **验收**:单文件端到端 wall 下降(死等被转写重叠掉);多文件无回退;并发正确性测试(double-processing/cancel)全绿。
- **风险**:别引入新 race;改 `Vad` 接口要兼容 `suji_cli` 单文件路径。

### P3 —— 【自主,GPU 专项】gpu_batch 扫描 + ORT IO-binding / CUDA Graph
- **做法**:
  - **扫 `gpu_batch ∈ {8,16,24,32,48}`**(perf 上轮只扫了 cpu_threads,没扫 gpu_batch)在长文件上测 GPU 半边吞吐,找最优,更新 `fill_hetero` 的 `gpu_batch` 公式。
  - **IO-binding**:int8/fp16 GPU 路径上,ORT 的 host↔device 拷贝是开销大头。查 sherpa-onnx 是否暴露 IO-binding;若能,绑定输入/输出到 device 减少 D2H/H2D。
  - **CUDA Graph / cudnn benchmark**:若 sherpa 的 CUDA provider 选项可开 `cudnn_conv_algo_search`/CUDA graph,实测是否提升。
- **验收**:GPU 半边吞吐提升且无正确性问题;否则记录"已试无效"。

### P4 —— 【自主,小-中】ORT session 优化
- 在 `asr.cpp` 给 recognizer 配 ① graph optimization level = ALL ② 合理的 arena / mem pattern ③ 确认 `intra_op_num_threads` 真生效(H0 已验证 per-session 被尊重)。实测每项前后。

### P5 —— 【自主,小】长度分桶减少 batch padding 浪费(原 G4)
- VAD 段长度不一,同一 batch 内 padding 到最长 → 浪费。把相近长度的段分到同一 batch(轻量排序/分桶)再喂 `DecodeMultipleOfflineStreams`。注意:跨文件时间戳路由仍按 file_id,分桶不能打乱 token 归属。实测 padding 浪费下降→吞吐提升。

### P6 —— 【自主,中】单文件生产者并行(可选,P2 之后评估)
- 若 P2 后单文件仍受限于单生产者的 decode/VAD 速度:把单个长文件按时间**切块**分给多个生产者并行 decode+VAD(各块独立解码片段),保持段的全局时间戳正确。较复杂,P2 若已够好可跳过。

### P7 —— 【NEEDS-HUMAN 硬件】3070 Ti 上重调 + 全测
- 在部署机跑 `--provider cpu|cuda|hetero` 三测 + `gpu_batch`/`cpu_threads` 扫描,把 `fill_hetero` 的常量按该机 benchmark 调到最优(`--cpu-threads`/`--gpu-batch` 已可不重编译调)。Ampere + fp16 预期是另一个台阶。

## 4. 测量纪律(硬性)
- **固定语料**:一个长文件(≥10min,如 `build/bench10.wav`)+ 一个多文件集(6×60s,`build/bench_multi/`),每个改动都跑这两组前后对比。
- **指标**:`done:` 行的 `throughput=Nx`(整文件时长 / wall,已是 G13 的口径)。
- **不回退闸门**:任何改动若使长文件或多文件吞吐**下降**,回滚。提升才合入。
- **正确性优先**:并发改动(P2/P6)后,double-processing / cancel / R3 / R6 测试必须全绿;输出段数与基线一致(异构非确定性见 BENCHMARK,只比段数/语义不比 bit-exact)。

## 5. 执行纪律
- 分支 + 本地提交;**绝不 push**。TDD + subagent-driven(implementer+reviewer,ledger 在 `.superpowers/sdd/progress.md`)。先读真实头文件(`c-api.h`、ORT API),禁止凭记忆。每改动 build + 全测试通过 + **展示真实 benchmark 数字**;禁止虚报。不死循环(2-3 次不行标 blocked 转下一个)。维护 PROGRESS.md + 决策日志。**程序因机自适应,绝不在 dev 机写死**(常量调优要么基于探测、要么留 `--flag` 覆盖)。

## 6. NEEDS-HUMAN 汇总
- **fp16 模型权重来源**(P1 的前置,我无法生成模型);商用许可证核实。
- **3070 Ti 硬件**(P7,以及在该机复测 P1-P5 的真实收益)。
- push。

> 开始:先 P1 的**代码接线 + 转换尝试**(若能拿到/转出 fp16 就接着测,否则标 blocked);并行可做 P2(VAD 重叠)和 P3(gpu_batch 扫描)——这两个不依赖 fp16,是自主能拿到的确定收益。
