# 从 arduino-cli 生成的 compile_commands.json 生成 .vscode/c_cpp_properties.json
#
# 为什么需要这个脚本：
#   arduino-cli 把 ESP-IDF 的 370 条 include 路径写在 gcc 响应文件里（@flags/includes，
#   全是 -iwithprefixbefore 相对路径，配合 -iprefix 使用）。
#   cpptools 的 IntelliSense 不展开 @响应文件、也不认 -iprefix/-iwithprefixbefore，
#   所以直接把 compile_commands.json 丢给它，<mbedtls/md.h>、<esp_system.h> 一定找不到。
#   这里把它们展开成绝对路径，再连同交叉编译器的系统头目录一起写进配置。
#
# 用法（一般由 tasks.json 的「刷新 IntelliSense 配置」任务调用）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .vscode\gen-intellisense.ps1
# 可选参数：
#   -BuildDir  build\AI_KEY_v3   含 compile_commands.json 的目录
#   -SketchDir AI_KEY_v3         sketch 源目录（用于把 .ino.cpp 映射回 .ino）

[CmdletBinding()]
param(
    [string]$BuildDir = 'build\AI_KEY_v3',
    [string]$SketchDir = 'AI_KEY_v3'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dbPath = Join-Path $root $BuildDir | Join-Path -ChildPath 'compile_commands.json'

if (-not (Test-Path $dbPath)) {
    throw "找不到 $dbPath，请先执行一次编译（或「刷新 IntelliSense 配置」任务）。"
}

$db = Get-Content $dbPath -Raw | ConvertFrom-Json
$entry = $db | Where-Object { $_.file -like '*sketch*' } | Select-Object -First 1
if (-not $entry) { $entry = $db[0] }
$args_ = @($entry.arguments)

# ---- 展开 gcc 响应文件：把 @file 替换成文件内容里的 token ----
function Expand-Argv {
    param([string[]]$Argv)
    $out = [System.Collections.Generic.List[string]]::new()
    foreach ($a in $Argv) {
        if ($a.StartsWith('@')) {
            $rf = $a.Substring(1)
            if (Test-Path $rf) {
                $tokens = (Get-Content $rf -Raw) -split '\s+' | Where-Object { $_ }
                # 响应文件里的引号（-DXX=\"yy\"）需要还原
                foreach ($t in $tokens) { $out.Add(($t -replace '\\"', '"')) }
            }
        }
        else { $out.Add($a) }
    }
    ,$out.ToArray()
}
$argv = Expand-Argv $args_

# ---- -iprefix 基准目录（-iwithprefixbefore 的相对根）----
$iprefix = $null
for ($i = 0; $i -lt $argv.Count; $i++) {
    if ($argv[$i] -eq '-iprefix') { $iprefix = $argv[$i + 1]; break }
}

$inc = [System.Collections.Generic.List[string]]::new()
$def = [System.Collections.Generic.List[string]]::new()

for ($i = 0; $i -lt $argv.Count; $i++) {
    $a = $argv[$i]
    $next = if ($i + 1 -lt $argv.Count) { $argv[$i + 1] } else { $null }
    # 注意：不要用 switch（默认大小写不敏感，'-I.' 会误匹配 '-iwithprefixbefore'）
    $path = $null
    if ($a -ceq '-iwithprefixbefore') {
        if ($iprefix) { $path = $iprefix + $next }
        $i++
    }
    elseif ($a -ceq '-I') { $path = $next; $i++ }
    elseif ($a -clike '-I*') { $path = $a.Substring(2) }
    elseif ($a -ceq '-iprefix') { $i++ }
    elseif ($a -ceq '-D') { $def.Add($next); $i++ }
    elseif ($a -clike '-D*') { $def.Add($a.Substring(2)) }

    if ($path) {
        $p = $path -replace '/', '\'
        # 去掉 .\ 之类的冗余段，便于去重
        try { $p = [System.IO.Path]::GetFullPath($p) } catch { }
        $inc.Add($p)
    }
}

# ---- 交叉编译器的系统头目录（<string.h>、<cstdint> 等）----
$gxx = $argv[0]
# compile_commands.json 里的编译器路径没有 .exe 后缀，Test-Path 会判 False
if (-not (Test-Path $gxx) -and (Test-Path "$gxx.exe")) { $gxx = "$gxx.exe" }
if (Test-Path $gxx) {
    $gxxFull = (Resolve-Path $gxx).Path
    # <toolchain>/bin/xtensa-esp32s3-elf-g++.exe  ->  <toolchain>
    $tc = Split-Path -Parent (Split-Path -Parent $gxxFull)
    $gccVer = (Get-ChildItem (Join-Path $tc 'lib\gcc\xtensa-esp-elf') -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1).Name
    if ($gccVer) {
        $inc.Add((Join-Path $tc "lib\gcc\xtensa-esp-elf\$gccVer\include"))
        $inc.Add((Join-Path $tc "lib\gcc\xtensa-esp-elf\$gccVer\include-fixed"))
        $elfInc = Join-Path $tc 'xtensa-esp-elf\include'
        $inc.Add($elfInc)
        $inc.Add((Join-Path $elfInc "c++\$gccVer"))
        # 芯片专用 libstdc++ 配置头目录
        Get-ChildItem (Join-Path $elfInc "c++\$gccVer") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'xtensa-esp-elf*' } |
            ForEach-Object { $inc.Add($_.FullName); Get-ChildItem $_.FullName -Directory | ForEach-Object { $inc.Add($_.FullName) } }
    }

    # ---- 交叉编译器的目标架构预定义宏 ----
    # IDF 头文件靠这些宏选分支，例如 xtensa/config/core.h：
    #   #ifdef __XTENSA__  -> #include <xtensa/hal.h>
    #   #else              -> #include "../hal.h"     <- cpptools 会走到这里并报 1696
    # cpptools 用宿主(x64)的预定义集合，没有 __XTENSA__，所以必须显式补上。
    $pre = & $gxxFull '-dM' '-E' '-x' 'c++' '-std=gnu++2b' 'NUL' 2>$null
    foreach ($line in @($pre)) {
        if ($line -match '^#define\s+(\S+)(?:\s+(.*))?$') {
            $name = $Matches[1]; $val = $Matches[2]
            if ($name -match '^(?:__XTENSA|__xtensa|__RISCV|__riscv|__ESPRESSIF|__esp)') {
                if ($null -ne $val -and $val -ne '') { $def.Add("$name=$val") } else { $def.Add($name) }
            }
        }
    }
}

# ---- sketch 源目录（.ino 与生成的 .ino.cpp 同目录）----
$inc.Add((Join-Path $root $SketchDir))

# ---- 去重、剔除不存在的目录 ----
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$finalInc = [System.Collections.Generic.List[string]]::new()
$dropped = 0
foreach ($p in $inc) {
    $norm = $p.TrimEnd('\')
    if (-not $norm) { continue }
    if (-not (Test-Path -LiteralPath $norm)) { $dropped++; continue }
    if ($seen.Add($norm)) { $finalInc.Add($norm) }
}

# ---- 宏：去掉带引号/表达式的复杂值容易踩坑，全部原样保留即可 ----
$finalDef = $def | Where-Object { $_ } | Select-Object -Unique | Sort-Object

# ---- 写 c_cpp_properties.json ----
# 不写 compileCommands：
#   compile_commands.json 里 sketch 的 file 是 arduino-cli 生成的
#   build\AI_KEY_v3\sketch\AI_KEY_v3.ino.cpp，而不是编辑器里打开的 AI_KEY_v3.ino，
#   cpptools 找不到就会退回 includePath 并弹提示。includePath + defines 已经是从
#   真实编译参数展开出来的完整集合，直接用它更干净。
$cfg = [ordered]@{
    name            = 'Win32'
    includePath     = @($finalInc)
    defines         = @($finalDef)
    cStandard       = 'gnu17'
    cppStandard     = 'gnu++23'
    intelliSenseMode = 'linux-gcc-x64'
}
$json = @{ configurations = @($cfg); version = 4 } | ConvertTo-Json -Depth 6

$outPath = Join-Path $PSScriptRoot 'c_cpp_properties.json'
[System.IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "已生成 $outPath"
Write-Host "  include 目录: $($finalInc.Count)（跳过不存在 $($dropped)）"
Write-Host "  宏定义:       $($finalDef.Count)"
