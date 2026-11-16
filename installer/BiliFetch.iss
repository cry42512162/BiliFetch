; ============================================================
;  BiliFetch 安装脚本（Inno Setup 7）
;  编译：D:\软件\AI工具\依赖\InnoSetup7\ISCC.exe installer\BiliFetch.iss
; ============================================================
#define MyAppName "BiliFetch"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "BiliFetch"
#define MyAppExeName "BiliFetch.exe"

[Setup]
AppId={{7C2E9A4B-3D51-4F8A-B6E0-2026BILI0001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\{#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=BiliFetch-Setup-{#MyAppVersion}
OutputDir=..\dist
SetupIconFile=..\assets\app.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoProductTextVersion={#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=no
ShowLanguageDialog=auto

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加任务："
Name: "quicklaunchicon"; Description: "创建快速启动栏快捷方式(&Q)"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; 主程序与运行时
Source: "..\build\BiliFetch.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bili_bridge.py"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\启动BiliFetch.bat"; DestDir: "{app}"; Flags: ignoreversion
; Qt 插件目录
Source: "..\build\platforms\*"; DestDir: "{app}\platforms"; Flags: recursesubdirs ignoreversion
Source: "..\build\generic\*"; DestDir: "{app}\generic"; Flags: recursesubdirs ignoreversion
Source: "..\build\iconengines\*"; DestDir: "{app}\iconengines"; Flags: recursesubdirs ignoreversion
Source: "..\build\imageformats\*"; DestDir: "{app}\imageformats"; Flags: recursesubdirs ignoreversion
Source: "..\build\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: recursesubdirs ignoreversion
Source: "..\build\styles\*"; DestDir: "{app}\styles"; Flags: recursesubdirs ignoreversion
Source: "..\build\tls\*"; DestDir: "{app}\tls"; Flags: recursesubdirs ignoreversion
; 文档
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppName}"; Comment: "B站内容抓取（bili-digest 驱动）"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppName}"; Tasks: quicklaunchicon

[Run]
Filename: "{app}\{#MyAppName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
Filename: "{app}\README.md"; Description: "查看使用说明"; Flags: shellexec postinstall skipifsilent unchecked

[UninstallDelete]
; 卸载时清理运行期产生的诊断日志（归档产物保留）
Type: files; Name: "{app}\bridge_calls.log"

[Code]
// 安装后环境自检：Python 3.10+ 与 bili-digest 插件目录
procedure CurStepChanged(CurStep: TSetupStep);
var
  pythonFound: Boolean;
  pluginFound: Boolean;
begin
  if CurStep <> ssPostInstall then
    Exit;

  pythonFound :=
    FileExists(ExpandConstant('{localappdata}\Programs\Python\Python310\python.exe')) or
    FileExists('C:\Python312\python.exe') or
    FileExists('C:\Python311\python.exe') or
    FileExists('C:\Python310\python.exe');

  pluginFound :=
    FileExists('D:\软件\AI工具\依赖\bili-digest\bili_digest.py');

  if not pythonFound then
    MsgBox('未检测到 Python 3.10+！' #13#10 #13#10 +
      'BiliFetch 需要 Python 运行抓取桥接脚本（bili_bridge.py）。' #13#10 +
      '请安装 Python 3.10 或更高版本；若安装在非常规路径，' #13#10 +
      '可设置环境变量 BILI_BRIDGE_PYTHON 指向 python.exe。', mbError, MB_OK);

  if not pluginFound then
    MsgBox('未检测到 bili-digest 插件：' #13#10 +
      'D:\软件\AI工具\依赖\bili-digest\bili_digest.py' #13#10 #13#10 +
      'BiliFetch 复用该插件的抓取管线（WBI 签名 / 风控穿透 / 评论翻页）。' #13#10 +
      '若插件在其他位置，请设置环境变量 BILI_DIGEST_DIR 指向其目录。', mbError, MB_OK);
end;
