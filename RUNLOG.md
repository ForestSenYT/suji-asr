# RUNLOG — 中文讲课批量转写 · C++ 高吞吐流水线

> 全程维护;不确定项标 `[待核实]`。本机为**开发机**,**部署目标为 RTX 3070 Ti**。

## Decisions (locked)
- **开发机**:RTX 2080 (Turing `sm_75`, 8 GB) · Ryzen 7 5800X (8c/16t) · ~40 GB RAM · F: 124 GB 空闲 · Python 3.13.14 · VS2022。
- **部署目标**:RTX 3070 Ti (Ampere `sm_86`, 8 GB) — 用户确认。构建保持可移植;2080 上的 benchmark 视为 provisional,最终数以 3070 Ti 为准。
- **授权**:winget 安装 ffmpeg + cmake;下载 sherpa-onnx GPU 二进制 + 模型至 `F:\Git\suji-asr\models`。沙箱若 403 停下报告,不编造。
- **内存约束放宽**:实机 ~40 GB(非 spec 假设的 16 GB)→ 在飞文件数默认可上调(Phase 2/3 调优时定)。

## Phase 0 环境+冒烟

### 环境探测 (2026-06-25) — 已核实
| 项 | spec 假设 | 实测 |
|---|---|---|
| GPU | RTX 3070 Ti / Ampere sm_86 / 8 GB | **RTX 2080 / Turing sm_75 / 8 GB**(driver 591.86, CUDA 13.1-capable, ~6.4 GB 空闲 VRAM)。Turing 亦有 INT8 张量核 → int8 方案成立。 |
| CPU | i7 | **Ryzen 7 5800X (8c/16t)** |
| RAM | 16 GB（"主瓶颈"） | **~40 GB**（约束放宽） |
| Disk | — | C: 27 GB 空闲(紧)· **F: 124 GB 空闲** → 模型/构建放 F: |
| CUDA toolkit | 12.x | nvcc 不在 PATH;仅残留 `CUDA\v9.0`(无关)→ 依赖 GPU 二进制自带 runtime |
| ffmpeg | 需 | **未装** → curl 下 BtbN win64 **lgpl** portable(bundle-ready) |
| cmake | 需 | **未装** → curl 下 **v4.3.3** portable zip |
| 编译器 | MSVC | **VS2022 已装**(cl 需 dev shell) |
| sherpa-onnx + 模型 | 需 | **无** → 待下载 |

### 工具链核实(后台研究 wf_d360aa15-409)— ✅ 完成(权威结论见 spec §3)
- sherpa-onnx **v1.13.3**;CUDA 资产 `...-cuda-12.x-cudnn-9.x-win-x64-cuda.tar.bz2`(310MB,ORT 1.24.4,**需另装 CUDA12.x+cuDNN9.x** → 打包须带可再发行 DLL)。
- FireRedASR2-CTC tarball 实测 **520,516,278 B(~496 MiB)**,`model.int8.onnx` 740M;URL 已确认。
- ⚠️ 批量 API 真名 **`SherpaOnnxDecodeMultipleOfflineStreams`**(无 `DecodeStreams`)。VAD 字段 `max_speech_duration`(无 `_s`)。
- ITN:产品内用 sherpa 内置 FST(`rule_fsts`);wetext 仅开发期对照。
- ⚠️ **待实测**:CPU RTF、`num_threads=1` 必须性、VAD 默认值、Turing sm_75 实跑、bundle DLL 清单。

### 网络与下载
- ⚠️ **winget 受阻**:WININET `InternetOpenUrl 0x80072efd`(winget 不走代理),ffmpeg/cmake 均失败(exit -2147012867)。
- ✅ **curl / Invoke-WebRequest 经 HTTP 代理可下载**(github HEAD 200)。→ 全部改 **curl 直下 portable**,免 winget、免提权、bundle-ready。
- 下载中(curl,后台 `b53115bq9`+`bw2othpvw`):sherpa CUDA SDK · FireRedASR2-CTC · `silero_vad.onnx` · CT-punct-int8 → `vendor/`(+ `models/`);ffmpeg(lgpl)· cmake v4.3.3 → `vendor/`。下一步:解压、核实 DLL 清单、CUDA 冒烟。

### Bundle 解压 + 冒烟(2026-06-25)
**Bundle 清单**(`vendor/sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda/`):
- ✅ 可链接 C API:`include/sherpa-onnx/c-api/c-api.h` + `lib/sherpa-onnx-c-api.lib` + `lib/sherpa-onnx-c-api.dll`(4.3MB)→ `suji_core` 直接链接(无需源码编译)。
- ORT:`onnxruntime.dll`(14MB)+ **`onnxruntime_providers_cuda.dll`(262.8MB)** + tensorrt/shared providers。
- ⚠️ **不含** `cudart64_12`/`cublas*`/`cudnn*` → CUDA12.x+cuDNN9.x runtime 须自带(打包须 bundle 可再发行 DLL;**本开发机也无 CUDA**)。
- 模型就位:FireRedASR2-CTC `model.int8.onnx` **739.9MB** + tokens;CT-punct int8 72MB;`silero_vad.onnx` 0.6MB(均在 `models/`)。

**CPU 冒烟 ✅**(`sherpa-onnx-offline.exe --provider=cpu --num-threads=4`,FireRedASR2-CTC,test_wavs/0.wav):
- recognizer 1.393s 创建;输出 JSON **text + `timestamps` 非空(字级,24 点 1:1 token)**;config dump 确认 `rule_fsts/rule_fars` 字段存在(ITN 挂载点)、`fire_red_asr_ctc` 选中、`modeling_unit=cjkchar`。
- RTF 0.199(10s clip / 4 线程 5800X)—— 单点数据,真实吞吐 Phase 4 实测。

**GPU 冒烟 ✅**(`--provider=cuda --num-threads=1`,2080 / Turing sm_75):
- 用 `pip download` 取 CUDA **12.9** + cuDNN **9.23** redist(7 wheel ~1.55GB:cublas527 / cudnn658 / cufft191 / curand66 / nvrtc73 / nvjitlink34 / cudart3 MB)→ 解压加 PATH → **`provider=cuda` 生效,exit 0,Turing 实跑通过**,timestamps 非空。
- recognizer 创建 **21.2s**(首次 CUDA/cuDNN init + 740MB 上显存);decode 4.2s。
- ⚠️ **单 10s clip GPU RTF=0.418 反比 CPU 0.199 慢** —— 纯固定开销(CUDA init / 单流 / 小负载)。**GPU 优势在批量 `DecodeMultipleOfflineStreams` + 长音频**(Phase 2);勿用单短 clip 评判 GPU。
- 这 7 个 nvidia 包 = installer 须 bundle 的 CUDA/cuDNN runtime redist。CPU/GPU 输出 token 数微差(int8 数值差,正常)。
- VRAM 基线:留待 Phase 2 用 in-process `cudaMemGetInfo` 精测(批量调参时)。

### Phase 0 结论:✅ 完成 —— CPU+GPU 双路径均在真机跑通,资产/工具链就位,bundle 可链接。下一步进 Phase 1(需 spec 最终确认 + writing-plans)。

### 许可证 — **PENDING**:FireRedASR 代码条款 vs 权重条款(转自 ModelScope)分别核实。

## Phase 1 实现

### ITN 状态 (Phase 1)

**`--rule-fsts` 接线:✅ 完成**
- CLI `--rule-fsts <f>` 参数已连接到 `EngineConfig.rule_fsts`(Task 11)。
- recognizer 读取 `config.rule_fsts` 并在非空时挂载 FST;空值时 ITN 关闭(所有单元/集成测试通过)。
- 接线已验证,无需 C++ 修改。

**候选 zh-ITN FST 来源(均 404 或无标记发布)**:
- `https://github.com/k2-fsa/sherpa-onnx/releases/download/itn-models/itn_zh_number.fst` → 404
- `https://github.com/k2-fsa/sherpa-onnx/releases/download/itn-models/itn_zh.fst` → 404
- `https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/master/scripts/itn/itn_zh_number.fst` → 404
- k2-fsa/sherpa-onnx releases 中无 `itn` 标记发布;FST 资产名/位置待确认。

**决策:Phase 1 保持 ITN 关闭**
- 数字保留口语形式(如"二零二六"不转换为"2026")。
- CT-Transformer 标点仍应用(不受影响)。
- Phase 1 目标是验收流程正确性(VAD、ASR、标点、输出格式),不阻塞 ITN 有无。
- 产品级 ITN 待后续环节(获取/构建正确的 zh-ITN FST,验证内置 FST 质量)。

**开发期对照工具**:
- `scripts/itn_compare.py`:dev-time 脚本(不入产品),读取 stdin(无 ITN 的 ASR 输出),输出 wetext 标准化形式(ITN)。
  ```bash
  pip install wetext
  type build\no_itn\1.md | python scripts/itn_compare.py
  ```
- 用于比对内置 FST ITN vs wetext ITN,判断 FST 质量是否满足业务需求。

**后续**:
1. 从 WeTextProcessing FAR 或确认的资产源获取 zh-ITN FST。
2. 放入 `vendor/itn_zh_number.fst`,启用 `--rule-fsts vendor/itn_zh_number.fst`。
3. 用真实讲课文件(含数字、日期、特殊词)验证内置 FST vs `itn_compare.py`(wetext) 的输出,确认 FST 足以生产。
4. 若 FST 质量不达要求,评估集成 wetext 或采用 sherpa 预构建 FST 的其他来源。

### 输出格式 schema 说明 (Phase 1)

**JSON `tokens[]` vs `text` / `full_text` 语义**

- 每段 `tokens[]` 是 FireRedASR2-CTC 输出的**原始字符级识别单元**,携带逐 token 时间戳(PRE-punctuation)。
- 每段 `text` 及顶层 `full_text` 是经 CT-Transformer 标点模型处理后的**可读字符串**(POST-punctuation)。
- 因此 **`tokens[].t` 拼接 ≠ `text`**:标点符号由标点模型插入于 token 之间,本身不携带时间戳。此设计**有意且正确**:
  - `tokens[]` 服务于时序/对齐用途(字幕打点、跳转定位)。
  - `text` / `full_text` 服务于人类阅读。
- **Phase 2+ 后续**:若需要标点对齐的逐 token 流,届时将标点重新分配到相邻 token;Phase 1 不做此重分配。

(此数据契约细节由最终 whole-branch review 标记为 Important,决议为文档化而非 Phase 1 代码修改。)

## 待核实
1. sherpa-onnx GPU Windows 发布资产名 + 所需 CUDA/cuDNN(研究 pending)。
2. 模型确切下载 URL(GitHub + ModelScope 镜像)。
3. 真实 C API 签名(`c-api.h`)——写代码前必读。
4. Windows 可用 ITN 方案(WeTextProcessing 在 Win/Python 3.13 的可行性)。
