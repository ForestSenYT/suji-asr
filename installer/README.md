# suji 安装包(Inno Setup,CPU-only all-in-one)

把 `suji_gui` + `suji_cli` + `suji_batch` + Qt 运行时 + sherpa/ORT(CPU)DLL + ffmpeg + 模型 打成一个**离线安装包**,目标机零前置依赖。装好后 exe 用 **app-relative 路径**(`core/paths.cpp`)在自身目录找 `models\` 和 `ffmpeg.exe`。

> **决策 D9:CPU-only**。benchmark(`BENCHMARK.md`)显示本 int8 模型在 GPU(2080)上比 CPU 慢(ORT 对 int8 GPU 加速差),CPU 已 ~5× 碾压豆包 → **安装包不打包 ~1.5GB 的 CUDA/cuDNN redist**,并排除 `onnxruntime_providers_cuda/tensorrt.dll`。体积 ~0.9–1GB(模型 819MB 为主)。

## 前置(一次性)
1. 装 **Inno Setup 6**:https://jrsoftware.org/isdl.php(挂机时该站下载被代理挡,故需手动装)。
2. 已构建 Release + 跑过 windeployqt(下面的脚本会自动做)。

## 构建安装包
```powershell
powershell -File scripts\build_installer.ps1
# 或手动三步:
#   vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
#   F:\Qt\6.8.1\msvc2022_64\bin\windeployqt.exe --release --no-translations build\Release\suji_gui.exe
#   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\suji.iss
```
产物:`build\installer\suji-asr-setup-0.5.exe`。

## 安装包内容(installer\suji.iss 定义)
- `build\Release\` 的 `suji_{gui,cli,batch}.exe` + 运行期 DLL(排除 cuda/tensorrt provider)+ Qt 的 `platforms\`/`styles\`/`tls\`。
- `vendor\...\ffmpeg.exe` → `{app}\ffmpeg.exe`。
- `models\*` → `{app}\models\`。
- 装到 `{autopf}\suji-asr`(可改);per-user 安装无需管理员;开始菜单 + 可选桌面快捷方式。

## 验证(NEEDS-HUMAN)
- 挂机环境无法下载 Inno、也无法验证干净机安装 → **请在装好 Inno 后构建,并在一台干净 Windows / 部署机(3070 Ti)上装一次、跑 `suji_gui` 确认能找到模型并转写**。
