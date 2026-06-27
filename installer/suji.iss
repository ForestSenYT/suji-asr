; suji-asr 安装脚本 (Inno Setup 6) — all-in-one (含 CUDA;运行期自适应 GPU/CPU)。
; 构建: scripts\build_installer.ps1 会 cmake --build Release + windeployqt + 把 CUDA 运行时拷入 build\Release,
;       然后 ISCC.exe installer\suji.iss  → 产出 build\installer\suji-asr-setup-*.exe
; 说明: 打包 build\Release\ 的 exe+DLL+Qt运行时 + CUDA 运行时 + ffmpeg.exe + models\。装到目标机后
;       exe 用 app-relative 路径(core/paths.cpp)在自身目录找 models\、ffmpeg.exe 与 CUDA DLL,
;       并由 core/hardware.cpp 在运行期自适应:检测到可用 NVIDIA GPU + CUDA 运行时 → 走 GPU,否则 CPU。
; 决策 D10(修正 D9):打包 CUDA,让部署机(如 3070 Ti)能用 GPU;是否真比 CPU 快需在该机 benchmark 确认。
;       仅排除未使用的 TensorRT provider(sherpa 用 CUDA EP,不用 TRT)。

#define MyAppName "suji 中文讲课转写"
#define MyAppVersion "1.0"
#define MyAppPublisher "suji-asr"
#define MyAppExe "suji_gui.exe"
#define SrcRel "..\build\Release"
#define SrcVendor "..\vendor"
#define SrcModels "..\models"

[Setup]
AppId={{8F2C7A10-5B3D-4E91-9C2A-SUJIASR000001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\suji-asr
DefaultGroupName=suji-asr
DisableProgramGroupPage=yes
OutputDir=..\build\installer
OutputBaseFilename=suji-asr-setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; ~模型 ~1.8GB(Qwen3-ASR 0.95GB[准确度] + int8-CTC 742MB[词级字幕] + 标点 77MB;AED 已排除)+ CUDA 运行时 ~2.4GB + ffmpeg 108MB(lzma2 压缩后约 3GB,单文件 .exe);允许装到非系统盘
UsePreviousAppDir=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Files]
; --- 可执行 (排除测试 exe) ---
Source: "{#SrcRel}\suji_gui.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcRel}\suji_cli.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcRel}\suji_batch.exe"; DestDir: "{app}"; Flags: ignoreversion
; --- 运行期 DLL: onnxruntime(含 CUDA provider)+ sherpa + Qt + CUDA 运行时(cudnn/cublas/cudart…)。
;     CUDA 运行时由 scripts\build_installer.ps1 预先从 vendor\cuda-redist\dll 拷入 build\Release。
;     仅排除未使用的 TensorRT provider。 ---
Source: "{#SrcRel}\*.dll"; DestDir: "{app}"; Flags: ignoreversion; Excludes: "onnxruntime_providers_tensorrt.dll"
; --- Qt 插件 (windeployqt 产出): 平台/样式/tls + 图片格式/图标引擎/通用/网络信息 ---
Source: "{#SrcRel}\platforms\*";          DestDir: "{app}\platforms";          Flags: ignoreversion recursesubdirs
Source: "{#SrcRel}\styles\*";             DestDir: "{app}\styles";             Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\tls\*";                DestDir: "{app}\tls";                Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\imageformats\*";       DestDir: "{app}\imageformats";       Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\iconengines\*";        DestDir: "{app}\iconengines";        Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\generic\*";            DestDir: "{app}\generic";            Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs createallsubdirs
; --- ffmpeg + ffprobe (app_dir()/ffmpeg.exe, app_dir()/ffprobe.exe) ---
Source: "{#SrcVendor}\ffmpeg-master-latest-win64-lgpl\bin\ffmpeg.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcVendor}\ffmpeg-master-latest-win64-lgpl\bin\ffprobe.exe"; DestDir: "{app}"; Flags: ignoreversion
; --- 模型 (app_dir()/models/...) ---
; v1.0: 排除 fp16-AED「速度」模型(2.2GB)以保持单文件 .exe < 4.2GB 上限。
;       Qwen3(准确度,默认)+ int8-CTC(词级字幕)+ 标点 已足够;GUI 自动禁用「速度」模式。
Source: "{#SrcModels}\*"; DestDir: "{app}\models"; Excludes: "sherpa-onnx-fire-red-asr-large-zh_en-fp16-2025-02-16\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; --- 文档 ---
Source: "..\README.md";    DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\BENCHMARK.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\suji 转写";        Filename: "{app}\{#MyAppExe}"
Name: "{group}\卸载 suji";        Filename: "{uninstallexe}"
Name: "{autodesktop}\suji 转写";  Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "启动 suji"; Flags: nowait postinstall skipifsilent
