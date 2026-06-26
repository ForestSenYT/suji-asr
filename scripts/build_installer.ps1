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
