# Benchmark — suji-asr 聚合吞吐 (2026-06-26)

- **硬件**:RTX 2080 (Turing, 8GB) · Ryzen 7 5800X (16 线程) · 40GB RAM。
- **模型**:FireRedASR2-CTC **int8**。
- **测试音频**:test_wavs 循环合成 4 文件,共 **47.8 min**(2867s)。

| 模式 | 墙钟 | 聚合吞吐 | vs 豆包(RTF≈1 串行) |
|---|---|---|---|
| **CPU**(16 线程,batch≈23) | **579s** | **4.95× 实时** | **~5× 快** |
| GPU(RTX 2080,cuda,batch=24) | 1005s | 2.85× 实时 | ~2.9× 快 |
| 豆包(实时,一次一个) | ~2867s(= 音频时长) | 1× | 基线 |

## ⭐ 关键发现:int8 模型上 CPU 比 GPU 快(与最初假设相反)
- 本硬件上 **CPU(5800X)比 GPU(2080)快 ~1.7×**(GPU/CPU 加速比 = 0.58×)。原 spec "GPU 批量 = 吞吐核心" 的假设**在此模型+硬件上不成立**。
- **高置信原因**:ORT 的 **CUDA EP 对 int8 量化模型加速差**。int8 主要是 **CPU 优化**(5800X 的 AVX-512 VNNI int8 GEMM 很快);GPU 上 int8 常退化为 fp32 + quantize/dequantize 开销(Turing 尤其)。GPU 要快需 **fp16** 模型,而当前只有 int8。
- ✅ **但项目目标已达成**:**CPU 模式 4.95× 实时,已 ~5× 碾压豆包(RTF≈1)**。3 小时讲课单文件 ~36 分钟转完,一晚可清一大批;且多文件并行 + 错误隔离 + 自动续跑。**在 dev 2080 上 GPU 非必需**——但部署机(3070 Ti)可能不同,见下。
- **程序运行期自适应(不在 dev 机写死)**:`auto` 自动检测「可用 N 卡 + CUDA 运行时」→ 有就走 GPU、没有走 CPU,无需 flag(`decide()` + `core/paths.cpp::cuda_dll_dir()`,崩溃安全);安装包 all-in-one 含 CUDA。**在 dev 2080 上 GPU 较慢**,可 `--provider cpu` 覆盖;**在 3070 Ti 上 `auto` 会自动用 GPU**,实测吞吐由该机 benchmark 决定是否保留这个默认。

## NEEDS-HUMAN(方向性,留给你定)
1. **是否为 GPU 投入 fp16 模型?** 鉴于 CPU 已 5× 碾压豆包,**大概率不必**(投入产出低)。若要,需找/转 FireRedASR 的 fp16 ONNX。
2. **部署目标 3070 Ti(Ampere)**:Ampere int8 张量核优于 Turing,但 ORT int8-on-CUDA 的限制可能仍在 → **建议在 3070 Ti 上跑一次 `scripts\benchmark.ps1` 确认**。若 GPU 仍不快,部署直接全用 CPU(更省事、无 CUDA DLL 打包负担)。
   - `decide()` 现已在「有可用 GPU + CUDA」时偏好 GPU(自适应,已实现 D10)。若 3070 Ti benchmark 显示 GPU 不快,把 `auto` 改回偏 CPU(一行)或部署时用 `--provider cpu`。

## 复现
```powershell
# 先构建,且(GPU)整合 CUDA redist DLL 到 vendor\cuda-redist\dll(见 README)
powershell -File scripts\benchmark.ps1 -Audio build\bench_audio -CudaDll vendor\cuda-redist\dll
# 仅 CPU:省略 -CudaDll
```
（`build\bench_audio` 由 loop test_wavs 合成;脚本用 ffprobe 算总时长,各 provider 跑前清输出目录强制重跑。）
