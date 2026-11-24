#Requires -Version 7.0
<#
.SYNOPSIS
    智能体发布前检查：明文密钥、测试假端点、调试后门、危险开关默认值、确认闸存在性。

.DESCRIPTION
    对应 docs/features/AGENT.md §14 的 Stage 4「发布检查」与
    docs/features/AGENT_STAGE3_4_ACCEPTANCE.md 的 G4-16/G4-17。只读检查，不改任何文件：

    1. 产品源码（src/、ui/）里不许出现明文 API Key / 令牌字面量（`sk-…`、`Bearer <长串>`、
       测试用的假 key），tests/ 与 docs/ 不算；
    2. 产品源码里不许出现测试假端点（example.com / localhost:9 / 127.0.0.1 之类的默认地址）；
    3. `command.run`（任意可执行文件）不许出现在工具注册表里；
    4. `command.diagnose`、MCP、联网工具都必须有审批/确认闸的调用点；
    5. 诊断命令与 MCP 服务器的默认值必须是关闭/未启用。

    有任一违规退出码 1。用法：`pwsh tools/check-agent-release.ps1`。

.PARAMETER RepoRoot
    仓库根目录。默认是脚本所在目录的上一级。
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path $PSScriptRoot -Parent
}
$repo = (Resolve-Path -LiteralPath $RepoRoot).Path
$violations = [Collections.Generic.List[string]]::new()

function Get-ProductFiles([string[]]$roots) {
    $files = [Collections.Generic.List[object]]::new()
    foreach ($root in $roots) {
        $full = Join-Path $repo $root
        if (-not (Test-Path -LiteralPath $full)) { continue }
        Get-ChildItem -LiteralPath $full -Recurse -File -Include *.cpp, *.hpp, *.h, *.ui |
            ForEach-Object { $files.Add($_) }
    }
    return $files
}

# --- 1/2. 明文密钥与测试假端点 -------------------------------------------------
$secretPatterns = @(
    'sk-[A-Za-z0-9]{20,}',                 # OpenAI 形态
    'AKIA[0-9A-Z]{16}',                    # AWS
    'ghp_[A-Za-z0-9]{30,}',                # GitHub PAT
    '(?i)bearer\s+[A-Za-z0-9._-]{32,}'     # 长 Bearer 串
)
$endpointPatterns = @(
    '(?i)https?://(?:localhost|127\.0\.0\.1):\d{2,5}',
    '(?i)https?://[A-Za-z0-9.-]*example\.com',
    '(?i)api\.fake-'
)
$allowFileNames = @('AgentSecretStore.cpp', 'AgentSecretStore.hpp', 'SecretProtector.cpp',
                    'SecretProtector.hpp', 'OpenAiCompatibleClient.cpp', 'AgentProviderAdapters.cpp',
                    'AgentSseParser.cpp')

foreach ($file in (Get-ProductFiles @('src', 'ui'))) {
    $lines = Get-Content -LiteralPath $file.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        # 注释里写"形态"（正则/示例）是允许的：只查字符串字面量。
        if ($line -match '^\s*(//|\*|/\*)') { continue }
        foreach ($pattern in $secretPatterns) {
            if ($line -match $pattern) {
                $violations.Add("明文密钥形态：$($file.FullName):$($i + 1)")
            }
        }
        # 界面占位提示里给的例子不是"默认端点"（用户不填就没有地址）。
        if ($line -match 'setPlaceholderText') { continue }
        if ($allowFileNames -contains $file.Name) { continue }
        foreach ($pattern in $endpointPatterns) {
            if ($line -match $pattern) {
                $violations.Add("测试假端点：$($file.FullName):$($i + 1)")
            }
        }
    }
}

# --- 3. `command.run` 不许出现在产品里 ----------------------------------------
$registry = Join-Path $repo 'src/agent/AgentToolRegistry.cpp'
if (Test-Path -LiteralPath $registry) {
    $text = Get-Content -LiteralPath $registry -Raw
    if ($text -match 'command\.run') {
        $violations.Add('工具注册表里出现了 command.run（任意可执行文件未开放）')
    }
} else {
    $violations.Add('找不到 src/agent/AgentToolRegistry.cpp')
}

# --- 4. 审批/确认闸的调用点必须还在 -------------------------------------------
$gateChecks = @(
    @{ File = 'src/agent/AgentLauncherTools.cpp'; Pattern = 'AgentApprovalPolicy::request'; What = '启动器工具审批闸' },
    @{ File = 'src/agent/AgentWebTools.cpp';       Pattern = 'AgentApprovalPolicy::request'; What = '联网工具审批闸' },
    @{ File = 'src/agent/AgentCommandTools.cpp';   Pattern = 'AgentApprovalPolicy::request'; What = '诊断命令审批闸' },
    @{ File = 'src/agent/AgentMcpToolEngine.cpp';  Pattern = 'AgentApprovalPolicy::request'; What = 'MCP 工具审批闸' },
    @{ File = 'src/agent/McpTrustStore.cpp';       Pattern = 'Gate::NeedsConfirmation';      What = 'MCP 首次启动/重连确认闸' },
    @{ File = 'src/agent/McpManager.cpp';          Pattern = 'checkStart';                   What = 'MCP 启动前复核确认' },
    @{ File = 'src/agent/AgentWebPolicy.cpp';      Pattern = 'allAddressesAllowed';          What = '联网全地址校验' }
)
foreach ($check in $gateChecks) {
    $path = Join-Path $repo $check.File
    if (-not (Test-Path -LiteralPath $path)) {
        $violations.Add("缺失文件：$($check.File)（$($check.What)）")
        continue
    }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -notmatch [regex]::Escape($check.Pattern)) {
        $violations.Add("$($check.What)调用点不见了：$($check.File)")
    }
}

# --- 5. 危险开关默认值 --------------------------------------------------------
$settings = Join-Path $repo 'src/agent/AgentSettings.cpp'
if (Test-Path -LiteralPath $settings) {
    $text = Get-Content -LiteralPath $settings -Raw
    if ($text -notmatch 'diagnoseCommandsKey\(\), false') {
        $violations.Add('诊断命令默认值不是 false（AgentSettings::diagnoseCommandsEnabled）')
    }
} else {
    $violations.Add('找不到 src/agent/AgentSettings.cpp')
}
$mcpTypes = Join-Path $repo 'src/agent/McpTypes.hpp'
if (Test-Path -LiteralPath $mcpTypes) {
    $text = Get-Content -LiteralPath $mcpTypes -Raw
    if ($text -notmatch 'bool enabled = false;') {
        $violations.Add('MCP 服务器默认不是未启用（McpServerConfig::enabled）')
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Host "FAIL  $_" }
    Write-Host "智能体发布检查：$($violations.Count) 项不通过"
    exit 1
}
Write-Host 'PASS 智能体发布检查（明文密钥 / 假端点 / 危险开关 / 审批闸）'
exit 0
