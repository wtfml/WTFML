#Requires -Version 7.0
<#
.SYNOPSIS
    智能体发布前检查：明文密钥、测试假端点、调试后门、危险开关默认值、确认闸存在性。

.DESCRIPTION
    对应 docs/features/AGENT.md §12/§13.3 的发布检查项。只读检查，不改任何文件：

    1. 产品源码（src/、ui/）里不许出现明文 API Key / 令牌字面量（`sk-…`、`Bearer <长串>`、
       测试用的假 key），tests/ 与 docs/ 不算；
    2. 产品源码里不许出现测试假端点（example.com / localhost:9 / 127.0.0.1 之类的默认地址）；
    3. 技能提示词必须放在 `assets/agent/prompts/`、登记进 `agent.qrc`、不带 BOM，
       `AgentSkill.cpp` 里不许再出现提示词正文；内嵌知识（`assets/agent/knowledge/`）
       同样要登记、是有效 JSON、规则字段齐；
    4. `command.run`（任意可执行文件）不许出现在工具注册表里；
    5. `command.diagnose`、MCP、联网工具都必须有审批/确认闸的调用点；
    6. 诊断命令与 MCP 服务器的默认值必须是关闭/未启用。

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

# --- 3. 技能提示词：外置、可加载、无正文回流 ---------------------------------
$promptDir = Join-Path $repo 'assets/agent/prompts'
$promptQrc = Join-Path $repo 'assets/agent/agent.qrc'
$requiredPrompts = @('common.md', 'general.md', 'inspection.md', 'crash_rescue.md',
                     'upgrade.md', 'mods.md', 'repair_protocol.md', 'README.md')
if (-not (Test-Path -LiteralPath $promptDir)) {
    $violations.Add('找不到 assets/agent/prompts（技能提示词必须外置）')
} else {
    foreach ($name in $requiredPrompts) {
        if (-not (Test-Path -LiteralPath (Join-Path $promptDir $name))) {
            $violations.Add("缺少技能提示词：assets/agent/prompts/$name")
        }
    }
}
if (-not (Test-Path -LiteralPath $promptQrc)) {
    $violations.Add('找不到 assets/agent/agent.qrc（提示词资源清单）')
} else {
    $qrcText = Get-Content -LiteralPath $promptQrc -Raw
    if ($qrcText -notmatch 'prefix="/agent"') {
        $violations.Add('agent.qrc 的资源前缀不是 /agent')
    }
    if (Test-Path -LiteralPath $promptDir) {
        Get-ChildItem -LiteralPath $promptDir -File -Filter *.md | ForEach-Object {
            if ($qrcText -notmatch [regex]::Escape("prompts/$($_.Name)")) {
                $violations.Add("提示词没有登记进 agent.qrc：$($_.Name)")
            }
            $bytes = [IO.File]::ReadAllBytes($_.FullName)
            if ($bytes.Length -eq 0) {
                $violations.Add("技能提示词为空：$($_.Name)")
            }
            if ($bytes.Length -gt 65536) {
                $violations.Add("技能提示词超过 64 KB：$($_.Name)")
            }
            if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
                $violations.Add("技能提示词带 UTF-8 BOM：$($_.Name)")
            }
            foreach ($line in (Get-Content -LiteralPath $_.FullName)) {
                foreach ($pattern in $secretPatterns) {
                    if ($line -match $pattern) {
                        $violations.Add("技能提示词里出现密钥形态：$($_.Name)")
                    }
                }
            }
        }
    }
}
$skillSource = Join-Path $repo 'src/agent/AgentSkill.cpp'
if (Test-Path -LiteralPath $skillSource) {
    $text = Get-Content -LiteralPath $skillSource -Raw
    if ($text -match 'kCommonRules|systemPrompt\s*=\s*QStringLiteral|你在做|你是 WTFML 启动器内置') {
        $violations.Add('AgentSkill.cpp 里仍有提示词正文（应放在 assets/agent/prompts）')
    }
} else {
    $violations.Add('找不到 src/agent/AgentSkill.cpp')
}
$promptLibrary = Join-Path $repo 'src/agent/AgentPromptLibrary.cpp'
if (Test-Path -LiteralPath $promptLibrary) {
    $text = Get-Content -LiteralPath $promptLibrary -Raw
    if ($text -notmatch 'Q_INIT_RESOURCE\(agent\)') {
        $violations.Add('AgentPromptLibrary.cpp 缺少 Q_INIT_RESOURCE(agent)（静态库资源不会初始化）')
    }
} else {
    $violations.Add('找不到 src/agent/AgentPromptLibrary.cpp')
}

# --- 3b. 内嵌诊断知识（S5.3）：存在、登记、JSON 可解析、规则字段齐 ----------------
$knowledgeDir = Join-Path $repo 'assets/agent/knowledge'
$knowledgeQrc = Join-Path $repo 'assets/agent/agent.qrc'
$requiredKnowledge = @('crash_rules.json', 'launch_milestones.json')
if (-not (Test-Path -LiteralPath $knowledgeDir)) {
    $violations.Add('找不到 assets/agent/knowledge（崩溃规则与里程碑标记必须外置成数据）')
} else {
    $qrcKnowledge = if (Test-Path -LiteralPath $knowledgeQrc) {
        Get-Content -LiteralPath $knowledgeQrc -Raw
    } else { '' }
    foreach ($name in $requiredKnowledge) {
        $path = Join-Path $knowledgeDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            $violations.Add("缺少内嵌知识文件：assets/agent/knowledge/$name")
            continue
        }
        if ($qrcKnowledge -notmatch [regex]::Escape("knowledge/$name")) {
            $violations.Add("内嵌知识没有登记进 agent.qrc：$name")
        }
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -eq 0) { $violations.Add("内嵌知识文件为空：$name") }
        if ($bytes.Length -gt 262144) { $violations.Add("内嵌知识文件超过 256 KB：$name") }
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $violations.Add("内嵌知识文件带 UTF-8 BOM：$name")
        }
        try {
            $data = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
        } catch {
            $violations.Add("内嵌知识文件不是有效 JSON：$name")
            continue
        }
        if ($name -eq 'crash_rules.json') {
            $ids = @{}
            $allowedPhases = @('startup', 'mod_initialization', 'config_load', 'world_load',
                               'runtime', 'native', 'shutdown')
            foreach ($rule in @($data.rules)) {
                $id = [string]$rule.id
                if ($id -notmatch '^[A-Z][A-Z0-9_]{2,31}$') {
                    $violations.Add("崩溃规则 id 不合法：$id")
                    continue
                }
                if ($ids.ContainsKey($id)) { $violations.Add("崩溃规则 id 重复：$id") }
                $ids[$id] = 1
                if ($allowedPhases -notcontains [string]$rule.phase) {
                    $violations.Add("崩溃规则 $id 的 phase 不认识：$($rule.phase)")
                }
                if (@($rule.match).Count -eq 0) { $violations.Add("崩溃规则 $id 没有 match") }
                if (-not $rule.ContainsKey('confidence')) {
                    $violations.Add("崩溃规则 $id 没有 confidence")
                }
            }
            if ($ids.Count -lt 10) { $violations.Add("崩溃规则少于 10 条，覆盖不够：$($ids.Count)") }
        } else {
            foreach ($key in @('main_menu', 'world_loaded', 'crash')) {
                if (-not $data.milestones.ContainsKey($key) -or @($data.milestones[$key]).Count -eq 0) {
                    $violations.Add("里程碑缺少标记：$key")
                }
            }
        }
    }
}

# --- 4. `command.run` 不许出现在产品里 ----------------------------------------
$registry = Join-Path $repo 'src/agent/AgentToolRegistry.cpp'
if (Test-Path -LiteralPath $registry) {
    $text = Get-Content -LiteralPath $registry -Raw
    if ($text -match 'command\.run') {
        $violations.Add('工具注册表里出现了 command.run（任意可执行文件未开放）')
    }
} else {
    $violations.Add('找不到 src/agent/AgentToolRegistry.cpp')
}

# --- 5. 审批/确认闸的调用点必须还在 -------------------------------------------
$gateChecks = @(
    @{ File = 'src/agent/AgentLauncherTools.cpp'; Pattern = 'AgentApprovalPolicy::request'; What = '启动器工具审批闸' },
    @{ File = 'src/agent/AgentTestLaunchToolEngine.cpp'; Pattern = 'AgentApprovalPolicy::request'; What = '测试启动审批闸' },
    @{ File = 'src/agent/AgentModToolEngine.cpp';       Pattern = 'AgentApprovalPolicy::request'; What = '模组工具审批闸' },
    @{ File = 'src/agent/AgentModToolEngine.cpp';       Pattern = 'loadByWorkspace';              What = '模组修复只认事务工作区' },
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

# --- 6. 危险开关默认值 --------------------------------------------------------
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
