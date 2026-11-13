# Package the LAN MIC plugins as a Windows installer (Inno Setup).
# Run on Windows; needs Inno Setup 7 (ISCC.exe), falls back to Inno Setup 6.3+.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File make_installer.ps1 [-Version X.Y.Z] [plugin-or-dir]...
#
# Notes:
#   - Without plugin arguments, .vst3 files are found under vst3\build*.
#   - Version is parsed from common\Version.h by default.
#   - Output: dist\LANMIC_<version>.exe
#
# Examples:
#   .\make_installer.ps1                                  # auto-package from vst3\build*
#   .\make_installer.ps1 ..\vst3\build                    # explicit directory
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

# Version: parsed from common/Version.h by default.
if (-not $Version) {
    $verH = Join-Path $RepoRoot 'common\Version.h'
    if (Test-Path -LiteralPath $verH) {
        $m = Select-String -LiteralPath $verH -Pattern '^\s*#define\s+LANMIC_VERSION\s+"([0-9][0-9.]*)"' | Select-Object -First 1
        if ($m) { $Version = $m.Matches[0].Groups[1].Value }
    }
}
if (-not $Version) { $Version = '0.1.0' }

# Collect plugins.
if (-not $Path -or $Path.Count -eq 0) {
    $buildRoots = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'vst3') -Directory -Filter 'build*' -ErrorAction SilentlyContinue)
    $Path = @($buildRoots | ForEach-Object { $_.FullName })
}

$plugins = New-Object System.Collections.Generic.List[object]
foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p)) { throw "错误: 找不到 $p" }
    $item = Get-Item -LiteralPath $p
    if ($item.PSIsContainer) {
        # Directory: search recursively. Skip same-name files inside bundle Contents to avoid double collection.
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

# Deduplicate plugins by name, keeping the newest by mtime.
$plugins = @($plugins | Group-Object Name | ForEach-Object {
    $_.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
})

# Stage into dist\win-staging.
if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
New-Item -ItemType Directory -Path $Staging -Force | Out-Null
foreach ($plugin in $plugins) {
    Copy-Item -LiteralPath $plugin.FullName -Destination (Join-Path $Staging $plugin.Name) -Recurse -Force
    Write-Host "已暂存: $($plugin.Name)"
}

# Locate ISCC.exe (prefer Inno Setup 7: 64-bit in Program Files, 32-bit in Program Files (x86)).
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

# Compile the installer.
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
& $iscc "/DAppVersion=$Version" (Join-Path $ScriptDir 'installer_windows.iss')
if ($LASTEXITCODE -ne 0) { throw "错误: ISCC 编译失败" }

# Publish to site so index.html's version-built download links find the file.
$SetupExe = Join-Path $DistDir "LANMIC_$Version.exe"
$SiteExe = Join-Path $RepoRoot "site\LANMIC_$Version.exe"
Copy-Item -LiteralPath $SetupExe -Destination $SiteExe -Force

Write-Host ""
Write-Host "安装程序已生成: $SetupExe"
Write-Host "已复制到官网目录: $SiteExe"
