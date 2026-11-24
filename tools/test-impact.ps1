#Requires -Version 7.0
[CmdletBinding()]
param([Alias('ChangedPath')][string[]]$ImpactChangedPath,
    [Alias('BaseRef')][string]$ImpactBaseRef = 'origin/main',
    [Alias('Full')][switch]$ImpactFull, [Alias('GitHubOutput')][switch]$ImpactGitHubOutput)

function Get-TestImpact([string[]]$Paths, [bool]$ForceFull = $false) {
    $catalog = [ordered]@{
        core = @('core', 'core-bootstrap', 'core-downloads', 'core-install', 'core-instances', 'core-java', 'core-accounts', 'token-protection-fault',
                 'core-resources', 'core-launch', 'core-servers', 'core-activity', 'core-presets', 'core-toolbox', 'core-agent')
        'core-downloads' = @('core-downloads')
        'core-bootstrap' = @('core-bootstrap')
        'core-install' = @('core-install')
        'core-instances' = @('core-instances')
        'core-java' = @('core-java')
        'core-accounts' = @('core-accounts', 'token-protection-fault')
        'token-protection-fault' = @('token-protection-fault')
        'core-resources' = @('core-resources')
        'core-launch' = @('core-launch')
        'core-servers' = @('core-servers')
        'core-activity' = @('core-activity')
        'core-presets' = @('core-presets')
        'core-toolbox' = @('core-toolbox')
        'core-agent' = @('core-agent')
        'ui-agent' = @('ui-agent')
        'agent-window' = @('main-window-agent')
        runner = @('test-runner', 'test-modules', 'ci-partitions', 'surface-registry')
        ui = @('ui-refresh', 'install-layout', 'lifecycle', 'input-widths', 'window-size', 'files-rows',
               'ui-daily-quote', 'ui-long-instance-name', 'surface-sweep', 'fold-motion', 'loading-state',
               'design-defaults', 'group-editor', 'window-motion', 'launch-card'); theme = @('theme')
        popups = @('popups', 'popups-scale')
        'ui-history' = @('ui-bootstrap', 'ui-language')
        window = @('main-window', 'window-chrome'); workspace = @('workspace', 'activity-selection')
        java = @('java-manage'); mods = @('mod-action-panel'); upgrade = @('instance-upgrade')
        network = @('download-retry', 'high-speed-download', 'segmented-download', 'segment-install')
        presets = @('preset-install'); toolbox = @('tool-features', 'toolbox-polish', 'toolbox-layout', 'armor-trim', 'high-speed-download')
        resources = @('resource-packs'); app = @('network-diag')
    }
    $testSources = @{
        'tests/ui_check/modules/UiInstallLayoutCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiToolboxPolishCheck.cpp' = 'toolbox'
        'tests/ui_check/modules/UiToolboxLayoutCheck.cpp' = 'toolbox'
        'tests/ui_check/modules/UiResourcePacksCheck.cpp' = 'resources'
        'tests/ui_check/modules/UiAgentCheck.cpp' = 'ui-agent'
        'tests/mw_check/modules/MwAgentCheck.cpp' = 'agent-window'
        'tests/ui_check/modules/UiLifecycleCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiWindowMotionCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiLanguageChecks.cpp' = 'ui-history'
        'tests/ui_check/modules/UiInputWidthCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiPopupCheck.cpp' = 'popups'
        'tests/ui_check/modules/UiWindowSizeCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiSurfaceSweepCheck.cpp' = 'ui'
        'tests/ui_check/surface-registry.txt' = 'ui'
        'tests/ui_check/modules/UiUiRefreshCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiThemeCheck.cpp' = 'theme'
        'tests/ui_check/modules/UiWindowChromeCheck.cpp' = 'window'
        'tests/ui_check/chrome_native.cpp' = 'window'
        'tests/mw_check/modules/MwHighSpeedDownloadCheck.cpp' = 'network'
        'tests/mw_check/modules/MwDownloadRetryCheck.cpp' = 'network'
        'tests/mw_check/RequestBudgetChecks.cpp' = 'network'
        'tests/mw_check/modules/PromptProbe.cpp' = 'window'
        'tests/mw_check/modules/MwActivitySelectionCheck.cpp' = 'workspace'
        'tests/mw_check/modules/MwWorkspaceCheck.cpp' = 'workspace'
        'tests/mw_check/JavaManageChecks.cpp' = 'java'
        'tests/mw_check/ModActionPanelChecks.cpp' = 'mods'
        'tests/mw_check/RangeChecks.cpp' = 'network'
        'tests/mw_check/SegmentInstallChecks.cpp' = 'network'
        'tests/mw_check/ToolFeatureChecks.cpp' = 'toolbox'
        'tests/mw_check/PresetInstallChecks.cpp' = 'presets'
        'tests/mw_check/PresetManifestChecks.cpp' = 'presets'
        'tests/mw_check/PresetPreviewChecks.cpp' = 'presets'
        'tests/mw_check/PresetDependencyChecks.cpp' = 'presets'
        'tests/mw_check/InstanceUpgradeChecks.cpp' = 'upgrade'
        'tests/ui_check/ArmorTrimChecks.cpp' = 'toolbox'
        # 主页「每日一言」那一行：挨着主题/字号走，归 ui 那一组。
        'tests/ui_check/modules/UiDailyQuoteCheck.cpp' = 'ui'
        'tests/ui_check/modules/UiLongInstanceNameChecks.cpp' = 'ui'
        'tests/ui_check/modules/UiFilesRowsCheck.cpp' = 'ui'
    }
    $modules = [Collections.Generic.HashSet[string]]::new()
    $reasons = [Collections.Generic.List[string]]::new()
    $all = $ForceFull
    foreach ($raw in $Paths) {
        $path = $raw.Replace('\', '/') -replace '^\./', ''
        if ($path -match '^(docs/|build/|.*\.md$|LICENSE$|\.gitignore$)') { continue }
        if ($path -match '^(CMakeLists\.txt|CMakePresets\.json|\.github/|tests/.*/CMakeLists\.txt|tests/CMakeLists\.txt|src/MainWindow\.(cpp|hpp)|src/main\.cpp|ui/MainWindowUI\.(cpp|hpp))$' -or
            $path -match '^(\.github/|src/(auth|settings|net|launcher|tasks|util)/|tests/main\.cpp$|tests/mw_check/main\.cpp$|tests/ui_check/main\.cpp$)') {
            $all = $true; $reasons.Add("shared: $path"); continue
        }
        if ($path -match '^tests/core_modules/([^/]+)$') {
            $owners = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../tests/core_modules/owners.json') -Raw | ConvertFrom-Json -AsHashtable
            if ($owners.ContainsKey($Matches[1])) { $null = $modules.Add('core-' + $owners[$Matches[1]]); continue }
        }
        if ($path -match '^tests/ui_check/modules/Ui[^/]*Checks\.cpp$') {
            $null = $modules.Add('ui-history'); continue
        }
        if ($testSources.ContainsKey($path)) { $null = $modules.Add($testSources[$path]); continue }
        if ($path -match '^tests/mw_check/modules/[^/]*(Checks|Tests)\.cpp$') {
            $null = $modules.Add('window'); continue
        }
        $selected = switch -Regex ($path) {
            '(?i)VersionDeclaration' { @('core','upgrade'); break }
            '(?i)(ResourceSearchController|InstanceUpdateCheckController)' { @('window','mods','resources'); break }
            '(?i)(InstanceUpdate|InstanceUpgrade|UpgradeAssessment|InstanceTransfer)' { @('upgrade','ui'); break }
            '(?i)(Preset|Modpack)' { @('core','presets','network','mods','ui'); break }
            '(?i)(Theme|ToggleSwitch|MemorySlider|JavaSettings|JavaManage|GlobalJava|EffectiveJava)' { @('core','java','theme','ui','window'); break }
            '(?i)(ArmorTrim|Toolbox|ToolFeature|Banner|Beacon|Enchantment)' { @('core','toolbox','ui'); break }
            '(?i)(TaskCenter|TaskDetail|Activity|Workspace|HomeQuick)' { @('workspace','ui','window'); break }
            '^src/agent/|^src/util/SecretProtector\.' { @('core-agent'); break }
            '(?i)(ModAction|ModDepend|ResourceStore|ResourceCatalog|InstanceFile|ResourcePack)' { @('core','mods','resources','upgrade','presets','ui'); break }
            # 首启资源的已知集合清单与它的生成脚本：改的是 core-resources 那一套断言
            # （清单校验/嵌入字节一致）。不加这一条会落进 default ⇒ 整个 full。
            '^assets/assetdirector/|^tools/assetdirector-knownset\.py$' { @('core-resources'); break }
            '^assets/i18n/' { @('ui-history'); break }
            '^(src/download/|tools/network_diag/)|(?i:Download|RangeChecks|SegmentInstall)' { @('core','network','presets'); break }
            '^(tools/.*\.ps1|tests/test_runner/)' { @('runner'); break }
            '^ui/Agent' { @('ui-agent'); break }
            '^src/controllers/AgentController|^src/controllers/AgentLauncherHostImpl' { @('agent-window','window'); break }
            '^ui/Popups\.(cpp|hpp)$' { @('ui','theme','window','popups'); break }
            '^ui/' { @('ui','theme','window'); break }
            default { @() }
        }
        if ($selected.Count -eq 0) { $all = $true; $reasons.Add("unclassified: $path") }
        foreach ($module in $selected) { $null = $modules.Add($module) }
        if ($path -match '^ui/.*\.hpp$') { $null = $modules.Add('ui-history') }
        if ($path -match '^(src/|ui/|assets/|tools/network_diag/)') { $null = $modules.Add('app') }
    }
    if ($all) { foreach ($key in $catalog.Keys) { $null = $modules.Add($key) } }
    $tests = @($modules | ForEach-Object { $catalog[$_] } | Sort-Object -Unique)
    $suites = [Collections.Generic.List[string]]::new()
    if ($modules.Contains('app')) { $suites.Add('ci-app') }
    if (@($modules | Where-Object { $_ -eq 'core' -or $_ -like 'core-*' -or $_ -eq 'runner' }).Count) { $suites.Add('ci-core') }
    if (@($modules | Where-Object { $_ -in @('ui','ui-history','theme','resources','toolbox','ui-agent','popups') }).Count) { $suites.Add('ci-ui') }
    if (@($modules | Where-Object { $_ -in @('window','agent-window','workspace','java','mods','upgrade','network','presets','toolbox') }).Count) { $suites.Add('ci-integration') }
    [pscustomobject]@{ full = $all; modules = @($modules | Sort-Object); tests = $tests; suites = @($suites.ToArray());
        reasons = @($reasons.ToArray()); paths = @($Paths); testMap = $catalog }
}

function Get-ChangedTestPaths([string]$Base) {
    $repo = Split-Path $PSScriptRoot -Parent
    $paths = @(& git -C $repo diff --no-renames --name-only "$Base...HEAD")
    if ($LASTEXITCODE -ne 0) { throw "Cannot determine changes against $Base; use -Full or a valid -BaseRef." }
    $paths += @(& git -C $repo diff --no-renames --name-only HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate working changes.' }
    $paths += @(& git -C $repo ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate untracked source files.' }
    @($paths | Sort-Object -Unique)
}

if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    if (-not $PSBoundParameters.ContainsKey('ImpactChangedPath')) {
        $ImpactChangedPath = if ($ImpactFull) { @() } else { @(Get-ChangedTestPaths $ImpactBaseRef) }
    }
    $impact = Get-TestImpact $ImpactChangedPath ([bool]$ImpactFull)
    $impact | ConvertTo-Json -Depth 5
    if ($ImpactGitHubOutput) {
        if (-not $env:GITHUB_OUTPUT) { throw 'GITHUB_OUTPUT is not available.' }
        # An empty matrix is invalid even when the job is skipped. Keep one placeholder.
        $matrix = if ($impact.suites.Count) { @($impact.suites) } else { @('ci-core') }
        "suites=$($matrix | ConvertTo-Json -Compress -AsArray)" >> $env:GITHUB_OUTPUT
        $filter = if ($impact.full) { '' } else { $impact.modules -join ',' }
        "modules=$filter" >> $env:GITHUB_OUTPUT
        "run_tests=$([bool]$impact.suites.Count)".ToLowerInvariant() >> $env:GITHUB_OUTPUT
    }
}
