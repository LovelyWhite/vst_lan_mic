# 将 VST3 (.vst3) / VST2 (.dll) 插件安装到 Windows 标准插件目录。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File install_vst_windows.ps1 [-User] <插件或目录>...
#
# 说明:
#   - 目录会被递归搜索其中的插件；同名插件保留修改时间最新的一个。
#   - 默认安装到系统目录（自动请求管理员权限），加 -User 安装到当前用户目录。
#
# 示例:
#   .\install_vst_windows.ps1 LAN_MIC_Send.vst3
#   .\install_vst_windows.ps1 -User .\lan_mic_return.vst3
#   .\install_vst_windows.ps1 .\build

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Path,
    [switch]$User
)

$ErrorActionPreference = 'Stop'

$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

# 写系统目录需要管理员权限，自动重新以管理员身份运行本脚本
if (-not $IsAdmin -and -not $User) {
    Write-Host "写入系统插件目录需要管理员权限，正在请求提升（UAC）..."
    $scriptArgs = ($Path | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $cmdLine = '-NoProfile -ExecutionPolicy Bypass -File "{0}" {1}' -f $PSCommandPath, $scriptArgs
    try {
        $proc = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $cmdLine
        exit $proc.ExitCode
    }
    catch {
        Write-Error "已取消权限提升，无法安装到系统目录。可加 -User 安装到当前用户目录。"
        exit 1
    }
}

function Get-DestDir([string]$Ext) {
    # VST3 标准目录 (Steinberg 规范); VST2 无统一标准，使用常见的 VSTPlugins 目录
    if ($Ext -eq '.vst3') {
        if ($User) { Join-Path $env:LOCALAPPDATA 'Programs\Common\VST3' }
        else       { Join-Path $env:ProgramFiles 'Common Files\VST3' }
    }
    else {
        if ($User) { Join-Path $env:APPDATA 'VSTPlugins' }
        else       { Join-Path $env:ProgramFiles 'VSTPlugins' }
    }
}

$plugins = New-Object System.Collections.Generic.List[object]
foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { throw "错误: 找不到 $p" }
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        # 目录: 递归搜索插件。排除插件包 Contents 内部的同名文件，避免重复安装。
        $bundles = @(Get-ChildItem -LiteralPath $item.FullName -Recurse -Directory -Filter *.vst3)
        foreach ($b in $bundles) { $plugins.Add($b) }
        $files = @(Get-ChildItem -LiteralPath $item.FullName -Recurse -File | Where-Object {
            ($_.Extension -ieq '.vst3' -or $_.Extension -ieq '.dll') -and $_.FullName -notmatch '\\Contents\\'
        })
        foreach ($f in $files) { $plugins.Add($f) }
    }
    elseif ($item.Extension -ieq '.vst3' -or $item.Extension -ieq '.dll') {
        $plugins.Add($item)
    }
    else {
        Write-Warning "跳过不支持的文件: $($item.FullName)"
    }
}

if ($plugins.Count -eq 0) { throw "错误: 未在给定路径中找到任何插件 (.vst3 / .dll)" }

# 同名插件去重，保留修改时间最新的一个（用于过滤 Debug/Release 等重复构建产物）
$plugins = @($plugins | Group-Object Name | ForEach-Object {
    $_.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
})

$count = 0
foreach ($plugin in $plugins) {
    $destDir = Get-DestDir ($plugin.Extension.ToLower())
    if (-not (Test-Path -LiteralPath $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    $dest = Join-Path $destDir $plugin.Name
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Copy-Item -LiteralPath $plugin.FullName -Destination $dest -Recurse -Force

    # 移除网络下载标记，避免 DAW 拒绝加载（best-effort）
    if ($plugin.PSIsContainer) {
        Get-ChildItem -LiteralPath $dest -Recurse -File -ErrorAction SilentlyContinue |
            Unblock-File -ErrorAction SilentlyContinue
    }
    else {
        Unblock-File -LiteralPath $dest -ErrorAction SilentlyContinue
    }

    Write-Host "已安装: $($plugin.Name) -> $dest"
    $count++
}

Write-Host ""
Write-Host "安装完成，共 $count 个插件。请重启 DAW 或重新扫描插件。"
