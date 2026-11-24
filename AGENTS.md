# AGENTS.md — 本仓库给所有 AI agent 的规则

WTFML 是 C++23 + Qt 6 Widgets 写的 Minecraft 启动器，目前只有 Windows x64 版。本文件是规则的唯一来源，`CLAUDE.md`、`GEMINI.md` 只指向这里。违反布局规则的提交会让 CI 变红。

## 入口

- 构建、运行、目录结构：[docs/dev/BUILDING.md](docs/dev/BUILDING.md)。根目录 [README.md](README.md)（英文）与 [README.zh-CN.md](README.zh-CN.md)（中文）是给玩家看的介绍页。
- 测试怎么跑、套件与超时：[docs/dev/TESTING.md](docs/dev/TESTING.md)。命令以那里为准，别另抄一份。
- 文档索引：[docs/README.md](docs/README.md)。

## 目录布局：东西放哪

- 根目录只许这些文件：`README.md`、`README.zh-CN.md`、`LICENSE`、`AGENTS.md`、`CLAUDE.md`、`GEMINI.md`、`CMakeLists.txt`、`CMakePresets.json`、`.clang-format`、`.gitattributes`、`.gitignore`，以及 `.github/` 目录。其他一律不放根目录。
- 根目录下只许这些目录：`.github` `assets` `docs` `res` `src` `tests` `tools` `ui`。
- 源码 `src/`、界面 `ui/`、测试 `tests/`、脚本与小工具 `tools/`、素材 `assets/` `res/`。
- 文档只进 `docs/`：
  - `docs/features/`：产品约定与功能设计，写现在成立的行为。
  - `docs/dev/`（含 `dev/perf-notes/`）：开发手册、测试、性能、注释规范。
  - `docs/archive/`：历史流水与一次性记录，文件名可带日期后缀，其余目录不行。
  - `docs/` 顶层只有 `README.md`、`FEATURES.md`、`ROADMAP.md`。
- 机器检查：`pwsh tools/check-repo-layout.ps1`，规则同上，CI 里会跑。

## 文档规则

- 不新建一次性的报告、验收、审查散文件。结果写成一行，追加进 `docs/archive/ACCEPTANCE_LOG.md`（验收）或 `docs/archive/REVIEW_LOG.md`（审查、复核），格式 日期｜做了什么｜结论｜意外发现。
- `docs/ROADMAP.md` 只列未完成的事；做完的删掉，需要留痕写进 archive。
- 改了用户可见行为或约束，同步改 `docs/FEATURES.md` 和对应的 `docs/features/*.md`。
- 性能数据写进 `docs/dev/PERFORMANCE.md` 或 `docs/dev/perf-notes/`，带日期和量法。
- 文档文件名用大写加下划线，不带日期后缀（`docs/archive/` 除外）。
- 移动、改名、合并、删除文档之后，必须跑 `pwsh tools/check-doc-links.ps1`，并全仓搜旧文件名确认无残留。
- 源码注释里的 `docs/…` 路径跟着文档走，改名时一起改。

## 写作口吻

1. 直接写结论和事实：数字、命令、路径、原因。主语用"我们/本程序/某模块"，中文，平实，不夸大。
2. 不写先划界句：不写"这不是…也不是…，而是…"，不写"这不代表…""只证明…不作为…""不替代真机验收""本轮未重复…"这类声明。直接写它是什么、证明什么。
3. 真有读者会误用的限制，用一句"限制：…"写在该节末尾，一节最多一句；确有未验证项，集中到文末的「未覆盖」小表。
4. 判断一句话去留：删掉它，下一个改代码的人会不会误操作？会就留，改成肯定句或"注意：…"；不会就删。
5. 不记流水：少用"本轮/本次/当前轮"；历史写进 archive 的一行，不堆在功能文档里。
6. 每篇开头一句说明这篇讲什么、给谁看；章节按读者要做的事排（怎么用、约定、限制、排查）。
7. 文中点名的文件、类、函数、计数，写之前用 Grep 或 `git ls-files` 核对，不编造数字和引用。
8. 少用括号补充；一个意思写一次，别在多篇里复述同一段。

## 认领 issue

- 开工前先看 issue 有没有负责人（assignee），有没有人留言说已接手；已有人接手的不做。
- 接手时把自己设为负责人，并在 issue 下留言「已接手，正在修复」，避免别人重复开工。
- 修完后在 issue 下留言写明提交号和所在分支，再关闭 issue。

## 提交规范

- 提交信息用中文，一句话说清这次做了什么，一个提交只做一件事。
- 不提交日志、`.dmp`、`.bak`、`.orig`、`.rej`、`.stackdump`、`.tmp`、临时导出物和构建产物（`build*/` 等）。
- 不提交 `.tongxi/` 或其他个人 agent 的工作目录；不提交密钥、令牌、`dev-login.json`、`credential.json`。
- 改动先推到 `dev`（直接推送或向 `dev` 提 PR），不直接推 `main`。
- `dev` 上 CI 通过、测试完成后，再把 `main` 快进到 `dev`（`git merge --ff-only dev`）；发版标签只打在 `main` 上。
- 紧急修复如果直接改了 `main`，随即把 `main` 合回 `dev`。
- 不 force push，不改写已推送的历史。

## 验证规则

- 改了 `src/`、`ui/`、`tests/` 里的代码：先跑相关套件 `pwsh tools/test.ps1 …`，用法见 [docs/dev/TESTING.md](docs/dev/TESTING.md)；新增用例按那里的约定放。
- 改了文档、移动了文件：跑 `pwsh tools/check-doc-links.ps1` 和 `pwsh tools/check-repo-layout.ps1`，两个都要 PASS。
- 改了 `src/` 或 `ui/` 里的界面文案：跑 `pwsh tools/check-i18n.ps1`。新写的中文要包 `tr()` / `QCoreApplication::translate()`，确不翻的行尾标 `// i18n-ignore`，流程见 [docs/dev/I18N.md](docs/dev/I18N.md)。
- 交付时写清跑了什么、结果如何；没跑的写没跑，别写成通过。
