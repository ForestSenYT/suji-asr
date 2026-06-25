; suji-asr 安装脚本 (Inno Setup 6) — CPU-only all-in-one。
; 构建: 先 cmake --build build --config Release + windeployqt build\Release\suji_gui.exe,
;       然后 ISCC.exe installer\suji.iss  → 产出 build\installer\suji-asr-setup-*.exe
; 说明: 打包 build\Release\ 的 exe+DLL+Qt运行时 + ffmpeg.exe + models\,装到目标机后
;       exe 用 app-relative 路径(core/paths.cpp)在自身目录找 models\ 与 ffmpeg.exe。
; 决策 D9: CPU-only(benchmark 显示 GPU 对 int8 不值)→ 不打包 CUDA redist,排除 cuda/tensorrt provider DLL。

#define MyAppName "suji 中文讲课转写"
#define MyAppVersion "0.5"
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
; ~813MB 模型为主;允许装到非系统盘
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
; --- 运行期 DLL (排除 CUDA/TensorRT provider —— CPU-only) ---
Source: "{#SrcRel}\*.dll"; DestDir: "{app}"; Flags: ignoreversion; Excludes: "onnxruntime_providers_cuda.dll,onnxruntime_providers_tensorrt.dll"
; --- Qt 平台/样式/tls 插件 (windeployqt 产出) ---
Source: "{#SrcRel}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "{#SrcRel}\styles\*";    DestDir: "{app}\styles";    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcRel}\tls\*";       DestDir: "{app}\tls";       Flags: ignoreversion recursesubdirs createallsubdirs
; --- ffmpeg (app_dir()/ffmpeg.exe) ---
Source: "{#SrcVendor}\ffmpeg-master-latest-win64-lgpl\bin\ffmpeg.exe"; DestDir: "{app}"; Flags: ignoreversion
; --- 模型 (app_dir()/models/...) ---
Source: "{#SrcModels}\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs createallsubdirs
; --- 文档 ---
Source: "..\README.md";    DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\BENCHMARK.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\suji 转写";        Filename: "{app}\{#MyAppExe}"
Name: "{group}\卸载 suji";        Filename: "{uninstallexe}"
Name: "{autodesktop}\suji 转写";  Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "启动 suji"; Flags: nowait postinstall skipifsilent
