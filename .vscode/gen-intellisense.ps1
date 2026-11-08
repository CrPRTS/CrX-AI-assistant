# Generate .vscode/c_cpp_properties.json from arduino-cli's compile_commands.json
#
# Why this script exists:
#   arduino-cli hides the ESP-IDF include paths inside GCC response files
#   (@flags/includes, ~370 entries written as -iwithprefixbefore <rel> plus a
#   single -iprefix <root>). cpptools does not expand @response files and does
#   not understand -iprefix / -iwithprefixbefore, so feeding it
#   compile_commands.json directly leaves <mbedtls/md.h>, <esp_system.h>, ...
#   unresolved. This script expands everything into absolute paths and also
#   adds the cross toolchain's own system header directories.
#
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads .ps1 files
#   without a UTF-8 BOM as ANSI; any non-ASCII byte (e.g. a Chinese comment)
#   can then be mis-decoded into a quote or a backslash and break the parser.
#
# Usage (normally invoked by the "Refresh IntelliSense config" task):
#   powershell -NoProfile -ExecutionPolicy Bypass -File .vscode\gen-intellisense.ps1
# Optional parameters:
#   -BuildDir  build\AI_KEY_v3   directory holding compile_commands.json
#   -SketchDir AI_KEY_v3         sketch source directory

[CmdletBinding()]
param(
    [string]$BuildDir = 'build\AI_KEY_v3',
    [string]$SketchDir = 'AI_KEY_v3'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dbPath = Join-Path $root $BuildDir | Join-Path -ChildPath 'compile_commands.json'

if (-not (Test-Path $dbPath)) {
    throw "Missing $dbPath - run a compile first (or the 'Refresh IntelliSense config' task)."
}

$db = Get-Content $dbPath -Raw | ConvertFrom-Json
$entry = $db | Where-Object { $_.file -like '*sketch*' } | Select-Object -First 1
if (-not $entry) { $entry = $db[0] }
$args_ = @($entry.arguments)

# ---- Expand GCC response files: replace @file with the tokens inside it ----
function Expand-Argv {
    param([string[]]$Argv)
    $out = [System.Collections.Generic.List[string]]::new()
    foreach ($a in $Argv) {
        if ($a.StartsWith('@')) {
            $rf = $a.Substring(1)
            if (Test-Path $rf) {
                $tokens = (Get-Content $rf -Raw) -split '\s+' | Where-Object { $_ }
                # restore quotes escaped in the response file (-DXX=\"yy\")
                foreach ($t in $tokens) { $out.Add(($t -replace '\\"', '"')) }
            }
        }
        else { $out.Add($a) }
    }
    ,$out.ToArray()
}
$argv = Expand-Argv $args_

# ---- -iprefix base directory (the root for -iwithprefixbefore) ----
$iprefix = $null
for ($i = 0; $i -lt $argv.Count; $i++) {
    if ($argv[$i] -eq '-iprefix') { $iprefix = $argv[$i + 1]; break }
}

$inc = [System.Collections.Generic.List[string]]::new()
$def = [System.Collections.Generic.List[string]]::new()

for ($i = 0; $i -lt $argv.Count; $i++) {
    $a = $argv[$i]
    $next = if ($i + 1 -lt $argv.Count) { $argv[$i + 1] } else { $null }
    # Do not use switch here: it is case-insensitive by default, and
    # '-I.' would also match '-iwithprefixbefore'.
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
        try { $p = [System.IO.Path]::GetFullPath($p) } catch { }
        $inc.Add($p)
    }
}

# ---- Cross toolchain system header directories (<string.h>, <cstdint>, ...) ----
$gxx = $argv[0]
# compile_commands.json stores the compiler path without the .exe suffix
if (-not (Test-Path $gxx) -and (Test-Path "$gxx.exe")) { $gxx = "$gxx.exe" }
$compilerPath = $null
if (Test-Path $gxx) {
    $gxxFull = (Resolve-Path $gxx).Path
    # Without compilerPath, cpptools falls back to C_Cpp.default.compilerPath
    # (here a host MinGW g++), which predefines __x86_64__, _WIN64 and
    # __SIZEOF_POINTER__=8. IDF/mbedTLS/TLSF headers branch on those, so
    # IntelliSense parses 64-bit host branches that never exist on the target.
    # Pointing at the cross compiler gives the real 32-bit Xtensa predefines.
    $compilerPath = $gxxFull
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
        # chip specific libstdc++ configuration header directories
        Get-ChildItem (Join-Path $elfInc "c++\$gccVer") -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'xtensa-esp-elf*' } |
            ForEach-Object { $inc.Add($_.FullName); Get-ChildItem $_.FullName -Directory | ForEach-Object { $inc.Add($_.FullName) } }
    }

    # ---- Target architecture predefined macros of the cross compiler ----
    # IDF headers branch on these, e.g. xtensa/config/core.h:
    #   #ifdef __XTENSA__  -> #include <xtensa/hal.h>
    #   #else              -> #include "../hal.h"     <- cpptools takes this dead branch and reports 1696
    # cpptools uses the host (x64) predefined set, which has no __XTENSA__, so
    # they must be injected explicitly.
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

# ---- sketch source directory (.ino lives next to the generated .ino.cpp) ----
$inc.Add((Join-Path $root $SketchDir))

# ---- dedupe, drop directories that do not exist ----
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$finalInc = [System.Collections.Generic.List[string]]::new()
$dropped = 0
foreach ($p in $inc) {
    $norm = $p.TrimEnd('\')
    if (-not $norm) { continue }
    if (-not (Test-Path -LiteralPath $norm)) { $dropped++; continue }
    if ($seen.Add($norm)) { $finalInc.Add($norm) }
}

# ---- macros: keep every value verbatim ----
$finalDef = $def | Where-Object { $_ } | Select-Object -Unique | Sort-Object

# ---- language standard: the LAST -std token wins, exactly like GCC ----
# The esp32 core passes -std twice (currently "-std=gnu++2b ... -std=gnu++2a"),
# so the real standard is C++20 (__cplusplus == 202002L), not gnu++23. Reading
# it from the response-file-expanded argv keeps the config in sync with the
# compiler instead of hardcoding a guess that goes stale on the next core bump.
function Get-GccStandard {
    param([string[]]$Argv, [string]$Fallback)
    $token = $null
    for ($i = 0; $i -lt $Argv.Count; $i++) {
        $t = $Argv[$i]
        if ($t -ceq '-std') {
            if ($i + 1 -lt $Argv.Count) { $token = $Argv[$i + 1] }
        }
        elseif ($t -clike '-std=*') { $token = $t.Substring(5) }
    }
    if (-not $token) { return $Fallback }
    # cpptools only accepts c++98..c++26 / gnu++98..gnu++26, while GCC also
    # accepts the pre-ratification aliases 0x, 1y, 1z, 2a, 2b, 2c.
    $year = $token -replace '^(?:gnu|c)\+\+', ''
    $alias = @{ '0x' = '11'; '1y' = '14'; '1z' = '17'; '2a' = '20'; '2b' = '23'; '2c' = '26' }
    if ($alias.ContainsKey($year)) { $year = $alias[$year] }
    if ($year -notmatch '^(?:98|03|11|14|17|20|23|26)$') { return $Fallback }
    if ($token -clike 'gnu++*') { return "gnu++$year" }
    return "c++$year"
}
$cppStandard = Get-GccStandard $argv 'gnu++23'

# ---- write c_cpp_properties.json ----
# compileCommands is deliberately omitted:
#   compile_commands.json only lists build\AI_KEY_v3\sketch\AI_KEY_v3.ino.cpp,
#   never the AI_KEY_v3.ino the editor has open, so cpptools falls back to
#   includePath anyway and prints a warning. includePath + defines below is
#   already the complete set expanded from the real compile arguments.
# ---- emit JSON by hand ----
# ConvertTo-Json is deliberately not used: PowerShell 5.1 and 7 differ in both
# indentation and the space after ':', so the same config would produce a
# whole-file diff depending on which host happened to run this script.
function ConvertTo-JsonString {
    param([string]$s)
    $sb = [System.Text.StringBuilder]::new()
    foreach ($ch in $s.ToCharArray()) {
        $code = [int]$ch
        if ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($ch -eq '"') { [void]$sb.Append('\"') }
        elseif ($code -eq 8) { [void]$sb.Append('\b') }
        elseif ($code -eq 12) { [void]$sb.Append('\f') }
        elseif ($code -eq 10) { [void]$sb.Append('\n') }
        elseif ($code -eq 13) { [void]$sb.Append('\r') }
        elseif ($code -eq 9) { [void]$sb.Append('\t') }
        elseif ($code -lt 32) { [void]$sb.Append(('\u{0:x4}' -f $code)) }
        else { [void]$sb.Append($ch) }
    }
    $sb.ToString()
}

$nl = "`r`n"
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('{')
$lines.Add('    "version": 4,')
$lines.Add('    "configurations": [')
$lines.Add('        {')
$lines.Add('            "name": "Win32",')
$lines.Add('            "includePath": [')
for ($i = 0; $i -lt $finalInc.Count; $i++) {
    $sep = ''
    if ($i -lt $finalInc.Count - 1) { $sep = ',' }
    $lines.Add('                "' + (ConvertTo-JsonString $finalInc[$i]) + '"' + $sep)
}
$lines.Add('            ],')
$lines.Add('            "defines": [')
for ($i = 0; $i -lt $finalDef.Count; $i++) {
    $sep = ''
    if ($i -lt $finalDef.Count - 1) { $sep = ',' }
    $lines.Add('                "' + (ConvertTo-JsonString $finalDef[$i]) + '"' + $sep)
}
$lines.Add('            ],')
if ($compilerPath) {
    $lines.Add('            "compilerPath": "' + (ConvertTo-JsonString $compilerPath) + '",')
}
$lines.Add('            "cStandard": "gnu17",')
$lines.Add('            "cppStandard": "' + $cppStandard + '",')
$lines.Add('            "intelliSenseMode": "linux-gcc-x64"')
$lines.Add('        }')
$lines.Add('    ]')
$lines.Add('}')
$json = ($lines -join $nl) + $nl

$outPath = Join-Path $PSScriptRoot 'c_cpp_properties.json'
[System.IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "Generated $outPath"
Write-Host "  include dirs: $($finalInc.Count) (skipped missing: $dropped)"
Write-Host "  defines:      $($finalDef.Count)"