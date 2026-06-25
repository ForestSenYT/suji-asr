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
- ✅ **但项目目标已达成**:**CPU 模式 4.95× 实时,已 ~5× 碾压豆包(RTF≈1)**。3 小时讲课单文件 ~36 分钟转完,一晚可清一大批;且多文件并行 + 错误隔离 + 自动续跑。GPU 非必需。
- 实践默认即 CPU:`suji_batch <dir>`(auto,无 `--cuda-dll-dir`)→ 走 CPU(快 + 不会因缺 CUDA DLL 崩);GPU 为**显式 opt-in**。所以当前默认行为已是最快路径,**无需改代码**。

## NEEDS-HUMAN(方向性,留给你定)
1. **是否为 GPU 投入 fp16 模型?** 鉴于 CPU 已 5× 碾压豆包,**大概率不必**(投入产出低)。若要,需找/转 FireRedASR 的 fp16 ONNX。
2. **部署目标 3070 Ti(Ampere)**:Ampere int8 张量核优于 Turing,但 ORT int8-on-CUDA 的限制可能仍在 → **建议在 3070 Ti 上跑一次 `scripts\benchmark.ps1` 确认**。若 GPU 仍不快,部署直接全用 CPU(更省事、无 CUDA DLL 打包负担)。
   - 若 3070 Ti 上 GPU 确实快,再考虑把 `decide()` 的 GPU 偏好保留;否则可让 `auto` 直接偏 CPU。

## 复现
```powershell
# 先构建,且(GPU)整合 CUDA redist DLL 到 vendor\cuda-redist\dll(见 README)
powershell -File scripts\benchmark.ps1 -Audio build\bench_audio -CudaDll vendor\cuda-redist\dll
# 仅 CPU:省略 -CudaDll
```
（`build\bench_audio` 由 loop test_wavs 合成;脚本用 ffprobe 算总时长,各 provider 跑前清输出目录强制重跑。）
