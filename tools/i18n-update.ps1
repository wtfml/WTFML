#Requires -Version 7.0
<#
.SYNOPSIS
    用 lupdate 刷新英文目录，并生成 Crowdin 使用的简体中文源目录。

.DESCRIPTION
    跑完之后**必须**打开 .ts 把新出现的条目补上译文（构建设不会因为"没翻"失败，
    没翻的条目英文界面里显示的还是中文）。流程见 docs/dev/I18N.md。

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

function Write-CrowdinSource([string]$TranslationPath, [string]$SourcePath) {
    $catalog = [System.Xml.XmlDocument]::new()
    $catalog.PreserveWhitespace = $true
    $catalog.Load($TranslationPath)
    $catalog.DocumentElement.SetAttribute('language', 'zh_CN')
    $catalog.DocumentElement.SetAttribute('sourcelanguage', 'zh_CN')
    foreach ($translation in $catalog.SelectNodes('/TS/context/message/translation')) {
        $translation.RemoveAll()
        $translation.SetAttribute('type', 'unfinished')
    }

    $format = [System.Xml.XmlWriterSettings]::new()
    $format.Encoding = [System.Text.UTF8Encoding]::new($false)
    $format.NewLineChars = "`n"
    $writer = [System.Xml.XmlWriter]::Create($SourcePath, $format)
    try { $catalog.Save($writer) } finally { $writer.Dispose() }
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
Write-CrowdinSource $ts $crowdinSource

# 没翻的条目数：只报个数，判不判失败交给流程（第 1 期只翻试点那几句）。
$text = [IO.File]::ReadAllText($ts)
$unfinished = ([regex]::Matches($text, 'type="unfinished"')).Count
$total = ([regex]::Matches($text, '<message>')).Count
Write-Host ("已刷新 {0}：{1} 条，其中未翻译 {2} 条。" -f $ts, $total, $unfinished)
exit 0
