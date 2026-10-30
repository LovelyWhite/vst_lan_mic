# 打包 LAN MIC 插件为 Windows 安装程序（Inno Setup）。
# 在 Windows 上运行，需要 Inno Setup 7（ISCC.exe），回退支持 Inno Setup 6.3+。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File make_installer.ps1 [-Version X.Y.Z] [插件或目录]...
#
# 说明:
#   - 未指定插件时，自动在 vst3\build* 目录中查找 .vst3。
#   - 版本号默认从 vst3\CMakeLists.txt 的 project(... VERSION ...) 解析。
#   - 产物: dist\LANMIC_Setup_<版本>.exe
#
# 示例:
#   .\make_installer.ps1                                  # 自动从 vst3\build* 打包
#   .\make_installer.ps1 ..\vst3\build                    # 指定目录
#   .\make_installer.ps1 -Version 0.1.0 "LAN MIC Send.vst3"

[CmdletBinding()]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Path,
    [string]$Version = ""
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$Staging = Join-Path $RepoRoot 'dist\win-staging'
$DistDir = Join-Path $RepoRoot 'dist'

# 版本号: 默认从 CMakeLists 解析
if (-not $Version) {
    $cmake = Join-Path $RepoRoot 'vst3\CMakeLists.txt'
    if (Test-Path -LiteralPath $cmake) {
        $m = Select-String -LiteralPath $cmake -Pattern '^\s*project\s*\(.*VERSION\s+([0-9][0-9.]*)' | Select-Object -First 1
        if ($m) { $Version = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $Version) { $Version = '0.1.0' }

# 收集插件
if (-not $Path -or $Path.Count -eq 0) {
    $buildRoots = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'vst3') -Directory -Filter 'build*' -ErrorAction SilentlyContinue)
    $Path = @($buildRoots | ForEach-Object { $_.FullName })
}

$plugins = New-Object System.Collections.Generic.List[object]
foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { throw "错误: 找不到 $p" }
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        # 目录: 递归搜索。排除插件包 Contents 内部的同名文件，避免重复收集。
        $bundles = @(Get-ChildItem -LiteralPath $item.FullName -Recurse -Directory -Filter *.vst3)
        foreach ($b in $bundles) { $plugins.Add($b) }
        $files = @(Get-ChildItem -LiteralPath $item.FullName -Recurse -File | Where-Object {
            $_.Extension -ieq '.vst3' -and $_.FullName -notmatch '\\Contents\\'
        })
        foreach ($f in $files) { $plugins.Add($f) }
    }
    elseif ($item.Extension -ieq '.vst3') {
        $plugins.Add($item)
    }
    else {
        Write-Warning "跳过不支持的文件: $($item.FullName)"
    }
}
if ($plugins.Count -eq 0) { throw "错误: 未找到任何 .vst3 插件，请把插件路径作为参数传入" }

# 同名插件去重，保留修改时间最新的一个
$plugins = @($plugins | Group-Object Name | ForEach-Object {
    $_.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
})

# 暂存到 dist\win-staging
if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
New-Item -ItemType Directory -Path $Staging -Force | Out-Null
foreach ($plugin in $plugins) {
    Copy-Item -LiteralPath $plugin.FullName -Destination (Join-Path $Staging $plugin.Name) -Recurse -Force
    Write-Host "已暂存: $($plugin.Name)"
}

# 定位 ISCC.exe（优先 Inno Setup 7: 64 位版装在 Program Files，32 位版装在 Program Files (x86)）
$iscc = $null
$cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if ($cmd) { $iscc = $cmd.Source }
if (-not $iscc) {
    foreach ($dir in @(
        "$env:ProgramFiles\Inno Setup 7",
        "${env:ProgramFiles(x86)}\Inno Setup 7",
        "$env:ProgramFiles\Inno Setup 6",
        "${env:ProgramFiles(x86)}\Inno Setup 6"
    )) {
        $candidate = Join-Path $dir 'ISCC.exe'
        if (Test-Path -LiteralPath $candidate) { $iscc = $candidate; break }
    }
}
if (-not $iscc) {
    throw "错误: 未找到 ISCC.exe，请先安装 Inno Setup 7: https://jrsoftware.org/isdl.php"
}

# 编译安装程序
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
& $iscc "/DAppVersion=$Version" (Join-Path $ScriptDir 'installer_windows.iss')
if ($LASTEXITCODE -ne 0) { throw "错误: ISCC 编译失败" }

Write-Host ""
Write-Host "安装程序已生成: $(Join-Path $DistDir "LANMIC_Setup_$Version.exe")"
