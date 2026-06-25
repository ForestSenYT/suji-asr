# suji benchmark — 聚合吞吐(音频秒/墙钟秒 = ×realtime),CPU vs GPU vs 豆包(RTF≈1 串行)。
# 用法:
#   powershell -File scripts\benchmark.ps1 -Audio <音频目录> [-CudaDll <CUDA DLL 目录>] [-Out build\bench]
# 说明:每个 provider 跑前清空其输出目录以强制重跑;GPU 需 -CudaDll 指向整合好的 CUDA/cuDNN DLL 目录。
param(
  [Parameter(Mandatory=$true)][string]$Audio,
  [string]$CudaDll = "",
  [string]$Out = "build\bench"
)
$ErrorActionPreference = "Stop"
$ROOT    = Split-Path -Parent $PSScriptRoot
$batch   = Join-Path $ROOT "build\Release\suji_batch.exe"
$ffprobe = Join-Path $ROOT "vendor\ffmpeg-master-latest-win64-lgpl\bin\ffprobe.exe"
if (-not (Test-Path $batch))   { Write-Error "未找到 $batch — 先构建 (cmake --build build --config Release)"; exit 1 }
if (-not (Test-Path $Audio))   { Write-Error "音频目录不存在: $Audio"; exit 1 }

# 总音频时长(秒)
$audioSec = 0.0
$files = Get-ChildItem $Audio -File
foreach ($f in $files) {
  if (Test-Path $ffprobe) {
    $d = & $ffprobe -v error -show_entries format=duration -of csv=p=0 $f.FullName 2>$null
    if ($d) { $audioSec += [double]$d }
  }
}
Write-Host ("audio: {0:N1} min ({1:N0}s) across {2} files" -f ($audioSec/60), $audioSec, $files.Count)

function Run-Provider($prov, $cudadll) {
  $od = Join-Path $ROOT ("{0}_{1}" -f $Out, $prov)
  if (Test-Path $od) { Remove-Item -Recurse -Force $od }   # 清空 → 强制重跑(绕过 resume)
  $a = @($Audio, "-o", $od, "--provider", $prov)
  if ($cudadll) { $a += @("--cuda-dll-dir", $cudadll) }
  $t0 = Get-Date
  & $batch @a 2>&1 | Out-Null
  return (New-TimeSpan $t0 (Get-Date)).TotalSeconds
}

Write-Host "=== CPU ==="
$cpu = Run-Provider "cpu" ""
Write-Host ("CPU: {0:N1}s  =>  {1:N1}x realtime" -f $cpu, ($audioSec/$cpu))

if ($CudaDll -ne "") {
  Write-Host "=== GPU ==="
  $env:PATH = "$CudaDll;" + $env:PATH
  $gpu = Run-Provider "cuda" $CudaDll
  Write-Host ("GPU: {0:N1}s  =>  {1:N1}x realtime   (GPU/CPU speedup {2:N2}x)" -f $gpu, ($audioSec/$gpu), ($cpu/$gpu))
}

Write-Host "=== vs 豆包(实时,RTF≈1,串行一次一个)==="
Write-Host ("豆包 ~= {0:N1} min wall(= 音频时长). CPU 提速 {1:N1}x." -f ($audioSec/60), ($audioSec/$cpu))
