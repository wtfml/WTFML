#Requires -Version 7.0
<#
.SYNOPSIS
    守卫：src/ 与 ui/ 里含中文、却没包进 tr() / translate() / QT_TR_NOOP 之类里的字符串字面量，
    不许比基线多。

.DESCRIPTION
    源语言是中文，所以界面上的每一句中文都得**能被翻译**才会出现在英文界面里。第 2 期之前
    这类字面量还有几千处（见 tools/i18n-baseline.txt），一次清零不现实；这一层守的是
    "新代码不许再加"：逐文件比计数，只要有一个文件比基线多就失败。

    判据（逐条都是"这处文面上的中文能不能被 lupdate 收走"）：
      1. 只看 .cpp / .hpp / .h；
      2. 字符串字面量里含中日韩汉字或全角标点才算；
      3. 字面量外面最近一层还没闭合的 '(' 属于包装调用就不算：tr / translate / QT_TR_NOOP /
         QT_TRANSLATE_NOOP / QT_TRANSLATE_NOOP3 / QT_TRANSLATE_NOOP_UTF8 /
         QT_TR_NOOP_UTF8（`QCoreApplication::translate("ctx", "中文")` 的两个实参都算）；
         外层括号是谁不影响这个判断，所以 `tr("…").arg(QStringLiteral("没包"))` 里的
         「没包」照计；
      4. 注释里的不算；`#include` 后面那个文件名不算；字符字面量 `'('`、普通字符串里的括号、
         原始字符串 R"(…)" 里的括号都不算括号；
      5. 行尾写 `// i18n-ignore` 的那一行整行不算 —— 这是"这处不翻"的唯一标记，
         用途：日志、内部提示词、语言名本身。文档见 docs/dev/I18N.md。

    括号状态跨行维护：从文件开头扫下来，遇到 '(' 就把前面的函数名压栈，')' 弹栈。所以长文案
    按列宽拆成相邻字面量写（`tr("前半"` 换行 `"后半")`）不会被误判成没包。预处理行（#if /
    #define 等）用一份临时栈，指令结束就丢，不平衡的括号不会带偏后面；for 头之外的 `;`
    与行首 `}` 把栈清空兜底。
    认不出的写法（宏拼串、括号不配对）按"没包"算，宁可多报。

.PARAMETER RepoRoot
    仓库根目录。默认是脚本所在目录的上一级。

.PARAMETER UpdateBaseline
    把当前计数写回 tools/i18n-baseline.txt（第 2 期降基线时用），不判失败。

.PARAMETER SelfTest
    在临时目录里造样本，验证上面那几条判据本身（不查真实仓库）。
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [switch]$UpdateBaseline,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$Marker = 'i18n-ignore'
$Wrappers = 'tr', 'translate', 'QT_TR_NOOP', 'QT_TRANSLATE_NOOP', 'QT_TRANSLATE_NOOP3',
    'QT_TRANSLATE_NOOP_UTF8', 'QT_TR_NOOP_UTF8'
# 汉字（基本区 + 扩展 A + 兼容区）与全角标点：这两类都是"得翻"的字。
$CjkPattern = '[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\u3000-\u303f\uff00-\uffef]'

# 逐行扫一个文件，返回"含中文且没包起来"的字面量条数。
function Measure-File([string]$Path) {
    $count = 0
    $stack = [Collections.Generic.List[string]]::new()   # 未闭合的 '(' 各自属于哪个函数
    $ppStack = $null        # 预处理指令自己的栈，指令结束就丢
    $inBlock = $false       # 跨行 /* */
    $rawEnd = $null         # 跨行原始字符串的收尾标记（如 ')x"'）
    $rawText = $null        # 原始字符串已扫过的内容，收尾时判中文
    $specials = [char[]]@('"', "'", '(', ')', '}', ';', '/')
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        $trim = $line.TrimStart()
        if ($trim -match '^#\s*include') { $ppStack = $null; continue }
        if ($trim.StartsWith('#')) { $ppStack = [Collections.Generic.List[string]]::new() }
        $names = $stack
        if ($null -ne $ppStack) { $names = $ppStack }   # 预处理行用自己的栈
        $lineIgnored = $line.Contains($Marker)
        $i = 0
        while ($i -lt $line.Length) {
            if ($null -ne $rawEnd) {
                # 原始字符串：内容整段跳过，括号与引号都不算语法，但中文照样要计
                $j = $line.IndexOf($rawEnd, $i, [StringComparison]::Ordinal)
                if ($j -lt 0) { [void]$rawText.Append($line.Substring($i)); break }
                [void]$rawText.Append($line.Substring($i, $j - $i))
                if (-not $lineIgnored -and $rawText.ToString() -match $CjkPattern) {
                    if ($names.Count -eq 0 -or $Wrappers -notcontains $names[$names.Count - 1]) { $count++ }
                }
                $rawText = $null
                $i = $j + $rawEnd.Length
                $rawEnd = $null
                continue
            }
            if ($inBlock) {
                $j = $line.IndexOf('*/', $i, [StringComparison]::Ordinal)
                if ($j -lt 0) { break }
                $i = $j + 2
                $inBlock = $false
                continue
            }
            $i = $line.IndexOfAny($specials, $i)
            if ($i -lt 0) { break }
            $c = $line[$i]
            if ($c -eq '/') {
                if ($i + 1 -ge $line.Length) { break }
                if ($line[$i + 1] -eq '/') { break }
                if ($line[$i + 1] -eq '*') {
                    $j = $line.IndexOf('*/', $i + 2, [StringComparison]::Ordinal)
                    if ($j -lt 0) { $inBlock = $true; break }
                    $i = $j + 2
                    continue
                }
                $i++
                continue
            }
            if ($c -eq '"') {
                # 原始字符串 R"(…)"：里面的括号和引号都不是语法
                if ($i -gt 0 -and $line[$i - 1] -eq 'R') {
                    $m = [regex]::Match($line.Substring($i), '^"([^()\\]{0,16})\(')
                    if ($m.Success) {
                        $rawEnd = ')' + $m.Groups[1].Value + '"'
                        $rawText = [Text.StringBuilder]::new()
                        $i += $m.Length
                        continue
                    }
                }
                $j = $i + 1
                while ($j -lt $line.Length -and $line[$j] -ne '"') {
                    if ($line[$j] -eq '\') { $j++ }
                    $j++
                }
                if ($j -gt $line.Length) { $j = $line.Length }
                $text = $line.Substring($i + 1, $j - $i - 1)
                if (-not $lineIgnored -and $text -match $CjkPattern) {
                    if ($names.Count -eq 0 -or $Wrappers -notcontains $names[$names.Count - 1]) { $count++ }
                }
                $i = $j + 1
                continue
            }
            if ($c -eq "'") {
                # 数字分隔符 1'000 不是字符字面量
                if ($i -gt 0 -and $line[$i - 1] -match '\w') { $i++; continue }
                $j = $i + 1
                while ($j -lt $line.Length -and $line[$j] -ne "'") {
                    if ($line[$j] -eq '\') { $j++ }
                    $j++
                }
                if ($j -gt $line.Length) { $j = $line.Length }
                $i = $j + 1
                continue
            }
            if ($c -eq '(') {
                $j = $i - 1
                while ($j -ge 0 -and [char]::IsWhiteSpace($line[$j])) { $j-- }
                $k = $j
                while ($k -ge 0 -and $line[$k] -match '\w') { $k-- }
                $name = ''
                if ($j -gt $k) { $name = $line.Substring($k + 1, $j - $k) }
                $names.Add($name)
                $i++
                continue
            }
            if ($c -eq ')') {
                if ($names.Count -gt 0) { $names.RemoveAt($names.Count - 1) }
                $i++
                continue
            }
            if ($c -eq ';') {
                # for 头里的分号不是语句结束
                if ($names.Count -gt 0 -and $names[$names.Count - 1] -ne 'for') { $names.Clear() }
                $i++
                continue
            }
            # '}'：行首右括号处栈还非空，说明括号没配上，清空兜底（宁可多报）
            if ($names.Count -gt 0 -and -not $line.Substring(0, $i).Trim()) { $names.Clear() }
            $i++
        }
        if ($null -ne $ppStack -and -not $line.TrimEnd().EndsWith('\')) { $ppStack = $null }
    }
    return $count
}

function Get-Counts([string]$Root) {
    $files = Get-ChildItem -LiteralPath $Root -Recurse -File -Include '*.cpp', '*.hpp', '*.h' |
        Where-Object { $_.FullName -notmatch '[\\/](build|_tmp|_archive)[\\/]' }
    $map = @{}
    foreach ($file in $files) {
        $text = [IO.File]::ReadAllText($file.FullName)
        if ($text -notmatch $CjkPattern) { continue }
        $n = Measure-File $file.FullName
        if ($n -gt 0) {
            $rel = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace([char]92, [char]47)
            $map[$rel] = $n
        }
    }
    # 只守 src/ 与 ui/：别的目录（tests、tools）不在此列。
    $filtered = @{}
    foreach ($key in $map.Keys) {
        if ($key -like 'src/*' -or $key -like 'ui/*') { $filtered[$key] = $map[$key] }
    }
    return $filtered
}

if ($SelfTest) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('i18n-selftest-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $tmp 'src') -Force | Out-Null
    $sample = Join-Path $tmp 'src/sample.cpp'
    @'
// 注释里的中文不算
/* 块注释
   里的中文也不算 */
#include "中文头文件.hpp"
#define BROKEN( tr("宏里（没闭合"
void f() {
    tr("要翻");
    QCoreApplication::translate("Ctx", "也要翻");
    QT_TR_NOOP("照样翻");
    QT_TRANSLATE_NOOP("Ctx", "两个实参都算");
    auto a = QStringLiteral("没包");          // 计 1
    auto b = QStringLiteral("不翻这处");      // i18n-ignore
    auto c = tr("包了 %1").arg(QStringLiteral("还是没包"));  // 「还是没包」计 1
    tr("前半句"
       "后半句");
    QCoreApplication::translate("Ctx",
                               "第二个实参");
    auto d = QStringLiteral("上一行"
                            "下一行");        // 两段都计 2
    auto e = tr("括号在串里（%1）(") + "裸串";   // 「裸串」计 1
    auto g = ('(' == '(') ? "字符括号后" : "x";  // 「字符括号后」计 1
    // ( 注释里的左括号不算
    auto h = "注释括号后";                        // 计 1
    auto k = QStringLiteral("续行第一"
                            "续行被忽略");  // i18n-ignore → 只计上一行那段 1
    auto i = tr("跨行 %1"
                "跨行后半").arg(QStringLiteral("裸串二"));  // 计 1
    auto r = QStringLiteral(R"(原始串里的（括号)(不算括号）");  // 计 1：R"(…) 整段是一个字面量
    name.remove(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")));  // 计 0：原始串里的引号不能把后面扫歪
}
#define MSG tr("宏里 %1" \
               "续行")
void g() {
    auto m = "宏后裸串";   // 计 1：宏里那个没闭合的 tr( 不该带偏这里
}
'@ | Set-Content -LiteralPath $sample -Encoding utf8
    $got = (Get-Counts $tmp)['src/sample.cpp']
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    if ($got -ne 11) { Write-Host "FAIL 自测：期望 11 处未包，实际 $got"; exit 1 }
    Write-Host 'PASS 自测：注释 / include / 包装调用 / 不翻标记都不计；跨行字面量、字符串与字符字面量里的括号、原始字符串、预处理行不平衡括号都不误判；裸字面量计 11'
    exit 0
}

if (-not $RepoRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$baselinePath = Join-Path $PSScriptRoot 'i18n-baseline.txt'
$now = Get-Counts $RepoRoot

if ($UpdateBaseline) {
    $lines = $now.Keys | Sort-Object | ForEach-Object { "$_`t$($now[$_])" }
    [IO.File]::WriteAllLines($baselinePath, $lines)
    $sum = ($now.Values | Measure-Object -Sum).Sum
    Write-Host ("基线已写回：{0} 个文件、{1} 处未包中文" -f $now.Count, $sum)
    exit 0
}

$baseline = @{}
if (Test-Path -LiteralPath $baselinePath) {
    foreach ($line in [IO.File]::ReadAllLines($baselinePath)) {
        if (-not $line.Trim()) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -eq 2) { $baseline[$parts[0]] = [int]$parts[1] }
    }
}

$bad = [Collections.Generic.List[string]]::new()
foreach ($file in $now.Keys) {
    $allowed = if ($baseline.ContainsKey($file)) { $baseline[$file] } else { 0 }
    if ($now[$file] -gt $allowed) {
        $bad.Add("$file  未包中文 $($now[$file]) 处（基线 $allowed）")
    }
}
$total = ($now.Values | Measure-Object -Sum).Sum
$baseTotal = ($baseline.Values | Measure-Object -Sum).Sum
Write-Host ("未包中文：{0} 个文件、{1} 处（基线 {2} 处）" -f $now.Count, $total, $baseTotal)
if ($bad.Count -gt 0) {
    Write-Host '比基线多（新代码里的中文要包 tr()，或行尾标 // i18n-ignore）：'
    $bad | Sort-Object | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'PASS 没有新增未包中文'
exit 0
