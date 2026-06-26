# 一键构建 suji 安装包(all-in-one,含 CUDA,运行期自适应 GPU/CPU)。需先装 Inno Setup 6(ISCC.exe)。
#   powershell -File scripts\build_installer.ps1 [-Iscc <ISCC.exe 路径>] [-QtBin <Qt bin 目录>]
# 步骤: 1) cmake --build Release  2) windeployqt(Qt 运行时)  2b) 拷入 CUDA 运行时  3) ISCC 编译 installer\suji.iss
param(
  [string]$Iscc  = "",
  [string]$QtBin = "F:\Qt\6.8.1\msvc2022_64\bin"
)
$ErrorActionPreference = "Stop"
$ROOT  = Split-Path -Parent $PSScriptRoot
$cmake = Join-Path $ROOT "vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe"
$wdq   = Join-Path $QtBin "windeployqt.exe"

Write-Host "1) 构建 Release..."
& $cmake --build (Join-Path $ROOT "build") --config Release
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

Write-Host "2) windeployqt 收集 Qt 运行时..."
if (-not (Test-Path $wdq)) { throw "未找到 windeployqt: $wdq (用 -QtBin 指定 Qt 的 bin 目录)" }
& $wdq --release --no-translations --no-system-d3d-compiler --no-opengl-sw (Join-Path $ROOT "build\Release\suji_gui.exe")

Write-Host "2b) 拷入 CUDA 运行时 DLL(GPU 自适应所需;装到目标机后在 exe 同目录,供运行时检测)..."
$cudaDir = Join-Path $ROOT "vendor\cuda-redist\dll"
if (Test-Path $cudaDir) {
  Copy-Item (Join-Path $cudaDir "*.dll") (Join-Path $ROOT "build\Release") -Force
  Write-Host "   已拷入 $((Get-ChildItem (Join-Path $cudaDir '*.dll')).Count) 个 CUDA DLL"
} else {
  Write-Warning "未找到 CUDA 运行时 ($cudaDir) — 安装包将不含 CUDA,目标机只能用 CPU。"
}

Write-Host "2c) 拷入 VC++ 运行时 DLL(MSVCP140/VCRUNTIME140…;干净机即使没装 VC++ Redist 也能启动,CUDA EP 才能加载)..."
$crt = @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio") |
       ForEach-Object { Get-ChildItem -Directory (Join-Path $_ "*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT") -ErrorAction SilentlyContinue } |
       Sort-Object FullName | Select-Object -Last 1
if ($crt) {
  Copy-Item (Join-Path $crt.FullName "*.dll") (Join-Path $ROOT "build\Release") -Force
  Write-Host "   已拷入 VC++ 运行时: $($crt.FullName)"
} else {
  throw "未找到 VC++ Redist CRT 目录 (Microsoft.VC*.CRT)。需装 Visual Studio,或手动把 msvcp140.dll / vcruntime140.dll / vcruntime140_1.dll 拷到 build\Release。"
}

Write-Host "2d) 校验关键运行时齐全(缺则中止,绝不打出半成品包)..."
$rel = Join-Path $ROOT "build\Release"
$need = @("suji_gui.exe","suji_batch.exe","suji_cli.exe",
          "onnxruntime.dll","onnxruntime_providers_cuda.dll","onnxruntime_providers_shared.dll",
          "sherpa-onnx-c-api.dll","msvcp140.dll","vcruntime140.dll","vcruntime140_1.dll",
          "Qt6Core.dll","Qt6Gui.dll","Qt6Widgets.dll")
$missing = $need | Where-Object { -not (Test-Path (Join-Path $rel $_)) }
if ($missing) { throw "build\Release 缺少必需文件: $($missing -join ', ') — 中止打包。" }
if (-not (Test-Path (Join-Path $rel "platforms\qwindows.dll"))) { throw "缺少 Qt 平台插件 platforms\qwindows.dll(windeployqt 未产出?)— 中止。" }
if (-not (Get-ChildItem (Join-Path $rel "cudart64_*.dll") -ErrorAction SilentlyContinue)) { throw "缺少 CUDA 运行时 cudart64_*.dll — GPU 路径会失败,中止。" }
Write-Host "   ✓ 关键文件齐全(exe / onnxruntime+CUDA provider / sherpa / VC++ 运行时 / Qt / 平台插件 / CUDA 运行时)"

Write-Host "3) ISCC 编译安装包..."
if ($Iscc -eq "") {
  foreach ($c in @("F:\InnoSetup\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "${env:ProgramFiles}\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $c) { $Iscc = $c; break }
  }
}
if ($Iscc -eq "" -or -not (Test-Path $Iscc)) {
  throw "未找到 ISCC.exe。请装 Inno Setup 6 (https://jrsoftware.org/isdl.php),或用 -Iscc <路径> 指定。"
}
& $Iscc (Join-Path $ROOT "installer\suji.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC 编译失败" }
Write-Host "完成 → $ROOT\build\installer\suji-asr-setup-*.exe"
