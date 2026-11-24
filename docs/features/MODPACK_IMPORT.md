# 整合包导入

给改整合包导入、MCLib AssetDirector 预取、缺项补齐的人看：支持哪些包、安全边界、GTNH 首启资源怎么预取、缺项报告与安装凭据的规则，以及已验过的样本。

入口为管理/下载页的「导入整合包」，或拖放 ZIP / MRPACK。导入创建或恢复自己的未完成实例，不覆盖现有正常实例。

## 1. 支持范围

- **Modrinth**：`modrinth.index.json`、下载文件和 `overrides` / `client-overrides`，客户端覆盖层后应用。
- **CurseForge**：`manifest.json`、项目/文件 ID 解析与指定覆盖目录；平台明确不可用及未返回的文件单独说明。FTB 的 `manifest.json`（`manifestVersion` / `files[]` 的 projectID+fileID / `overrides` / `minecraft.modLoaders`）与 CurseForge 同一套骨架，走同一入口；Technic 等专用清单没有独立适配器。
- **简单 MultiMC / Prism**：`mmc-pack.json`、`instance.cfg` 与游戏目录；明确 Minecraft 和受支持加载器版本后重新安装核心。组件白名单为 `net.minecraft`、`org.lwjgl` 和 Forge/Fabric/NeoForge/Quilt（Quilt 自 2.1.4 起与 Fabric 同一条 profile + 库安装路径，可导入、可导出清单）。有完整本地声明式 patches 的包可转换额外组件（§2），缺补丁时不猜专用启动配置。
- **PCL 等包装 ZIP**：内含一份 MRPACK 时解出后按同一流程导入，不运行随包附带的启动器或脚本。
- **简单懒人包**：直接游戏目录、单层包装目录，或启动器旁的 `.minecraft`；必须有可确认的版本及加载器版本。游戏目录导入跳过 versions、libraries、assets、日志及已知启动器账号文件；核心及加载器走现有安装器。

**格式与范围限制**：单个模组、纯 JAR 模组合集、世界存档及服务端包不支持按整合包导入。多个候选实例需用户明确指定，缺失版本信息时拒绝安装。压缩包内含两个及以上各带 `.minecraft/` 的顶层目录时拒绝导入，需解压后分别导入。多实例、依赖自定义脚本或特殊隔离布局的非标准结构暂不自动转换。

## 2. 安全边界

- **声明式启动补丁（GTNH 等）**：支持清单组件对应的本地 patches：按 order 排序，检查身份、版本、依赖/冲突及 Java 版本交集，转换主类、官方 mainJar、资源索引、库、`+tweakers` 与受限 `+jvmArgs`；未知字段/操作符明确拒绝。补丁客户端须与刚获取的官方原版 SHA-1 一致，不安装重新打包的 Minecraft 客户端。缺失本地组件补丁、未实现操作符及不支持的 JVM 参数仍拒绝。当前支持常用标准补丁结构。
- **GTNH 2.8.4 Java 17–25 包**：RFB 主类、专用 ClassLoader、forgePatches 本地库和模块参数已接通；LWJGL3 原生库翻译为标准 Maven classifier 及明确 CPU 架构，防止 x86 DLL 混入 x64。包内库校验后原子写入 libraries，同名内容不同则拒绝覆盖；断点身份包含补丁与本地库内容摘要。原版客户端与运行库沿用生产下载器；自定义运行配置单独保存，不继承会重新带入 LWJGL2 的原版库数组。
- **不执行**：不启动来源启动器，不执行 `instance.cfg` 的前后置命令、脚本、javaagent 或任意 JVM 参数；不继承来源启动器的账号和任意 Java 路径。压缩包路径检查、覆盖文件排除及目录锁仍生效。
- **启动目录**：隔离实例的进程工作目录与 `--gameDir` 一致（含 Java 8 的 ASCII 别名），共享库和资源仍用绝对路径。旧共享根目录下由模组生成的 config 等文件不会被删，也不自动覆盖实例内配置。现代 Forge/NeoForge 的 BootstrapLauncher `ignoreList` 补上继承链中实际客户端 JAR 文件名，避免实例名与原版 JAR 名不同导致重复 Minecraft 模块；不改实例身份、不从 classpath 删客户端资源。分段探测返回 404 时尝试一次完整 GET，仍检查 HTTP 状态、大小与 SHA-1。
- **CurseForge 下载候选**：文件 API 的 `downloadUrl` 为空时，用文件 ID 和安全的单个文件名构造官方 CDN 候选，整合包导入与普通资源页共用规则；文件名独立百分号编码。`isAvailable` 明确为 false 时不构造候选；空链接不单独标成"作者禁止第三方分发"。限制：构造候选不等于下载成功，实际仍由生产下载器检查 HTTP 状态、大小和已知 SHA-1；技术可下载不等于取得再分发许可（如项目 1121856 的 `allowModDistribution=false` 示例）。
- **分发**：适配器自行解析格式，不嵌入第三方启动器或复制其实现。用户提供包，启动器只在本机转换并安装；发布包/仓库不含 GTNH、Minecraft 客户端及模组二进制。不要重新打包分发 Mojang 文件或修改版客户端；第三方模组/库遵守各自许可。若将来捆绑、修改或再分发 LGPL 等组件，需另行审核署名、许可和源码义务。格式依据：[Modrinth 规范](https://support.modrinth.com/en/articles/8802351-modrinth-modpack-format-mrpack)、[GTNH LWJGL3ify 启动说明](https://github.com/GTNewHorizons/lwjgl3ify#client)。

## 3. MCLib AssetDirector 首启资源预取（GTNH）

GTNH 2.8.4 首次启动会停在 MCLib 的 AssetDirector（AD）资源下载（包内 Et Futurum Requiem 2.6.2.25-GTNH 把 MCLib 0.3.7.7 打进 jar）。启动器在导入成功后按"已知集合"预取这些资源。

### 3.1 AD 要什么、怎么下、写在哪

依据 MCLib 0.3.7.7 的 `AssetDirector.java` / `AssetFetcher.java` / `ADConfig.java`（与包内版本一致；0.3.7.8、0.4 另有增补）。

- 请求集合**来自模组代码**：模组在 FMLConstructionEvent 里注册 `ADConfig`（`addObject` / `addSoundEvent` / `addJar`），资产名与声音事件名是 Java 字面量，MCLib 只按 modid 存内存（`AssetDirectorAPI.jsons`），**不落盘，也不在包清单里**。
- 解析：对每个声明的 MC 版本下载版本 JSON（launchermeta）→ 取 `assetIndex.url` → 用索引 name→hash 表。`addObject` 的名字原样查表；`addSoundEvent` 先取索引里的 `minecraft/sounds.json`，把事件展开成 `minecraft/sounds/<name>.ogg`。查不到的名字、sounds.json 里没有的事件只记 warn，不算失败。
- 下载：对象走 `https://resources.download.minecraft.net/<hash[0:2]>/<hash>`，每次尝试后校验 SHA-1（3 次尝试、连接/读取各 10 秒超时、`.part` 临时文件后改名）；版本 JSON 与资源索引 JSON 各存自己一份，**不走 overlay 复用**，缺了自己下。0.3.7.7 是**单线程**逐个下，大清单会长时间停在进度条上；并发与 `-DassetDirector.downloadThreads` 是 0.4 才有。
- 目录：`-Dminecraft.sharedDataDir/asset_director` → `$MINECRAFT_SHARED_DATA_DIR/asset_director` → 否则 `<--assetsDir>/asset_director`（只有老官方启动器才退回 `<assets>/../asset_director`）。本启动器传 `--assetsDir <数据目录>/assets`，不传 `minecraft.sharedDataDir`（补丁 `+jvmArgs` 白名单：两个固定 `-D`、`--enable-native-access` 和受限的 `--add-opens` / `--add-exports`），数据根的 `launcher_profiles.json` 也没有 `launcherVersion` 段，所以落点是 `<数据目录>/assets/asset_director`，**所有实例共用一份**（按实例隔离只能用 `MINECRAFT_SHARED_DATA_DIR` 环境变量，那是另一条产品改动）。
- 信任边界：读对象时先找 `<ad>/assets/objects`，再找启动器的 `<assets>/objects`，只写 `<ad>`。**AD 只判"文件存在"，不重算哈希**；索引/版本 JSON 在 0.3.7.7 里损坏后没有重取路径（0.3.7.8 才加）。所以预取方必须自己按官方大小/SHA-1 校验后再发布，否则一个坏对象会被长期当成好的用。

### 3.2 请求集合怎么得到

| 来源 | 能否得到 | 依据 |
| --- | --- | --- |
| 包内声明 | 不能 | MultiMC patches 只声明 `net.minecraft` 的 `assetIndex`（1.7.10）；ADConfig 在模组 jar 里运行时才注册 |
| 资源索引 | 只有"某 MC 版本的全集" | 1.21 索引 3911 个对象/787.8 MiB，只取 sounds/ 也有 3727 个/686.5 MiB；取哪个版本的索引同样来自模组代码 |
| 已校验缓存 | 能 | 一次成功启动后 `<ad>` 里就是实际集合，逐项按官方索引 size/SHA-1 复核即可固定成"已知集合" |
| 模组源码 | 只有**下界** | EFR 的 `addObject` / `addSoundEvent` 字面量可离线算，但 `getSounds()` 末尾按方块注册表循环动态注册 step/place/break 声音事件，静态正则看不见 |

结论：没有"只靠包清单、离线、通用"的推导路径；做法是**按整合包＋模组版本固定一份已知集合清单**，每项用官方 HTTPS 索引的大小/SHA-1 校验，再由启动器预取。清单权威来源是**跑过一次的 AD 目录**，静态解析只作交叉核对（2026-10-26 实测静态集合比真实集合少 612 个对象）。

### 3.3 体积（2026-10-26）

| 项目 | GTNH 2.8.4（EFR 2.6.2.25-GTNH，资产 1.21，索引 id 17） | 2.9.0-RC-2 侧（EFR 2.6.60-GTNH，资产 1.21.10，索引 id 27） |
| --- | --- | --- |
| 静态解析：声明 sound event / 对象 | 243 / 6 | 250 / 6 |
| 静态解析：去重对象 | 755 个，115,427,181 B（110.08 MiB） | 763 个，59,315,112 B（56.57 MiB） |
| 索引里查不到的 | 0 | 0（2 个事件在 sounds.json 不存在，只 warn） |
| 已在 1.7.10 索引里的 | 12 个 / 10.3 MiB | 0 |
| 加 sounds.json / 再加版本 JSON＋索引 JSON（冷启动总量） | 110.57 MiB / **111.04 MiB**（116,429,155 B） | 57.11 MiB / 57.63 MiB（60,432,550 B） |

真机实际集合：GTNH 2.8.4 是 **1368 个对象 / 128,399,283 B（122.45 MiB）**，全部能在索引 17 里按名字、大小、SHA-1 核对；加两份 JSON 后冷启动总量 122.92 MiB；减去能从 1.7.10 资产复用的 12 个（10.3 MiB），实际下载 112.6 MiB。1.21 索引共 3911 个对象 826,115,023 B（787.8 MiB），1.21.10 共 4403 个 449,113,996 B；整索引是已知集合的 6–7 倍，故不采用。本包 1.7.10 资产索引（686 个对象 112,396,854 B，sha1 `1863782e…`）与已知集合只有 12 个哈希相同，同名 ogg 哈希全不同（重编码过），所以复用已有 `assets/objects` 在这套包上只省 10.3 MiB。（一次"本机有就铺一遍"的 3213 个对象夹具补法与这份清单无关。）

### 3.4 预取行为（2.1.6b）

边界：接受"已知集合"方案、允许自动下载、只写启动器管理的 overlay。

- **何时跑**：整合包导入成功之后（`MainWindow::maybePrefetchAssetDirector()`；"导入完成"提示关掉后才开始）。不挂在启动前路径上：启动是同步的且含嵌套事件循环，塞入几十 MB 下载会把取消/关窗/重复点击的闸全部重排。
- **认不认实例**：只看 `versions/<实例>/mods/` 里的**文件名**，按清单里的正则（目前只有 `Et-Futurum-Requiem-2.6.2.25-GTNH.jar` 及其小写/下划线别名）。认不出就什么都不做（不建任务、不置忙、不建目录），不读 jar 的 `mcmod.info`，不"看起来像就猜"。其它 GTNH 版本、其它 MCLib 消费者要各自新增一套。
- **下什么、跳什么**：对象先看 `<数据目录>/assets/asset_director/assets/objects/<xx>/<hash>`（AD 自己的目录），再看共享的 `<数据目录>/assets/objects/<xx>/<hash>`；命中都要过"字节数 + SHA-1"，只判存在不算数。两份 JSON 也落在 AD 读的两条路径上并同样校验。
- **写在哪**：只写 `<数据目录>/assets/asset_director/` 下：
  ```
  asset_director/versions/1.21/1.21.json
  asset_director/assets/indexes/17.json
  asset_director/assets/objects/<hash 前两位>/<hash>
  ```
  不写共享 `assets/objects`，不写游戏目录，不改模组文件。写入走 `DownloadManager`：字节数 + SHA-1 校验、失败重试与看门狗、`QSaveFile` 原子提交；批次前后取数据根生命周期锁，拿不到就一个字节不写。
- **体积上限**：单套已知集合 256 MiB（`AssetDirectorKnownSet::kMaxKnownSetBytes`），超了不下，界面直说是哪套、多大。GTNH 2.8.4 实际 122.45 MiB（冷启动下 112.6 MiB）。
- **进度/取消/重试**：公共下载批次，任务中心一条「预取 <实例> 的首启资源」（可取消），底栏进度报真要下的那几个（`stats.skipped` 不算进分母）。取消不删文件，已校验的留原处，下次按 SHA-1 跳过。失败给「再试一次」（重新发起预取，不重下整份清单），并说明"不影响启动：AssetDirector 会自己继续下"。
- **结果话术**：成功且有下载 →「首启资源已预取：下载 N 个文件（X MiB），复用 M 个（其中共享 `assets/objects` K 个）」；全已就绪 → 只写日志；取消 → 只写日志；失败 → 警告框 + 重试入口，附失败清单（前 10 条）。

### 3.5 清单维护

- 人读的一份：`assets/assetdirector/known-sets.json`（一套 = 消费者文件名正则 + MC 版本 + 版本 JSON/资源索引的 URL/sha1/size + 每对象 name/hash/size + 总量）。编进产品的一份：`src/download/AssetDirectorKnownSetData.cpp`（同样字节，chunk 拼接）。两份**必须逐字节一致**，`AssetDirectorKnownSetTests.cpp` 断言。
- **重新生成（首选）**：跑一次真包，用那次的 AD 目录反推：
  ```
  python tools/assetdirector-knownset.py --from-cache --ad-dir <数据根>/assets/asset_director --assets-root <数据根>/assets --tag <EFR tag> --mc-version <MC asset version>
  ```
  再 `--embed`。它枚举 AD 目录里的对象（逐个按官方索引字节数＋SHA-1 核过），加上"上一份清单里被 AD 从共享 `assets/objects` 复用因而没进它自己目录"的那几个；两份 JSON 的地址/哈希取自 launchermeta，并核对盘上两份与官方逐字节一致。
- **静态解析（交叉核对）**：`--efr-java <EtFuturum.java> --tag <EFR tag> --mc-version <MC>`，从 `EtFuturum.getSounds()` 字面量算一遍，结果必须是缓存反推集合的**子集**。参数读自 EFR 的 `build.gradle`（`injectedTags` 的 `MC_ASSET_VER`）和 `dependencies.gradle`（MCLib 版本）。
- 清单按整合包＋模组版本钉死：换 GTNH 版本、EFR 版本、资产版本都要重新生成。一次真机跑可看出清单够不够：启动日志里的 `Downloading resources` 和 AD 目录文件数就是判据。
- 校验（`AssetDirectorKnownSet::parse()`）：只认 `formatVersion: 1`；地址必须是 Mojang 官方 HTTPS 主机（`piston-meta` / `launchermeta` / `resources.download`）；哈希 40 位十六进制；对象名不许 `..`、不许绝对路径；同一套里哈希不许重复；声明的 `totalBytes` 必须等于逐项之和。任何一条不对整份拒用，不"尽量读"。

### 3.6 无缓存首启实测（2.1.6c，2026-10-26）

环境：空数据根（`D:\WTFtest\wtfml-216c-clean`，只有产品自己下的 `java/21` = Temurin 21.0.12.1，由 `GlobalJavaSettings::downloadJavaFromMojang` 下载）+ `GT_New_Horizons_2.8.4_Java_17-25.zip`（595,584,698 B，sha1 `fa8a5a93…`，官方 MultiMC 导出包）。导入用 `MainWindow::importModpackFromFile()`（`mw_check.exe multimc-import-live`，两问按产品默认答），启动用产品 `LauncherManager`（`verify_instances --launch-main-menu`）。

| 步骤 | 结果 |
| --- | --- |
| 导入（1.7.10 原版＋库＋资产＋解开 11972 个覆盖文件） | 42.7 s |
| 预取（1.7.10 资产已在盘上） | 1358 个文件（1356 对象 + 两份 JSON）/ **112.6 MiB 下载**、复用 12 个；20.5 s |
| 首启到主菜单 | `LAUNCH_MAIN_MENU ok=1 reason=sound-marker elapsedMs=36566`（36.6 s） |
| 启动期间 AD 写入 | 0：`assets/asset_director` 1358 个文件 / 118,056,787 B，最新 mtime 17:03:47 与启动前一致 |
| 启动日志里的 AD 行 | 0 行 |

对照（同日同机、**只预取静态集合**）：启动时 AD 又下了 612 个对象（+12.5 MB，AD 目录 746 → 1358 个文件），到主菜单 152.1 s。两趟差别就是"预取是否覆盖真实集合"，也是清单改为"以已校验缓存为准"的原因。

复现：
```
mw_check.exe multimc-import-live <包.zip> <实例名> <空数据根>
verify_instances.exe --launch-main-menu <数据根> <实例名> <数据根>/java/21/bin/java.exe 8192 780 <日志>
```
判据看 AD 目录文件数/mtime 有没有变、日志里有没有 `Downloading resources`。两趟启动日志与探针输出留在 `build/gtnh-216c-evidence/`（不随仓库发布）；原包与测试根是临时现场，可清掉，包与 Java 都能重下。2026-10-26 修过 `--launch-main-menu` 的顺序：必须先 `prepareNatives()` 再 `checkPreLaunch()`，与产品启动路一致，否则 1.7.10 首次启动还没有 natives 目录的实例会被探针自己拦下。

## 4. 缺项报告、补齐与安装凭据

**缺项报告**：CurseForge 预览保留每个缺项的项目/文件 ID、文件名、原因和可确认的目标路径，可在列表中选中条目打开官方文件网页。允许先安装其余内容，但缺项未消除时不写成功 meta、不移除 `wtfml.importing`、不向任务中心发安装成功；已下载、已解压的内容保留以便继续。实例内 `wtfml.import-report.json` 原子记录 complete、issues、failureReason；每项含编号、网页、路径、大小和 SHA-1。网络下载失败也记录可重新核对的清单，不把 HTTP/鉴权错误推断成作者意图；报告保存失败明确报错。

**手动补齐**：把文件放到清单所示位置，再用同一原包和同一实例名重新导入，可继续并重新校验。只对路径安全、非链接、大小和 40 位 SHA-1 均匹配的文件消除缺项；同名同大小但哈希不符不算补齐。接口未提供强校验或目标位置无法确认时不猜，需重查元数据。真正补齐后才写成功 meta、清除未完成标记、更新持久报告。

**补齐窗口**：在「导入整合包」文件选择框切换「继续补齐报告」，选当前数据目录下实例内的 `wtfml.import-report.json`。可选缺项、打开固定官方网页、选本地文件；核对大小和 SHA-1 后原子放入清单路径，已有目标不静默覆盖（选目标文件本身可重新校验），源文件只读，无可信路径或哈希拒绝自动部署。报告读取限制：已知 schema、4 MiB、一万条，不接受越界路径或错误类型，不信任报告里的任意网页地址。
- 「批量匹配」：选多个本地文件，按大小筛选、SHA-1 匹配后分别部署，不靠文件名猜，不覆盖已有目标；匹配阶段每源文件读一次，发布时再读校验以防源变化；共用 900 秒截止和取消标记，已完成保留，未匹配和失败项逐个写日志，界面只展示前五条原因。「已校验」标记只表示本次会话，重启后重新核对。
- 「重新校验」：不重选包或本地源文件，只读核对报告内全部条目的路径、大小和 SHA-1，刷新本次会话标记；取消/超时不把未完成哈希算通过，不复制或删除文件，也不清除 importing。
- 单文件校验/复制在后台、可取消，900 秒截止在每个 1 MiB 块之间检查；取消或哈希失败清理临时文件、不发布半文件。实例运行或安装繁忙时拒绝补入；沿用数据目录写锁，拒绝目标路径任一层的链接。
- 任务中心入口：失败/中断的导入任务在当前数据目录仍有 importing 标记及报告时显示「补齐文件」（面板右栏和详情页共用动作）；点击时重检能力及导入忙态；报告缺失/损坏明确报错，不当作下载重试。

**无原包收尾（2026-10-24）**：生产安装在核心、加载器、清单下载、覆盖解压结束后，后台生成版本化 `wtfml.import-ready.json`：绑定数据根、实例、清单 SHA-256 与每次导入独立标记，记录客户端、库、资源、继承链 JSON 和覆盖内容的大小/SHA-1，单独保存缺项强校验条件。再次导入产生不同的 attempt，不能用上一趟的凭据；历史安装仍需原包，安装完成须以完整凭据重新校验通过为准。补齐窗口的「完成安装」后台重验凭据内全部文件及补齐项，合并写元数据、保存完成报告，最后移除 importing 才放行启动；失败保留内容和门禁。补齐项要求符合清单的大小和 SHA-1（不冻结原来的错误文件字节）；目录链接、越界、标记改变、覆盖文件改变、取消均拒绝收尾。凭据保留作本地审计，没有匹配的 importing 标记就不能重复提交；凭据仅作为本地状态标记使用。自动最终校验和手动收尾均在后台按 1 MiB 分块检查取消与 900 秒截止，任务中心可取消后台解压/最终校验，工人结束前不释放安装占用。

**提交边界**：`importing` 标记优先于 meta 和顶层 jar，直到最终移除标记才视为完成。meta 写入失败或最后移除标记失败，都保留内容、返回失败并尽力保存未完成报告，不降级成仅影响显示名的警告。`ModpackCompletion::finish()` 的任何一次提前退出（文件校验取消、缺项未补齐、元数据写入失败、最后移除标记失败）都会把盘上报告退回未完成并写明原因，即使取消发生在完成报告发布之后，也不留下 `complete=true` 却仍被启动门禁挡住的现场（2026-10-25）。

**CurseForge 网页入口**（2026-10-24 实网发现）：官方网页可下载的 Fabulously Optimized 文件 8769267（项目 396246），规范路径 `/minecraft/modpacks/fabulously-optimized/files/8769267` 有效，但启动器曾用的 `/projects/396246/files/8769267` 返回 404；FO 6.5.0 清单引用的 Translations for Sodium 文件 8203766（项目 867964）接口明确不可用，其规范路径 `/minecraft/texture-packs/translations-for-sodium/files/8203766` 也 404。两种 404 要分开处理，不能据此断言文件下架。现保留平台 `links.websiteUrl`，严格限制 HTTPS、官方域名、Minecraft 已知类目与项目 slug 后构造文件页；即时缺项提示与持久报告共用该入口。旧报告没有规范地址或元数据查询失败时明确提示手动查找，不再猜数字路由。

## 5. 验证

命令：
```
tools/test.ps1 -Suite ci-core -Modules core-resources
tools/test.ps1 -Suite ci-integration -Modules window    # MwModpackImportChecks 段：主窗口导入接线
```
环境变量：`WTFML_TEST_PACK_ARCHIVES`（分号分隔的本地 ZIP 列表，附加只读样本检查；CI 不依赖本机下载目录）；`WTFML_TEST_GTNH_ARCHIVE`（真实 GTNH ZIP，验证补丁转换、主类/Java 声明、架构筛选和测试目录本地库发布）。原始归档与现有游戏目录不用于测试写入。现场验收工具见 `tests/acceptance/modpack_boundary/README.md`。

### 已验样本

| 日期 | 样本 / 场景 | 做了什么 | 结果 | 意外发现 |
| --- | --- | --- | --- | --- |
| 10-23 | GTNH 2.8.4，生产 VersionManager 独立工作区 | 真实安装（解开 11972 个覆盖文件）；生产 LauncherManager、Java 21.0.7、8 GiB 堆，实机确认 313 个模组的主菜单、新建世界生成和进入；正常关闭 | 退出码 0，日志保存玩家/世界，level.dat 时间一致 | 首启停在 AD 下载阶段，缓存对象 274→294、线程栈处于 HTTPS 网络读取状态（排查确认非死锁）；测试夹具从本机官方资源按索引大小/SHA-1 复制 3213 个对象（614,557,694 B）到隔离根 `assets/objects` 后越过。产品侧预取已在 2.1.6b 落地。旧版游戏点击位置不一致，鼠标全流程不计通过；经退出确认框正常关闭 |
| 10-23/24 | CurseForge 项目 1121856 / 文件 5813216（MinersMoon-fabric-1.20-1.0.1.jar） | 生产 VersionManager 解析 + DownloadManager 联网；PCL、HMCL 3.16.4 通过各自本地下载入口；资源模块回归 723/723（`build/test-runs/20261023-235813-e3b74413`） | 三者均在独立目录得 5637 字节、SHA-1 `64a0a20074c17998442dcb52eae13794a07f8bd3`，与 API 一致 | 社区 API 镜像 `downloadUrl=null`、`isAvailable=true`，项目 `allowModDistribution=false`。测试时开了代理，未比速度；第三方启动器未清缓存；未借用其它启动器 API Key。详细副本 `build/gtnh-install-probe/curseforge-compare` |
| 10-24 | 缺项补齐状态机（离线） | 缺文件、同名同大小错误文件、正确补齐三次导入 | 失败保留内容及继续资格，最后一次成功；资源 754/754（`20261024-012159-420ae340`），主窗口 2661/2661（`20261024-013057-60799b26`） | |
| 10-24 | 补齐入口与批量匹配 | 报告读取、未知版本/越界拒绝、不覆盖、原位重验、错误哈希拒绝、发布前取消；活动中心打开/关闭报告、按钮分派 | 资源 766/766（`20261024-014756-d0873289`）、769/769（`20261024-015251-de166f3d`）；主窗口 2661/2661（`20261024-014505-f89bd327`）；工作区 24/24、活动中心 34/34（`20261024-020109-fdbfff22`） | |
| 10-24 | 重新校验与门禁 | 真实主窗口夹具点「重新校验」；模拟 meta 路径被目录占用 | 资源 776/776、实例 1447/1447（`20261024-095549-20992363`）；工作区 27/27、活动中心 34/34（`20261024-095925-e2503abc`）；complete/meta/importing 未被放行，失败终态与已安装内容保留 | |
| 10-24 | CurseForge 网页入口修正 | 规范链接、外部域名/查询参数拒绝、报告持久化、旧报告兼容 | 资源 785/785、实例 1447/1447（`20261024-103050-ab449690`） | 沙箱内本地网络/junction 拒绝的原始失败日志已保留，通过数来自允许这些夹具操作的复测 |
| 10-24 | Fabulously Optimized 12.3.0（文件 8769267）实网安装 | 生产 VersionManager/DownloadManager 下载 50/50 个清单文件，解压 70 个覆盖文件；隔离实例 FO-1.21.11-CF-Test，MC 1.21.11 / Fabric 0.19.3 / Java 21，离线 InstallTest 身份 | complete=true、issues 为空、meta 存在、importing 已移除；主菜单显示 FO 12.3.0、43 个模组；「退出游戏」后 GAME_EXIT 0、Normal | 证据 `build/gtnh-install-probe/runs/cf-current-20261024-101444/`。同轮 FO 6.5.0 的 49 个可用条目下完后因不可用资源包返回未完成：marker=1、unfinished=1、meta=0，未误报成功，也未启动该实例。日志中 401 / Realms 鉴权错误来自离线身份，Steam Deck 检测跳过与可选兼容类警告仍在 |
| 10-24 | 无原包收尾 UI 验收 | 管理页选包、确认缺项、安装其余、活动中心「补齐文件」；重启后入口与凭据仍可用；原生选择框补入本地缺项后「完成安装」 | 界面显示安装完成，complete=true、importing 消失（`build/gtnh-install-probe/ui-completion/complete-after-restart.jpg`）；提交中断回归资源 807/807（`20261024-183628-a3e6be4a`）；ZG10 路径占位拒绝与恢复，资源 818/818（`20261024-183833-b05b1f51`） | 缺项由可控本机 API 构造，核心与 Fabric 用隔离缓存。发现并修复真实导入任务缺 targetId 导致补齐按钮不出现，已加回归。ZG10：元数据已发布而报告失败时仍保留 importing，缺失凭据也不信任完成报告 |
| 10-25 | 收尾报告一致性 | `ModpackCompletion::finish()` 各提前退出路径 | 资源 824/824 | |
| 10-26 | GTNH 2.8.4 + 空数据根（2.1.6c） | 导入 + 预取 + 首启到主菜单 | 见 §3.6：36.6 s，AD 写入 0 | 静态集合少 612 个对象 |
| 10-27 | CurseForge 项目 1121856 / 文件 5813216 实网，现场 `H:/WTFML-Acceptance-20261025/modpack-boundary-20261027-0050`（2.1.7） | 官方无 key 请求返回 403；元数据 `allowModDistribution=false`、`downloadUrl=null`；生产解析器与下载器取得 5637 字节 JAR，SHA-1 `64a0a20074c17998442dcb52eae13794a07f8bd3`；`report.json`：USB HDD 目标上慢速回放取消、强制结束及新进程恢复通过 | 通过；回放只保证命中中断点 | 完整生产导入 `import-*.json`：取消 2747 ms 通过，强制终止时 importing=true/meta=false，新进程 80541 ms 完成，`import-complete.json` 中 ok/success/realFileVerified/stageProof/completeReport/meta 均为 true、marker=false。首次工具漏传清单指纹，安装成功但无安装凭据，未算通过，保存为 `import-complete-first-missing-fingerprint.json`；取消早于目标建立的首次错误判据保存为 `import-cancel-first-oracle.json`；二者是工具问题，改用 `RestrictedBoundaryVerified` 独立实例补验 |
| 10-27 | GTNH 2.8.4（Microsoft Java 17）与 Fabulously Optimized（Java 21）已有隔离实例 | 生产启动链启动，等声音初始化后确认真实主菜单并截图，未打开存档 | `build/acceptance-2.1.7-20261027/gtnh-java17-main-menu.jpg`、`fo-java21-main-menu.jpg`；日志 `gtnh-java17-final.log`、`fo-java21-final.log` | 首次电脑控制无法激活窗口，只有加载画面，经重新启动才取得主菜单截图；旧失败不删除 |

### 未覆盖

| 项 | 说明 |
| --- | --- |
| 其它 GTNH 版本 / Java 组合 / 长期玩法 | 只跑了 Java 21（及 17 主菜单）、8 GiB 堆、一次冷缓存；未进世界做长期游玩 |
| 多 DPI、慢盘、真实磁盘满/断电/最后移除标记失败 | 取消只在 1 MiB 块边界检查，单次系统 I/O 卡住时无法保证立即返回；路径占位夹具未涵盖真实磁盘硬件级故障 |
| 失败与取消的 AD 预取现场 | 只在离线夹具里验过 |
| 新缺项弹层、补齐入口的真机多 DPI 视觉验收 | 仅离线接线与文件夹具（报告界面的"已校验"标记与取消等待输入保护同） |
| 主窗口导入向导的整条 CurseForge 实网安装 | 10-24 的 FO 安装由调用生产后端的测试程序驱动；下载复用另一隔离测试的已校验缓存（非冷缓存测试） |
| 其它受限 CurseForge 文件 | 只验了项目 1121856 / 文件 5813216 |
| 样本识别/清单解析 | 仅验证结构解析，未覆盖全流程联网安装与启动 |
