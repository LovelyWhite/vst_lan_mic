; LAN MIC 插件 Windows 安装脚本（Inno Setup 7，兼容 Inno Setup 6.3+）
; 由 scripts/make_installer.ps1 调用编译:
;   ISCC.exe /DAppVersion=x.y.z installer_windows.iss
; 暂存目录: dist\win-staging （由 make_installer.ps1 生成）
; 产物:     dist\LANMIC_Setup_<版本>.exe

#ifndef AppVersion
#define AppVersion "0.1.0"
#endif

#define AppName "LAN MIC Plugins"
#define AppPublisher "suxitech"
#define StagingDir "..\dist\win-staging"

[Setup]
AppId={{5C6B7A90-1E2D-4F3A-9B8C-7D6E5F4A3B2C}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
; 安装目录固定为 VST3 标准目录，由 Vst3Dir() 按权限决定
DefaultDirName={code:Vst3Dir}
DirExistsWarning=no
AppendDefaultDirName=no
DisableDirPage=yes
; 允许用户在安装时选择“为所有用户安装”(管理员) 或“仅为我安装”
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
OutputDir=..\dist
OutputBaseFilename=LANMIC_Setup_{#AppVersion}
Compression=lzma2
SolidCompression=yes
Uninstallable=yes
DisableProgramGroupPage=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; 递归复制整个 .vst3 bundle 目录结构
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
// VST3 标准目录: 管理员 -> C:\Program Files\Common Files\VST3
//                普通用户 -> %LOCALAPPDATA%\Programs\Common\VST3
function Vst3Dir(Param: string): string;
begin
    if IsAdminInstallMode then
        Result := ExpandConstant('{cf}\VST3')
    else
        Result := ExpandConstant('{localappdata}\Programs\Common\VST3');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
    // 目录固定，跳过目录选择页
    Result := (PageID = wpSelectDir);
end;
