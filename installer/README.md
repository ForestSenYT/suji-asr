# suji 安装包(Inno Setup,all-in-one,含 CUDA,运行期自适应 GPU/CPU)

把 `suji_gui` + `suji_cli` + `suji_batch` + Qt 运行时 + sherpa/ORT DLL + **CUDA 运行时** + ffmpeg + 模型 打成一个**离线安装包**,目标机零前置依赖。装好后 exe 用 **app-relative 路径**(`core/paths.cpp`)在自身目录找 `models\`、`ffmpeg.exe` 和 CUDA DLL,并由 `core/hardware.cpp` **运行期自适应**:检测到可用 NVIDIA GPU + CUDA 运行时 → 走 GPU,否则 CPU。同一个安装包在有/无独显的机器上都能跑,不在开发机上写死。

> **决策 D10(修正 D9)**:打包 CUDA,让部署机(如 3070 Ti)能用 GPU。注意:dev 机(2080)上 benchmark 显示本 int8 模型 GPU 比 CPU 慢(部分是 ORT 对 int8 的 CUDA 支持弱,非纯显卡算力),**3070 Ti 上 GPU 是否真更快需在该机跑 `scripts\benchmark.ps1` 确认**;程序无论如何都会自适应——默认有可用 GPU 就走 GPU,可用 `--provider cpu` 覆盖。仅排除未用的 TensorRT provider(sherpa 用 CUDA EP)。
>
> 体积:模型 819MB + CUDA 运行时 ~2.4GB + ffmpeg 108MB → setup.exe(lzma2)压缩后约 1.5–2GB。

## 前置(一次性)
1. 装 **Inno Setup 6**:https://jrsoftware.org/isdl.php(挂机时该站下载被代理挡,故需手动装)。
2. `vendor\cuda-redist\dll\` 里有 CUDA 运行时 DLL(cudnn/cublas/cudart… 共 21 个,本仓库已整合)。

## 构建安装包
```powershell
powershell -File scripts\build_installer.ps1
# 它做:1) cmake --build Release  2) windeployqt(Qt 运行时)  2b) 拷 CUDA 运行时入 build\Release  3) ISCC 编译
# 或手动:
#   vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
#   F:\Qt\6.8.1\msvc2022_64\bin\windeployqt.exe --release --no-translations build\Release\suji_gui.exe
#   Copy-Item vendor\cuda-redist\dll\*.dll build\Release\ -Force
#   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\suji.iss
```
产物:`build\installer\suji-asr-setup-0.5.exe`。

## 安装包内容(installer\suji.iss 定义)
- `build\Release\` 的 `suji_{gui,cli,batch}.exe` + 运行期 DLL(onnxruntime 含 CUDA provider、sherpa、Qt、CUDA 运行时;**仅排除** TensorRT provider)+ Qt 的 `platforms\`/`styles\`/`tls\`。
- `vendor\...\ffmpeg.exe` → `{app}\ffmpeg.exe`。
- `models\*` → `{app}\models\`。
- 装到 `{autopf}\suji-asr`(可改);per-user 安装无需管理员;开始菜单 + 可选桌面快捷方式。

## 验证(NEEDS-HUMAN)
挂机环境无法下载 Inno、也无法验证干净机安装 → 请在装好 Inno 后构建,并在**部署机 3070 Ti** 上装一次,确认:
- (a) 能找到模型并转写;
- (b) 自动用上 GPU(GUI 状态栏 / CLI 的 `tune:` 行应显示 `provider=cuda`);
- (c) 跑 `scripts\benchmark.ps1` 看 GPU vs CPU 实测吞吐,决定默认是否保留 GPU。
