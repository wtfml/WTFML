#Requires -Version 7.0
<#
.SYNOPSIS
    用 lupdate 刷新英文目录，并生成 Crowdin 使用的简体中文源目录。

.DESCRIPTION
    跑完之后**必须**打开 .ts 把新出现的条目补上译文（构建设不会因为"没翻"失败，
    没翻的条目英文界面里显示的还是中文）。流程见 docs/dev/I18N.md。

    lupdate 自己写出的 .ts 与仓库里现有的格式不一样（缩进、引号转义、换行），
    脚本在它之后折回仓库的格式，这样刷新只留下条目本身的增删，不会出现几万行格式差异。

    找不到 lupdate 时按顺序试：-Lupdate 参数、Qt6LinguistTools 的 lupdate（若在 PATH 里）、
    本机那份动态 Qt 的 bin（静态 Qt 只编了 qtbase，里面没有 lupdate）。

.PARAMETER Lupdate
    lupdate.exe 的完整路径。

.PARAMETER RepoRoot
    仓库根目录。默认是脚本所在目录的上一级。

.PARAMETER CrowdinSourceOnly
    用现有英文目录生成 Crowdin 源文件，无需 lupdate。
#>
[CmdletBinding()]
param(
    [string]$Lupdate = '',
    [string]$RepoRoot = '',
    [switch]$CrowdinSourceOnly
)

$ErrorActionPreference = 'Stop'
if (-not $RepoRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)

$textTags = 'source|translation|comment|numerusform'

# 把 lupdate 写出的 .ts 折回仓库既有格式：逐层 4 个空格、文本里的引号不转义、
# 多行文本的收尾标签顶格、换行 CRLF、编码 UTF-8 无 BOM。
function Format-TsCatalog([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = $text.Split("`n")
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }

    $result = [Collections.Generic.List[string]]::new()
    $depth = 0
    $inText = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($inText) {
            # 多行文本按原样写回，收尾标签留在行首，内容一个字符都不动。
            $result.Add($line)
            if ($line -match "</($textTags)>\s*$") { $inText = $false }
            continue
        }
        if ($line.Trim() -eq '') { $result.Add(''); continue }
        $s = $line.TrimStart()
        if ($s -match '^<\?xml' -or $s -match '^<!DOCTYPE') { $result.Add($s.TrimEnd()); continue }
        if ($s -match '^</([A-Za-z0-9_:-]+)>\s*$') {
            $depth--
            $indent = $depth * 4
            # 仓库现有文件里根元素的收尾标签缩进一级，保持原样，免得每次刷新都多一行格式差异。
            if ($Matches[1] -eq 'TS') { $indent = 4 }
            $result.Add((' ' * $indent) + $s.TrimEnd())
            continue
        }
        $result.Add((' ' * ($depth * 4)) + $s)
        if ($s -match '^<([A-Za-z0-9_:-]+)( [^>]*)?>\s*$') {
            $tag = $Matches[1]
            if ($tag -match "^($textTags)$") {
                # 空的开标签后面若跟着子元素就是容器（复数形式），否则是多行文本。
                $next = ''
                for ($j = $i + 1; $j -lt $lines.Count; $j++) {
                    if ($lines[$j].Trim() -ne '') { $next = $lines[$j].Trim(); break }
                }
                if ($next.StartsWith('<')) { $depth++ } else { $inText = $true }
            }
            else { $depth++ }
            continue
        }
        if ($s -match '^<([A-Za-z0-9_:-]+)( [^>]*)?>') {
            $tag = $Matches[1]
            if (($tag -match "^($textTags)$") -and ($s -notmatch "</$tag>\s*$")) { $inText = $true }
            elseif (($s -notmatch '</') -and ($s -notmatch '/>\s*$')) { $depth++ }
        }
    }

    $final = ($result -join "`r`n") + "`r`n"
    # Qt 会把文本里的引号写成 &quot; / &apos;，仓库里保留原文，其余实体不动。
    $final = $final.Replace('&quot;', '"').Replace('&apos;', "'")
    [IO.File]::WriteAllText($Path, $final, [Text.UTF8Encoding]::new($false))
}

# 用英文目录生成 Crowdin 源文件：源文照抄，译文清空，格式与英文目录一致。
function Write-CrowdinSource([string]$TranslationPath, [string]$SourcePath) {
    $text = [IO.File]::ReadAllText($TranslationPath)
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $lines = $text.Split("`n")
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }
    $result = [Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^<TS ') {
            $tsLine = $line -replace 'language="[^"]*"', 'language="zh_CN"' -replace '>$', ' sourcelanguage="zh_CN">'
            $result.Add($tsLine)
            continue
        }
        if ($line.TrimStart() -match '^<translation') {
            $indent = $line.Substring(0, $line.Length - $line.TrimStart().Length)
            $result.Add($indent + '<translation type="unfinished"></translation>')
            if ($line -notmatch '</translation>\s*$') {
                for ($j = $i + 1; $j -lt $lines.Count; $j++) {
                    if ($lines[$j] -match '</translation>\s*$') { $i = $j; break }
                }
            }
            continue
        }
        $result.Add($line)
    }

    $final = ($result -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($SourcePath, $final, [Text.UTF8Encoding]::new($false))
    Write-Host "已生成 Crowdin 源文件：$SourcePath"
}

$ts = Join-Path $RepoRoot 'assets/i18n/wtfml_en.ts'
$crowdinSource = Join-Path $RepoRoot 'assets/i18n/wtfml_zh-CN.ts'
if ($CrowdinSourceOnly) {
    Write-CrowdinSource $ts $crowdinSource
    exit 0
}

if (-not $Lupdate) {
    $candidates = [Collections.Generic.List[string]]::new()
    foreach ($name in @('lupdate', 'lupdate.exe')) {
        $found = Get-Command $name -ErrorAction SilentlyContinue
        if ($found) { $candidates.Add($found.Source) }
    }
    $candidates.Add('D:/Qt/Qt/6.10.1/mingw_64/bin/lupdate.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { $Lupdate = $candidate; break }
    }
}
if (-not $Lupdate -or -not (Test-Path -LiteralPath $Lupdate)) {
    Write-Host '找不到 lupdate。用 -Lupdate <lupdate.exe 的完整路径> 指一个（动态 Qt 的 bin 里有）。'
    exit 1
}

Push-Location $RepoRoot
try {
    & $Lupdate -locations none -no-obsolete src ui -ts $ts
    if ($LASTEXITCODE -ne 0) { Write-Host "lupdate 失败：$LASTEXITCODE"; exit $LASTEXITCODE }
} finally {
    Pop-Location
}
Format-TsCatalog $ts
Write-CrowdinSource $ts $crowdinSource

# 没翻的条目数：只报个数，判不判失败交给流程（第 1 期只翻试点那几句）。
$text = [IO.File]::ReadAllText($ts)
$unfinished = ([regex]::Matches($text, 'type="unfinished"')).Count
$total = ([regex]::Matches($text, '<message>')).Count
Write-Host ("已刷新 {0}：{1} 条，其中未翻译 {2} 条。" -f $ts, $total, $unfinished)
exit 0
