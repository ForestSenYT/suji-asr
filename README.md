# suji-asr — 中文讲课批量转写(C++ / Windows)

把大量**几小时长的中文讲课录像**离线、高吞吐地批量转写成文字文档(SRT / VTT / JSON / Markdown)。基于 **sherpa-onnx + FireRedASR2-CTC int8**,GPU 批量 + CPU 喂数流水线,**因电脑而异自动调参**(CUDA↔CPU)。

> 状态:**Phase 1(单文件)+ Phase 2(批量引擎)已完成**。Phase 3(续跑/ETA)/ 4(benchmark)/ 5(Qt GUI)/ 6(安装包)进行中。设计见 `docs/superpowers/specs/`,各阶段计划见 `docs/superpowers/plans/`,运行日志见 `RUNLOG.md` / `PROGRESS.md`。

## 能力
- **单文件**:`suji_cli` — 一个音/视频 → SRT/VTT/JSON/Markdown(UTF-8 无 BOM,字级时间戳)。
- **批量**:`suji_batch` — 目录/多文件 → 每文件输出 + 聚合吞吐;CPU 多线程喂数 + 单 GPU 消费者批量解码(`DecodeMultipleOfflineStreams`);错误隔离(单文件失败不拖垮整批)。
- **硬件自适应**:启动探测 GPU/VRAM/CPU/RAM,自动选 provider、batch、在飞文件数、线程数。

## 流水线
```
音/视频 → ffmpeg 解码(16k 单声道) → Silero VAD 切段 → FireRedASR2-CTC 识别(字级时间戳)
        → CT-Transformer 标点 → 段落重建 → SRT / VTT / JSON / Markdown
```

## 依赖(Phase 0 已就位于 `vendor/`,均 gitignored)
- **sherpa-onnx v1.13.3**(预编译 CUDA C API):`vendor/sherpa-onnx-v1.13.3-cuda-12.x-cudnn-9.x-win-x64-cuda/`(链接 `lib/sherpa-onnx-c-api.lib`,运行期 `lib/*.dll` 自动拷到 exe 同目录)。
- **模型**(`models/`):`sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/`(ASR,740MB)、`silero_vad.onnx`、`sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/`(标点)。
- **ffmpeg**(LGPL):`vendor/ffmpeg-master-latest-win64-lgpl/bin/ffmpeg.exe`。
- **CMake**:`vendor/cmake-4.3.3-windows-x86_64/bin/cmake.exe`(便携版,不在 PATH)。
- **MSVC**:Visual Studio 2022 + Windows SDK 10.0.22621。
- **GPU(可选)**:CUDA 12.x + cuDNN 9.x 运行时 DLL。开发机用 pip wheel 取并整合到 `vendor/cuda-redist/dll/`(见下"GPU")。

## 构建
```powershell
$cmake = "vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
& $cmake --build build --config Release
& "build\Release\suji_tests.exe"   # 全套测试(单元 + 跑真实模型的集成测试)
```

## 运行
**单文件:**
```powershell
build\Release\suji_cli.exe "<讲课文件.mp4>" -o build\out
# 选项: --provider cpu|cuda  --rule-fsts <f.fst>  --no-srt|--no-vtt|--no-json|--no-md
```
**批量(目录或多个文件):**
```powershell
build\Release\suji_batch.exe "<目录或多个文件>" -o build\out --provider auto
# 选项: --provider auto|cpu|cuda  --batch N  --in-flight N  --cuda-dll-dir <CUDA DLL 目录>
# auto: 自动探测;有可用 N 卡(且给了 --cuda-dll-dir)用 GPU,否则 CPU
```
输出:`<out>/<输入名>.{srt,vtt,json,md}`(全 UTF-8 无 BOM)。

## GPU 说明(重要)
- Windows 上,CUDA `CreateOfflineRecognizer` 在**缺 CUDA 运行时 DLL 时会硬崩**(不返回 null)。因此 `suji_batch` **只有显式 `--cuda-dll-dir <dir>`** 时才尝试 CUDA;否则 `auto`/`cuda` 安全回退 CPU。
- 开发机准备 GPU DLL:
  ```powershell
  python -m pip download --only-binary=:all: --dest vendor\cuda-redist `
    nvidia-cuda-runtime-cu12 nvidia-cublas-cu12 nvidia-cufft-cu12 nvidia-curand-cu12 nvidia-cudnn-cu12
  # 解压各 .whl,把 nvidia\*\bin\*.dll 整合到 vendor\cuda-redist\dll\,然后:
  $env:PATH = "F:\Git\suji-asr\vendor\cuda-redist\dll;" + $env:PATH
  build\Release\suji_batch.exe "<目录>" -o build\out --provider cuda --cuda-dll-dir F:\Git\suji-asr\vendor\cuda-redist\dll
  ```
- **吞吐**:GPU 的固定开销(CUDA/cuDNN 初始化 + 模型上显存,~20s)需**长音频**才摊薄;短小文件上 CPU 反而快。真实讲课(数十分钟到数小时)上 GPU 批量优势显现(见 benchmark)。

## 已知限制 / 待办(详见 `PROGRESS.md`)
- **ITN 默认关**:`--rule-fsts` 接线已就位,但未找到现成的中文 ITN FST(数字保留口语形式,如"二零二六");标点正常。待获取/构建 FST。
- **media_decode** 经 cmd.exe(`_popen`)起 ffmpeg,未对路径做 shell 转义;含 `%` 的文件名可能解码失败(安全报错跳过)。后续改 `CreateProcessW`。
- **JSON `tokens[]`** 是标点前的字级时间(用于对齐),`text`/`full_text` 是标点后可读文本——有意为之。
- 批量解码非 bit-exact(padding),与单流可有极小差异(正常)。

## 许可证
代码:本仓库。第三方:sherpa-onnx(Apache-2.0)、ONNX Runtime(MIT)、ffmpeg(LGPL build)、FireRedASR/CT-Transformer/Silero 模型权重(各自条款,商用前核实)、CUDA/cuDNN(NVIDIA 可再发行条款)。
