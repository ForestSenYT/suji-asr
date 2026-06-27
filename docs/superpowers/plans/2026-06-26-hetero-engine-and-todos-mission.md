# MISSION PROMPT — suji-asr 异构 CPU+GPU 并发引擎 + 收尾 TODO（2026-06-26）

> 这是一份交给**自主编码 agent 端到端执行**的任务书。你（agent）没有其它上下文，本文档即全部上下文。请严格按"执行纪律"工作，按"任务清单"顺序推进，按"验证与交付"产出。所有路径均为绝对路径。

---

## 1. 背景与现状

### 1.1 项目是什么
suji-asr 是一个 **Windows 原生 C++ 批量中文讲座转写流水线**。核心链路：

```
ffmpeg 解码 -> Silero VAD 切分 -> FireRedASR2-CTC int8 (sherpa-onnx) 识别 -> CT 标点 -> 输出 SRT/VTT/JSON/MD
```

三个可执行程序：
- `suji_cli`（单文件 CLI，走 `pipeline.cpp` 的 `transcribe_file`）
- `suji_batch`（批量 CLI，走 `batch_engine.cpp` 的 `transcribe_batch_files`）
- `suji_gui`（Qt GUI，`engine_worker.cpp` 调用同一个批量引擎）

### 1.2 现状
- Phase 1-6 已完成并本地合并到 `main`（本地 `main` 领先 `origin/main` 约 55 个提交，**尚未 push**）。
- 当前在 `main` 分支(运行期自适应 GPU/CPU 修正 D10 已合入)。开始本次工作请从 `main` 切一个新分支。
- 运行期已能**自适应**选 GPU/CPU：`decide()`（`src/core/hardware.cpp`）+ `paths::cuda_dll_dir()`，崩溃安全（CUDA DLL 不存在则回退 CPU）。
- 已验证 benchmark（dev 机 RTX 2080 + Ryzen 5800X）：**CPU 4.95x 实时、GPU(2080) 2.85x 实时**。int8 模型对 ORT-CUDA EP 不友好，所以 dev 机上 CPU 反而更快。
- 安装包 all-in-one（含 CUDA/cuDNN redist + Qt 运行时 + ffmpeg.exe + 模型）**已构建并实测**：`build\installer\suji-asr-setup-0.5.exe`（**1.75GB**）。静默装到干净目录 `F:\suji-install-test`（3.51GB/63 文件,清单全对)后,从安装目录零环境变量跑通 `suji_batch`/`suji_gui`——自动 `provider=cuda`、转写正常。仅剩「真·干净机 / 3070 Ti 实测」属 NEEDS-HUMAN。

### 1.3 硬件
- **Dev 机**：RTX 2080（Turing，8GB VRAM，典型可用约 6GB）+ Ryzen 5800X（16 逻辑线程）+ 40GB RAM。
- **部署机**：RTX 3070 Ti（Ampere）。
- 程序必须**因机自适应**，**绝不在 dev 机上写死任何数值**。

### 1.4 已实读并验证的 sherpa-onnx C API 事实（务必复用，禁止凭记忆改）
头文件：`F:\Git\suji-asr\vendor\sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda\include\sherpa-onnx\c-api\c-api.h`

- 批量解码：`SherpaOnnxDecodeMultipleOfflineStreams(recognizer, streams**, n)`。
- VAD 配置字段名是 `max_speech_duration`（**无** `_s` 后缀）。
- `SpeechSegment.start` 的单位是**样本数**（不是秒）。
- 标点 API：`SherpaOfflinePunctuationAddPunct` / `SherpaOfflinePunctuationFreeText`（**无** `Onnx` 字样）。
- ASR 创建/解码/销毁（每 batch 每个 recognizer 的精确序列）：
  1. `SherpaOnnxCreateOfflineStream(rec_)` —— 每个 segment 一个 stream。
  2. `SherpaOnnxAcceptWaveformOffline(st, 16000, samples, n)` —— 每个 stream **至多调用一次**（头文件 @warning）。
  3. `SherpaOnnxDecodeMultipleOfflineStreams(rec_, streams.data(), (int)streams.size())` —— 解整个 batch。
  4. 每个 stream：`SherpaOnnxGetOfflineStreamResult(st)` 读 `r->text / r->tokens_arr / r->timestamps / r->count`，再 `SherpaOnnxDestroyOfflineRecognizerResult(r)`。
  5. 每个 stream：`SherpaOnnxDestroyOfflineStream(st)`。
  6. teardown：`SherpaOnnxDestroyOfflineRecognizer(rec_)`。
- FireRedASR-CTC 配置字段：`c.model_config.fire_red_asr_ctc.model`（`asr.cpp` 已正确使用，无需改）。
- provider 是**每 recognizer 的字段**（`model_config.provider` = `"cpu"` 或 `"cuda"`）。每个 `Create*` 返回独立 opaque handle，CPU/CUDA 两个 recognizer 不共享可变全局状态。

> **关键约束（贯穿全设计）**：头文件**没有任何声明**保证同一个 recognizer handle 可被多线程并发调用。本设计的安全契约是：**每个 recognizer handle 只被恰好一个线程触碰**。两个消费者线程各自拥有独立 handle（一个 CPU、一个 CUDA），不存在共享 handle 的并发危险。**这一点必须先用测试经验性验证（见任务 H0），不得仅凭推断就开工。**

---

## 2. 本次目标：异构 CPU+GPU 并发引擎（重中之重）

**核心目标：让 CPU recognizer 与 GPU recognizer 在同一进程内并行运行，聚合吞吐 ≈ 两者之和（扣除竞争开销）。**

设计形态（pull-based work-stealing，无调度器）：

```
P 个生产者 (解码+VAD)         ← P = tune.in_flight_files
       │  push SegTask{file_id,start_sample,samples}
       ▼
  BoundedQueue<SegTask>   (cap 随 in_flight_files 与 batch 同时放大)   ← 共享队列，类不改
      ╱                    ╲
     ▼                      ▼
 CPU 消费者              GPU 消费者
 owns Asr(Cpu,            owns Asr(Cuda,
   num_threads=K)          num_threads=1)
 batch 到 cpu_batch      batch 到 gpu_batch
     ╲                      ╱
      ╲  各自按 file_id 路由 token
       ▼
  tok_cpu[N] / tok_gpu[N]  ← 两个消费者各写各的分片（无锁）
       ▼
  join 两个消费者 → 逐文件 finalize（合并两分片 → 排序 → merge → 标点）
```

**负载自然均衡**：两个消费者从同一个 `BoundedQueue` `pop()`，先做完当前 batch 的消费者先取下一个 segment，更快的引擎自然抢到更多活。一个 segment 只被一次 `pop()` 取走，保证不重复处理。

**这是最大的卖点，必须精确实现，并把风险评审里的每一条缓解措施都内建进去（见任务 H1-H9 与风险表）。**

---

## 3. 任务清单

> 顺序：异构引擎子任务（H0-H9）在前；其余收尾 TODO（T1-T18 + 规格缺口 G）在后。每个任务含 **目标 / 做法 / 验收标准 / 测试**，并标 **【自主可做】** 或 **【NEEDS-HUMAN】**。
>
> 每个任务用 subagent-driven-development：implementer + reviewer，ledger 写在 `.superpowers/sdd/progress.md`。TDD：先写测试再实现。

---

### === 异构引擎子任务（先做，按 H0→H9 顺序）===

#### H0 —— 【自主可做，GATING 前置】两 recognizer 并发可行性 smoke 测试
> 这是整个异构设计的地基。**在写引擎之前必须先过这一关。失败则设计作废，标 blocked 上报。**

- **目标**：经验性证明「同一进程内创建一个 CPU recognizer + 一个 CUDA recognizer，从两个线程各自驱动，多轮迭代不崩溃 / 不卡死 / 不返回错误数量 / 不数据损坏」。把所有文档里「confirmed feasible」的措辞改成「validated by test H0」。
- **做法**：
  - 写一个独立测试/小程序（doctest case 或 `tests/` 下小工具），用**真实模型**：CPU `Asr`(`provider=Cpu`) 与 CUDA `Asr`(`provider=Cuda`, `cuda_dll_dir` 由 `paths::cuda_dll_dir()` 解析) 各起一个线程，各自用真实音频段循环 `transcribe_batch` N≥50 轮。
  - 断言：无 crash / 无 hang（带超时）/ 每轮 `res.size()==batch.size()` / 文本非空且每轮一致性合理。
  - 同时验证 **R7**：两个 live recognizer 时 `cpu_asr_threads` 是否真被尊重（ORT 可能用进程全局 intra-op 池）。开 `debug=1`，观察 ORT 日志 / 测量 CPU 线程数，确认 CPU 池大小 = 设定值、CUDA 池 = 1。若 ORT 用全局池导致设定不生效，需显式经 SessionOptions/env 设置 intra-op 线程数并记录。
  - 同时验证 **R8**：开 ORT profiling/`debug=1`，确认 int8 算子在 CUDA 上的覆盖率；若大量算子回退到 CPU EP（吞噬未计入 split 的 CPU 线程），记录实测，并在 §3 split 公式里相应调大 `gpu_feed`，重跑 R5 闸门。
  - 确认 `SetDefaultDllDirectories`/`AddDllDirectory`（`asr.cpp` 23-26）在两 handle 场景下被恰好调用一次、顺序确定（先建 CPU `Asr` 再建 CUDA `Asr`）。
- **验收标准**：测试在 dev 机（2080+5800X）稳定通过 N≥50 轮无异常；产出一份实测记录（CPU 池实际线程数、CUDA 算子覆盖率、是否有 EP 回退）写入 PROGRESS.md。
- **测试**：上述并发 smoke 即测试本体。**展示真实输出**（终端日志 + ORT debug 摘要）。
- **失败处理**：若崩溃/卡死/数量错乱 → 立即标 `blocked`，写清现象与 ORT 日志，停止 H1-H9，转去做收尾 TODO（T/G 段），等人工裁决。

---

#### H1 —— 【自主可做】扩展 Provider 枚举与 AutoTune 字段
- **目标**：新增 `Provider::Hetero`（仅作引擎模式选择器，**绝不**传给 `model_config.provider`）；`AutoTune` 增加 per-engine 字段。
- **做法**：
  - `src/core/config.h`：
    ```cpp
    enum class Provider { Cpu, Cuda, Hetero };
    inline const char* provider_str(Provider p){
      switch(p){ case Provider::Cuda: return "cuda";
                 case Provider::Hetero: return "hetero";
                 default: return "cpu"; }
    }
    ```
    `provider_str` 永远不会把 `Hetero` 交给 sherpa-onnx；引擎内部把 Hetero 映射成 {Cpu recognizer, Cuda recognizer}。
  - `src/core/hardware.h`：`AutoTune` 增加
    ```cpp
    bool  hetero = false;
    int   cpu_batch = 1;
    int   gpu_batch = 8;
    int   cpu_asr_threads = 4;
    ```
- **验收标准**：编译通过；`provider_str(Hetero)=="hetero"`。
- **测试**：doctest 断言 `provider_str` 三种映射；断言 Hetero 从不被传入 `Asr`（由 H3 的 `fill_hetero` helper 构造出 Cpu+Cuda 两个 `EngineConfig` 来验证）。

---

#### H2 —— 【自主可做，含 R1 关键 bug 修复】decide() 加 hetero 分支 + 修复 in_flight 覆写顺序 bug
- **目标**：`decide()` 在「有可用 GPU 且 CPU 核足够」时选 Hetero，并按 §3 split 公式分配线程；**修复 R1**：现状 `hardware.cpp:63` 在分支之后无条件覆写 `in_flight_files`，会破坏「不超订」不变式。
- **R1 证据（已实读 `hardware.cpp:62-63` 确认）**：现状结尾 `t.in_flight_files = std::max(2, std::min(8, by_ram))` 在 hetero 分支算完 `cpu_asr_threads = C - in_flight_files - 1` 之后运行。dev 机 40GB → `by_ram=20→clamp 8`，`in_flight_files` 从 4 跳到 8，但 `cpu_asr_threads` 仍是 11，于是 `8+1+11=20 > 16` 逻辑核 → **超订**，恰好在目标机触发。
- **做法**：
  - **先把 `in_flight_files` 算到最终值，再推导 `cpu_asr_threads`**；并把 hetero 分支**排除在结尾 RAM clamp 之外**（hetero 分支内部自己定 `in_flight_files`，不再被覆写）。
  - §3 split 公式（`C = hw.cpu_threads`）：
    ```
    P (producers)   = clamp(C/4, 2, 6)        // in_flight_files
    gpu_feed        = 1
    cpu_asr_threads = max(2, C - P - gpu_feed)
    cpu_batch       = clamp(cpu_asr_threads/2, 2, 6)
    gpu_batch       = clamp((gpu_free_mb-1500)/150, 8, 32)
    ```
  - 闸门：`gpu_ok = has_cuda_gpu && gpu_free_mb>=3000 && cuda_runtime_available`；`cpu_ok = cpu_threads >= 12`。仅当 `gpu_ok && cpu_ok` 选 Hetero；否则沿用现有 cuda-only / cpu-only 逻辑。
  - **R5 守护**：hetero 仅在 GPU 吞吐是 CPU 的「有意义的比例」时才值得（2080+int8 上 GPU 是更慢的引擎）。若已知/可探测 GPU 吞吐显著低于 CPU，则 `decide()` 偏向 CPU-only。最小实现：在 `decide()` 里加注释化阈值钩子；真正的取舍由验收闸门「hetero 必须 ≥ 1.15×max(cpu,gpu) 且 ≥ cpu_only，否则发 CPU-only」把守（见 §6）。
  - 加硬后置条件 `assert(in_flight_files + gpu_feed + cpu_asr_threads <= C)`。
  - 把 `t.num_threads` / `t.batch` 作为 legacy 镜像填上（hetero 路径不用，但保持字段一致）。
- **验收标准**：编译通过；目标机 `C=16, 40GB` 下 `in_flight_files==4, cpu_asr_threads==11, gpu_batch∈[8,32]`，且 `4+1+11==16<=16` 不超订。
- **测试（必须是 decide() 级别，不是公式级别——R1 只有整链测试才抓得到）**：
  - 表驱动 `decide()`：cores×RAM 矩阵 `C∈{12,16,24,32} × ram_free_mb∈{8000,40000,128000}`，断言 `in_flight_files + gpu_feed + cpu_asr_threads <= C` 且 `cpu_asr_threads>=2, cpu_batch>=2, gpu_batch>=8`。**必须包含 `C=16, ram=40000` 这一格（即原 8+1+11=20 的 bug 场景）**。
  - 选择门：GPU present+16 核 → Hetero；GPU present+8 核 → Cuda；无 GPU+16 核 → Cpu；`gpu_free_mb<3000` → Cpu。

---

#### H3 —— 【自主可做】batch_engine 拆分 + 异构路径主体
- **目标**：保持公开 `transcribe_batch_files(...)` 签名不变（CLI/GUI 调用点零改动），内部按 `tune.provider` 分派；新增 `transcribe_batch_files_hetero`。
- **做法**：
  - 把现有 `transcribe_batch_files` 主体重命名为 file-static `transcribe_batch_files_single`。
  - 公开入口：
    ```cpp
    std::vector<FileResult> transcribe_batch_files(...){
      if (tune.provider == Provider::Hetero)
        return transcribe_batch_files_hetero(inputs, cfg, tune, cb, cancel);
      return transcribe_batch_files_single(inputs, cfg, tune, cb, cancel);
    }
    ```
  - 新增 `transcribe_batch_files_hetero`（要点）：
    - 建两个 recognizer：`cpu_cfg.provider=Cpu; cpu_cfg.num_threads=tune.cpu_asr_threads`；`gpu_cfg.provider=Cuda; gpu_cfg.num_threads=1; gpu_cfg.cuda_dll_dir=cfg.cuda_dll_dir`。
    - **优雅降级**：`cpu_ok=cpu_asr.ok()`，`gpu_ok=gpu_asr.ok()`；两者皆 false → 全文件标失败返回；只 CPU 可用 → 仅起 CPU 消费者；只 GPU 可用 → 仅起 GPU 消费者。
    - 队列容量 **按 R4 修正**：`cap = max(4, (cpu_batch+gpu_batch)*4)` 之外，再让容量随 `in_flight_files` 放大，保证队列保持饱和（例如 `cap = max(4, (cpu_batch+gpu_batch)*4, in_flight_files*8)`）。
    - token sink 分片：`std::vector<std::vector<Token>> tok_cpu(N), tok_gpu(N)`，各消费者只写自己的分片（热路径无锁），finalize 合并。
    - 生产者：与现状一致（`next_file.fetch_add` 取文件 → `decode_to_pcm` → `Vad vad(cfg)` → `vad.segment` → push）。
    - 一个可复用的 consumer lambda，参数化 recognizer/batch_max/sink：`pop` 一个 + `try_pop` 凑到 batch_max → 构造 `Asr::SegView` → `asr.transcribe_batch(views)` → 按 `file_id` 路由 token（`tk.start = start_sample/16000.0 + timestamps[k]`）→ `samples_done += ...`（atomic）。
    - join 顺序：先 join 生产者 → `queue.close()` → join 两个消费者。
    - finalize：逐文件合并 `tok_cpu[i]+tok_gpu[i]` → `std::sort` by start → `merge_tokens` → 逐段标点 → 进度回调。
  - **R3 修复（在此任务一并做，因为异构在 VRAM 压力下加倍暴露此 bug）**：现状 `asr.cpp:67-71` 当 `SherpaOnnxCreateOfflineStream` 返回 NULL 时，销毁部分 stream 后返回**已按 `segs.size()` 定长**的全空 `out`，于是 `res.size()==batch.size()`，消费者的 `if (res.size()!=batch.size()) continue;` **不触发**，整批被当成「成功的空结果」丢掉，文件却 `ok==true` → 静默丢字。修复：让 `transcribe_batch` 把「解出来就是空」与「stream 创建失败」区分开（例如返回更少结果、或加 per-seg error flag）；GPU 消费者检测到「期望非空却为空且疑似 VRAM 压力」时，要么把失败 segment 退回 CPU 引擎重试，要么把所属文件标 `ok=false`。绝不让 NULL-stream 批次被记成成功空输出。
  - **R6 修复**：cancel 语义明确化。生产者跟踪 per-file「production complete」标志（该文件所有 VAD segment 都 push 完才置位）。finalize 时：被 cancel 且文件「未完成生产」且收到过 segment 的，必须标 `ok=false, err="cancelled"`，绝不把截断的转写当成 ok。
- **验收标准**：编译通过；全测试通过；在 3-5 个真实短片（中英混）上 hetero 跑通，所有输出文件写出，段数>0，无 crash，VRAM 全程 <8GB（看 `nvidia-smi`）。
- **测试**：
  - 队列不重复处理（纯队列、无模型）：push K 个唯一 task，两消费者线程经 `pop`/`try_pop` 各自收进 set，断言 union==输入、交集==∅、总数==K。
  - token 合并/排序：构造交错时间戳的 `tok_cpu[i]+tok_gpu[i]`，断言排序后单调、`merge_tokens` 输出与单数组基线一致。
  - cancel 安全：阻塞生产者的桩 + 触发 cancel，断言两消费者线程在超时内 join 无 hang，且至少在飞 batch 完成；断言「收到过部分 segment 但未完成生产」的文件被标 `ok=false,err="cancelled"`，**没有截断转写被静默报成 ok**。
  - 优雅降级：强制 `gpu_asr.ok()==false`（坏 cuda_dll_dir）→ 引擎仍经 CPU-only 消费者跑完，所有文件 `ok`。
  - VRAM 压力下无静默丢字（R3）：构造/模拟 stream 创建失败，断言不会产生「ok 但缺 token」的文件。

---

#### H4 —— 【自主可做】CLI 暴露 hetero（suji_batch）
- **目标**：`--provider` 接受 `hetero`；强制 hetero 但硬件不支持时回退并日志；新增 `--cpu-batch`/`--gpu-batch`。
- **做法**（`src/cli/batch_main.cpp`）：
  - provider 解析加 `hetero` 分支；usage 串改 `[--provider auto|cpu|cuda|hetero]`。
  - provider 解析后立即加回退（镜像现有 CUDA 回退 lines 116-123）：
    ```cpp
    if (tune.provider == Provider::Hetero){
      bool gpu_ok = hw.has_cuda_gpu && hw.gpu_free_mb>=3000 && hw.cuda_runtime_available;
      if (!gpu_ok || hw.cpu_threads < 12){
        log_err("hetero unavailable (need CUDA GPU + >=12 cores), falling back");
        tune.provider = hw.has_cuda_gpu ? Provider::Cuda : Provider::Cpu;
      } else {
        // 用户强制 hetero：调用一个小 fill_hetero(tune,hw) 复用 decide() 的 hetero 分支公式填 tunables
      }
    }
    ```
  - 强制 hetero 被接受时，按现有 CUDA 分支方式解析 `c.cuda_dll_dir`（GPU 半边需要）。
  - 新增 `--cpu-batch N` / `--gpu-batch N`；保留 `--batch` 作为 `--gpu-batch` 的别名（向后兼容）。
- **验收标准**：`suji_batch --provider hetero` 在 dev 机正确进入异构路径；在 8 核机上自动回退并打日志。
- **测试**：CLI 参数解析单测（hetero 映射、回退分支、`--cpu-batch/--gpu-batch` 解析、`--batch` 别名）。

---

#### H5 —— 【自主可做】GUI 暴露 hetero（suji_gui）
- **目标**：combo 加 `"hetero"`；worker 映射 + 回退 + `cuda_dll_dir` 解析。
- **做法**：
  - `src/gui/main_window.cpp`（约 139-141 行）combo 加 `m_provider->addItem(QStringLiteral("hetero"));`。
  - `src/gui/engine_worker.cpp`（约 68-76 行）provider 映射加 hetero 分支，并对 Hetero/Cuda 都解析 `c.cuda_dll_dir`，不支持时回退 CPU。
  - `started(provider,total)` 信号已带 provider 串，`provider_str(Hetero)=="hetero"`，状态行自然显示 "HETERO"，无需额外改动。
- **验收标准**：headless 自检（`--selftest`）通过；combo 含 hetero。
- **测试**：headless `--selftest` 断言 ok=1；GUI 视觉确认归入 NEEDS-HUMAN。

---

#### H6 —— 【自主可做】suji_cli 单文件路径（可选）
- **目标**：单文件路径（`transcribe_file`，不走批量引擎）接受 `hetero` 但静默映射到最佳单 provider（单短文件段太少，异构无收益）。
- **做法**：`src/cli/main.cpp` 接受 `hetero`，映射为 best single provider。
- **验收标准**：`suji_cli ... --provider hetero` 不报错，按单 provider 跑。
- **测试**：参数解析单测。

---

#### H7 —— 【自主可做】非确定性文档化
- **目标**：明确记录 hetero 的 run-to-run 非确定性。
- **做法**：在 README/PROGRESS 写明：hetero 因（a）segment 引擎归属随时序变化 +（b）CPU vs CUDA EP 对同一 int8 模型的数值差异，token 级 run-to-run 非确定（比现有 batch-not-bit-exact 更甚）。需可复现的工作流请用 `--provider cpu`（或 `cuda`）。`--resume` 的 `transcript_complete` 检查不受影响（只查输出存在性，不查内容相等）。
- **验收标准**：README/PROGRESS 含该段。
- **测试**：输出等价性 sanity（任务 H8 #11）。

---

#### H8 —— 【部分 NEEDS-HUMAN】基准与验收闸门（headline 测试）
- **目标**：同一语料三跑 cpu / cuda / hetero，证明异构吞吐可加。
- **做法**：写 benchmark 脚本（`scripts/` 下，PowerShell），固定语料（30-60 分钟总时长、多文件，保证队列饱和）：
  - `suji_batch <corpus> --provider cpu  --no-srt --no-vtt --no-md` 记录 `throughput=Xx`（`batch_main.cpp` 已打印）。
  - 同上 `--provider cuda`、`--provider hetero`。
  - 三跑间清 out_dir 或 `--no-resume`，确保全量处理。
  - pin `--in-flight/--cpu-batch/--gpu-batch` 到 auto 值并记录；另扫 `gpu_batch∈{8,16,30} × cpu_batch∈{2,4,6}`。
  - 记录 split 比（`tok_cpu` vs `tok_gpu` 段/词计数），确认更快引擎拿更多活（预期 CPU≈60-65% / GPU≈35-40%，跟 4.95:2.85 速比一致）。
  - **R4 守护**：若 GPU 把队列「吸干」或 hetero < `max(cpu,gpu)`（超订征兆），fail 并回查 §3 split / 队列容量 / 生产者数。必要时给 GPU 的机会性 `try_pop` 抽取加帽。
  - **oversubscription 守护测试（#10）**：用故意坏 split（`cpu_asr_threads=C`）跑，确认比公式 split **更慢**，验证 §3 公式有效、保护选定值不回归。
  - **输出等价 sanity（#11，非 bit-exact）**：hetero vs cpu-only 同一文件，断言 token 级 CER/WER 低于小阈值（语义接近，非逐字节相等），把 §6 非确定性编码成「预期且有界的差异」而非 bug。
- **验收标准（自主可做部分）**：脚本在 dev 机跑出三组吞吐 + split 比 + 扫描曲线，数据写入 `BENCHMARK.md`。**通过条件**：`hetero >= 1.15 × max(cpu,gpu)` 且 `>= cpu_only`；理想接近 `cpu+gpu` 减竞争（CPU4.95+GPU2.85 加性上限 ~7.8x，现实目标 ~6.5-7.2x）。**若 hetero 在 dev 机（2080+int8，GPU 本就是更慢引擎）打不过 CPU-only，则结论是「dev 机发 CPU-only」，记录之，不算失败。**
- **测试**：上述基准即测试。**展示真实终端输出**。
- **【NEEDS-HUMAN】部分**：在 **3070 Ti（Ampere）部署机**上跑同一三跑基准（Ampere int8 tensor core 更好，可能逆转 dev 机结论）。这一步需目标硬件，归入 NEEDS-HUMAN 清单。

---

#### H9 —— 【自主可做】异构可观测性（最小）
- **目标**：把 split 比落到日志/状态，方便后续调优。
- **做法**：batch 结束打印 `CPU xx% / GPU yy%`（用 `tok_cpu`/`tok_gpu` 计数）。GUI 实时显示留作未来增强（标注即可，不在本次范围）。
- **验收标准**：CLI 末尾打印 split 比。
- **测试**：断言计数器在 H3 的合并测试里被正确累加。

---

### === 风险表（H1-H9 必须逐条内建缓解，验收时逐条核对）===

| # | 风险 | 级 | 缓解（必须落地） | 归属任务 |
|---|------|----|------------------|----------|
| R1 | `decide()` 在分支后覆写 `in_flight_files`，破坏不超订不变式（目标机 8+1+11=20>16） | 高 | 先定最终 `in_flight_files` 再算 `cpu_asr_threads`；hetero 分支排除结尾 RAM clamp；加 `assert(P+1+cpu_asr<=C)`；**decide()-级**矩阵测试覆盖 `C=16,ram=40000` | H2 |
| R2 | 「两 recognizer 并发 confirmed feasible」未被头文件证实（唯一 thread-safe 提及是 VAD 那行） | 高 | 措辞降级为「由测试 H0 验证」；H0 作为 **gating 前置**，失败则设计作废 | H0 |
| R3 | `transcribe_batch` 在 stream 创建失败时静默丢整批（`res.size()==batch.size()` 使 `continue` 不触发），GPU 近 VRAM 上限加倍暴露 | 高 | 区分「解出空」vs「stream 创建失败」；GPU 失败 segment 退 CPU 重试或标文件 `ok=false`；加 VRAM 压力测试断言无静默丢字 | H3 |
| R4 | 队列饱和无保障；`gpu_batch=30` 的机会性 `try_pop` 可能「吸干」队列饿死 CPU | 中 | 队列容量随 `in_flight_files` 放大；基准记录实测 split，GPU 吸干或 hetero<max 即 fail；必要时给 GPU `try_pop` 抽取加帽 | H3,H8 |
| R5 | hetero 可能比 CPU-only 更慢（2080 上 GPU 更慢 + 抢核 + CPU 从 16 降到 11 线程） | 中→高 | `decide()` 仅在 GPU 吞吐是 CPU 有意义比例时选 hetero；验收闸门 `hetero>=1.15×max 且 >=cpu_only`，否则发 CPU-only；2080+int8 明确标记为边际情形 | H2,H8 |
| R6 | cancel 丢在飞 segment；双分片「cancelled」判定不可靠（部分转写被报成 ok） | 中 | 跟踪 per-file「production complete」；收到过 segment 但未完成生产的 cancelled 文件标 `ok=false,err="cancelled"`；测试断言无截断转写被静默报 ok | H3 |
| R7 | 两 recognizer 共享进程全局 ORT 线程池 / DLL 搜索状态，`cpu_asr_threads` 可能不被尊重 | 中 | H0 测量两 live recognizer 下 CPU 池实际线程数；若全局池则显式经 SessionOptions/env 设置；`AddDllDirectory` 恰好一次、顺序确定 | H0 |
| R8 | int8→CUDA 算子回退吞噬未计入 split 的 CPU 线程；两模型 RAM/CUDA host footprint 未实测 | 低→中 | H0 开 profiling/`debug=1` 确认 CUDA 算子覆盖；显著回退则调大 `gpu_feed` 并重跑 R5；实测 RSS+VRAM 写入基准，不靠假设 | H0,H8 |

---

### === 其余收尾 TODO（异构之后做）===

> 这些是代码审计 + 文档 + 规格缺口去重后的全部剩余项。优先做与正确性/数据安全相关的（T1/T8/T9/T12/T14），其余按精力推进。每项标 effort 与 human 需求。

#### T1 —— 【自主可做】media_decode `_popen` → `CreateProcessW`（`%`/`^`/中文/UTF-8）
- **目标**：消除 cmd.exe shell，修 `%`/`^`/非 ASCII（中文）文件名失败。
- **背景**：`src/core/media_decode.cpp:7-9` 用 `_popen` + cmd.exe，外层双引号只处理空格、不转义 `%`/`^`；`%TEMP%`/`100%完整版.mp4`/`^` 会被 cmd 预展开或误解析。`_popen` 走 ANSI codepage，中文路径在 Windows 上可能直接坏。这是**主用例（中文讲座文件名）的正确性 bug**，也是注入面。
- **做法**：用 `CreateProcessW` + 管道对替换 `_popen`，全程不经 cmd.exe，参数经 `lpApplicationName`+`lpCommandLine`（宽字符）传递，路径用 `PathQuoteSpaces` 风格引用。文件名转 UTF-16。
- **验收标准**：含 `%`、空格、中文的文件名能正确解码；无 cmd.exe 介入。
- **测试**：构造含 `%`/空格/中文名的临时音频（或桩 ffmpeg），断言命令行正确组装、解码成功。
- **注**：文档曾标「留你在场时改」（触核心解码路径）。本次按自主可做执行，但务必充分测试 + reviewer 把关 + 展示真实解码输出。effort M。

#### T2 —— 【自主可做】hardware.cpp `_popen`(nvidia-smi) 同样改 `CreateProcessW`
- **目标**：同 T1 的 shell 注入面（`src/core/hardware.cpp:13-21`）。
- **做法**：复用 T1 的 CreateProcessW 模式。
- **验收/测试**：nvidia-smi 路径含特殊字符时正确捕获。effort S-M。

#### T3 —— 【自主可做】`decoding_method` 可配
- **目标**：`asr.cpp:36` 硬编码 `"greedy_search"`，加 `EngineConfig::decoding_method`（默认 `greedy_search`）并贯穿。
- **测试**：单测断言默认值 + 可覆盖。effort S。

#### T4 —— 【部分 NEEDS-HUMAN】ITN `rule_fsts`/`rule_fars` 自动加载 + GUI/batch 暴露
- **目标**：ITN（"二零二六"→"2026"）当前 GUI/batch 静默关闭；`rule_fars` 字段缺失。
- **做法（自主）**：`EngineConfig` 加 `rule_fars`；`Asr` 设 `c.rule_fars`；`paths.cpp` 加 `.fst`/`.far` 自动发现（类似 `models_dir`）；`batch_main.cpp` 加 `--rule-fsts`/`--rule-fars`；`engine_worker.cpp` 自动加载或加 UI 字段。
- **【NEEDS-HUMAN】**：实际 FST 资产获取——所有试过的 `itn_zh_number.fst`/`itn_zh.fst` URL 均 404，需人工决定用哪个 FST bundle（WeTextProcessing FAR 或确认的 sherpa 资产）。C++ 接线自主可做，资产落地阻塞。effort M。

#### T5 —— 【部分 NEEDS-HUMAN】`decide()` 的 `(void)cfg` override 钩子
- **目标**：`hardware.cpp:64` 丢弃 cfg；让 `EngineConfig` 携带上下界（如 `max_batch`/`min_threads`）约束 auto-tuner。
- **做法**：需先定 override 设计（哪些项可覆盖），再去掉 `(void)cfg` 实现。
- **human**：需一个设计决策（暴露哪些 override）。effort S。

#### T6 —— 【自主可做】段 `end` 用真实结束时间，不用 token `start`
- **目标**：`segment_merge.cpp:15-16` 把段 end 设成最后 token 的 `start`；`Token` 无 end 字段。
- **做法**：从 `r->timestamps`（或 `r->durations` 若 FireRedASR2-CTC 填充）传播 per-token 时长到 `Token::end`，用于 `merge_tokens` 与 SRT/VTT。需先验证模型是否填 `durations`。
- **测试**：断言段 end > 段 start 且合理。effort M。

#### T7 —— 【自主可做，由 T6 解决】SRT/VTT 硬编码 `+2.0s` 回退
- **目标**：`srt_writer.cpp:8`/`vtt_writer.cpp:7` 的 `s.start+2.0` 降级为最后兜底（T6 提供真实 end 后）。effort S。

#### T8 —— 【自主可做】Punctuator 失败静默 → 加日志
- **目标**：`pipeline.cpp:24-25` punct init 失败静默 passthrough，无告警。
- **做法**：`!punct.ok()` 时 `log_err("punct model not loaded: passthrough")`。
- **测试**：桩坏 punct 模型，断言日志出现。effort S。

#### T9 —— 【自主可做】`write_outputs` 返回值在 batch/GUI 被忽略
- **目标**：`batch_main.cpp:167`/`engine_worker.cpp:142` 丢弃 `write_outputs` 返回值，磁盘满/权限错被静默吞，文件仍计为完成。
- **做法**：检查返回值，`log_err` + 增 failedCount。
- **测试**：桩写失败，断言被计为失败。effort S。

#### T10 —— 【自主可做】`--batch`/`--in-flight` 用 `atoi` 无校验
- **目标**：`batch_main.cpp:61-62` `atoi` 解析失败返 0，`--batch abc` 静默置 0（可致 `BoundedQueue` 异常）。
- **做法**：换 `std::stoi`+try/catch 或校验 `>0` 再应用。
- **测试**：非法参数断言报错/拒绝。effort S。

#### T11 —— 【自主可做】Vad 每文件重建 → 每生产者线程一次
- **目标**：`batch_engine.cpp:49` 每文件 `Vad vad(cfg)` 重载 ONNX 模型，并发多文件 = 多次模型加载。
- **做法**：生产者循环外每线程初始化一次 Vad（Vad 当前 per-segment 顺序使用，每线程一个安全）。**注意**：本改动 single 路径与 hetero 路径都要做。
- **测试**：断言每线程仅一次 Vad 构造。effort M。

#### T12 —— 【自主可做】batch 丢 token 不记日志
- **目标**：`batch_engine.cpp:67` `if (res.size()!=batch.size()) continue;` 静默丢整批 token。
- **做法**：加 `log_err("transcribe_batch returned wrong size; skipping batch of "+...)`。（与 R3/H3 配合。）
- **测试**：桩返回错尺寸，断言日志。effort S。

#### T13 —— 【NEEDS-HUMAN 决策】`app_dir()` 非 Windows 返回 `"."`
- **目标**：`paths.cpp:39-41` `#else` 返回 `"."`，跨平台 relocatable install 失效。
- **human**：需人决定跨平台是否在范围内（当前 Windows-only by design）。在范围内则 Linux 用 `/proc/self/exe`、macOS 用 `_NSGetExecutablePath`。effort M。**默认：标 NEEDS-HUMAN，不动。**

#### T14 —— 【自主可做】cancel 启发式误判静默文件（与 R6 同源）
- **目标**：`batch_engine.cpp:85-87` 仅靠 `file_tokens[i].empty()` 判 cancelled，真·无对白文件会被误标 cancelled；部分处理文件被标 ok+部分转写。
- **做法**：加 per-file `was_started`/`production_complete` 标志，区分「从未开始」「完成前 cancel」「完成但空」。**single 与 hetero 路径都要做**（H3 已为 hetero 路径处理，single 路径在此补齐）。
- **测试**：cancel 各场景断言分类正确。effort M。

#### T15 —— 【部分 NEEDS-HUMAN】batch ASR 测试加内容校验
- **目标**：`tests/integration/test_batch_asr.cpp:17-24` 只验有效性不验正确性。
- **做法**：加 `CHECK(batch[i].text.find("某关键词")!=npos)`。
- **human**：需人确认测试 `.wav` 的预期转写内容。effort S。

#### T16 —— 【自主可做】resume 完整性用脆弱子串启发式
- **目标**：`resume.cpp:29-32` 用 `"\"start\":"`/`"**["`/`"-->"` 子串判完成，截断文件会误判完成被永久跳过。
- **做法**：换轻量真解析（JSON 查 `segments` 非空；SRT 查至少一条 cue），或写入末尾 `"suji_done"` sentinel。
- **测试**：构造截断文件，断言判为未完成。effort M。

#### T17 —— 【自主可做】模型路径硬编码在 3 处、无 CLI/GUI override
- **目标**：`main.cpp`/`batch_main.cpp`/`engine_worker.cpp` 各自拼 `"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/"`，copy-paste 隐患，升级需重编译。
- **做法**：`paths.cpp` 加 `ModelPaths` helper 集中构造；可选加 `--asr-model`/`--vad-model`/`--punct-model` override。
- **测试**：单测断言路径集中构造。effort M。

#### T18 —— 【自主可做】文件夹扫描非递归
- **目标**：GUI `main_window.cpp:437`（`QDir::entryInfoList`）与 `batch_main.cpp:72`（`fs::directory_iterator`）非递归，`课程/第一讲/001.mp4` 拖顶层加不进。
- **做法**：GUI 换 `QDirIterator(..., QDirIterator::Subdirectories)`；CLI 换 `fs::recursive_directory_iterator`。
- **测试**：构造嵌套目录，断言递归收集。effort S。

#### 规格缺口（G 段，去重后；与上面 T 段不重复的）—— 大多【自主可做】
- **G1 ITN `rule_fars`**：见 T4。
- **G2 GPU OOM 自动减半**：`batch_engine.cpp` 无 CUDA OOM 捕获→减半→重试。**自主可做**：捕获 OOM、`tune.batch/gpu_batch` 减半重试。effort M。
- **G3 队尾超时 flush**：消费者贪婪 `try_pop` 无定时器释放欠满 batch。**自主可做**：加 N ms 计时器 flush 欠满 batch。effort M。
- **G4 长度分桶**：段 FIFO 派发，超长段拖累 batch。**自主可做（可选）**。effort M。
- **G5 SRT/VTT 每行 ~15-20 中文字可配**：writer 输出整段无换行。**自主可做**：`EngineConfig::srt_max_chars_per_line` + CLI/GUI 暴露。effort M。
- **G6 单文件/GUI 输出防重名**：`suji_cli`/GUI 静默覆盖（只有 `suji_batch` 有 dedup）。**自主可做**。effort S。
- **G7 GUI resume 跳过**：`engine_worker.cpp` 从不调 `transcript_complete()`，总是重转。**自主可做**。effort S。
- **G8 `cudaMemGetInfo` 进程内查 VRAM**：现用 nvidia-smi 子进程，规格点名直接 API。**自主可做**（链接 cudart，注意崩溃安全 + 找不到时回退 nvidia-smi）。effort M。
- **G9 显示器驱动 VRAM 预留**：固定 1500MB headroom 不分是否驱动显示器。**自主可做**（驱动显示器再留 0.5-1GB）。effort S。
- **G10 标点 provider/线程可配**：`punctuation.cpp` 硬编码 `cpu`/1 线程。**自主可做**：`EngineConfig::punct_provider`/`punct_threads`。effort S。
- **G11 GUI QSettings 持久化**：provider/batch/in-flight/格式 每次重置。**自主可做**。effort M。
- **G12 GUI batch/in-flight override 控件 + ETA 显示**：GUI 无此控件、无 ETA 倒计时。**自主可做**。effort M。
- **G13 throughput 指标准确性**：`audio_seconds_done` 累的是 VAD 段样本非整文件时长；CLI `last_audio` 快照可能滞后。**自主可做**：用整文件解码时长统计。effort S-M。

---

## 4. NEEDS-HUMAN 清单（agent 不得自行做，整理后上报，等人工）

1. **git push**：本地 `main` 领先 `origin/main` ~55 提交；本次新工作也只本地提交。**push = NEEDS-HUMAN，绝不自动 push。**
2. **3070 Ti 上跑异构基准（hetero vs 单 CPU vs 单 GPU）**：dev 机 2080+int8 上 GPU 更慢，hetero 可能边际；Ampere int8 tensor core 更好可能逆转结论。需目标硬件。决定 `auto` 默认是否在 3070 Ti 上保留 GPU/hetero，或一行改 `decide()` 偏 CPU。
3. ~~构建 setup.exe~~ **已完成**（Inno Setup 已装；`build\installer\suji-asr-setup-0.5.exe` 1.75GB 已生成,并在干净安装目录实测 `provider=cuda` 跑通)。如需重建:`powershell -File scripts\build_installer.ps1`。
4. **干净机 / 3070 Ti 安装验证**：验 (a) 模型被找到、转写可用，(b) GPU 自动选中（状态显示 provider=cuda/hetero），(c) 该机吞吐基准。需目标硬件。
5. **GUI 视觉/交互人工确认**：headless 测试已过（`--selftest` ok=1，`--selftest-cancel` cancelled=6，Critical Cancel bug 已修），但布局、拖放、运行中队列表更新、Cancel UX、整体观感未经人确认。
6. **fp16 GPU 模型方向决策**：CPU 已 4.95x 实时；找/转 FireRedASR2-CTC 的 fp16 ONNX ROI 存疑。方向归用户。
7. **ITN FST 资产获取（T4/G1）**：所有试过的 zh-ITN FST URL 均 404，需人定用哪个 bundle（WeTextProcessing FAR 或确认的 sherpa 资产）。C++ 接线已可自主做，资产阻塞。
8. **FireRedASR 权重许可证核实（商用）**：权重来自 ModelScope；代码许可证与权重许可证需分别核实，商用前确认。法务，无代码工作。
9. **跨平台是否在范围内（T13）**：`app_dir()` 非 Windows stub，是否要做 Linux/macOS。
10. **batch ASR 测试预期转写内容（T15）**：需人确认测试 `.wav` 实际说了什么。

---

## 5. 执行纪律（硬规则，不可违反）

1. **分支 + 本地提交；绝不 `git push`**（push = NEEDS-HUMAN）。当前在 `feat/asr-pipeline`；如需新分支从 `main` 切。
2. **TDD + subagent-driven-development**：每个任务 implementer + reviewer 两个 subagent；ledger 写 `.superpowers/sdd/progress.md`。先写测试，再写实现。
3. **先读真实头文件 / 官方 API，禁止凭记忆**；正确性优先于速度；复用官方 API。改 sherpa 相关代码前实读 `c-api.h`。
4. **每个改动必须 build + 全测试通过 + 展示真实输出**；**禁止虚报 done**；**不死循环**——同一处 2-3 次不行就标 `blocked` 转别的任务。
5. **维护 PROGRESS.md 时间线 + 决策日志（从 D11 起编号）**；程序**因机自适应，绝不在 dev 机上写死**任何数值（batch/线程/VRAM 等全从 `decide()` + 探测来）。
6. **复用已验证的 sherpa C API 事实**（§1.4）：`SherpaOnnxDecodeMultipleOfflineStreams(recognizer, streams**, n)`；VAD 字段 `max_speech_duration`（无 `_s`）；`SpeechSegment.start` 单位是样本数；标点 `SherpaOfflinePunctuationAddPunct` / `SherpaOfflinePunctuationFreeText`（无 `Onnx`）。
7. **硬件事实**：dev = RTX 2080(Turing,8GB) + 5800X(16线程) + 40GB；部署 = 3070 Ti。已知 benchmark CPU 4.95x / GPU(2080) 2.85x（int8 对 ORT-CUDA 不友好）。运行期已能自适应选 GPU/CPU（`decide()` + `paths::cuda_dll_dir()`，崩溃安全）；安装包 all-in-one 含 CUDA。
8. **不在 dev 机上写死**：任何「2080 上 CPU 更快」的结论只能体现为自适应阈值/验收闸门，不能硬编码成「永远用 CPU」。
9. **复用确认不变的组件**（无需重写）：`BoundedQueue`（已支持多消费者 mutexed `pop`/`try_pop`/`close`）、`Asr` 及其 `transcribe_batch`（每 handle 单线程）、`Vad`（cpu）、`Punctuator`、`merge_tokens`、`decode_to_pcm`、`SegTask` 结构、公开 `transcribe_batch_files` 签名（CLI/GUI 调用点不改，只改 provider 串管线）。

---

## 6. 验证与交付

### 6.1 每个任务交付
- 代码改动 + 测试（先红后绿）+ 真实 build/test 终端输出。
- `.superpowers/sdd/progress.md` ledger 更新（implementer/reviewer 状态）。
- 触及决策的写入 PROGRESS.md 决策日志（D11 起编号）。

### 6.2 异构引擎整体验收闸门（headline）
- H0 并发 smoke 通过（gating，否则全停）。
- `decide()`-级测试矩阵通过（含 `C=16,ram=40000` 不超订）。
- 队列不重复处理 / token 合并 / cancel 安全 / 优雅降级 / VRAM 压力无丢字 测试全绿。
- **基准数据**（dev 机）写入 `BENCHMARK.md`：cpu / cuda / hetero 三组吞吐 + split 比 + `gpu_batch×cpu_batch` 扫描曲线。通过条件 `hetero >= 1.15×max(cpu,gpu)` 且 `>= cpu_only`；若 2080+int8 上 hetero 打不过 CPU-only，记录「dev 机发 CPU-only」结论（非失败），并把 3070 Ti 基准列入 NEEDS-HUMAN。
- 风险表 R1-R8 逐条核对，缓解全部落地。

### 6.3 交付物
- 更新的 PROGRESS.md（时间线 + D11+ 决策日志，含 H0 实测：CPU 池实际线程数、CUDA 算子覆盖率、EP 回退情况）。
- BENCHMARK.md（异构 vs 单引擎数据 + 扫描曲线 + split 比）。
- README/PROGRESS 含 hetero 非确定性说明（H7）。
- `.superpowers/sdd/progress.md` 完整 ledger。
- **晨间摘要**：本次完成了什么、blocked 在哪、NEEDS-HUMAN 待办（含 3070 Ti 基准、push、安装包、GUI 视觉、ITN FST、fp16 决策、许可证），以及给人的下一步清单。
- **绝不 push**——交付为本地提交 + 上述文档；push 由人工执行。

---

> 开始执行：先 H0（gating）。H0 过则 H1→H9，再 T/G 段（优先 T1/T8/T9/T12/T14 等正确性项）。任一处卡住 2-3 次标 blocked 转下一个。全程展示真实输出，禁止虚报 done。
