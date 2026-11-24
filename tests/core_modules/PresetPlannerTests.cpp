#include "Support.hpp"

void runPresetPlannerTests() {
    section("PE. 应用计划：一段一行地算这一趟会怎么样");

    section("PE0. 那些字（动作词与量词）");
    {
        const QStringList verbs = {Presets::applyPlanVerb(Presets::ApplyMode::Merge),
                                   Presets::applyPlanVerb(Presets::ApplyMode::Replace),
                                   Presets::applyPlanVerb(Presets::ApplyMode::Append),
                                   Presets::applyPlanVerb(Presets::ApplyMode::CopyIfMissing),
                                   Presets::applyPlanVerb(Presets::ApplyMode::Skip)};
        check(verbs.at(0) == QStringLiteral("合并") && verbs.at(1) == QStringLiteral("覆盖") &&
                  verbs.at(2) == QStringLiteral("追加") &&
                  verbs.at(3) == QStringLiteral("仅在缺失时复制") &&
                  verbs.at(4) == QStringLiteral("跳过"),
              "PE0 五档的动作词就是表上那五个字（唯一出处，别处不许再写一遍）",
              verbs.join(QStringLiteral(" / ")));
        check(Presets::applyPlanUnit(Presets::SectionKind::VanillaOptions) ==
                      QStringLiteral("项") &&
                  Presets::applyPlanUnit(Presets::SectionKind::VanillaKeys) ==
                      QStringLiteral("项") &&
                  Presets::applyPlanUnit(Presets::SectionKind::ServerList) ==
                      QStringLiteral("个") &&
                  Presets::applyPlanUnit(Presets::SectionKind::GenericFiles) ==
                      QStringLiteral("个") &&
                  Presets::applyPlanUnit(Presets::SectionKind::MalilibKeys) ==
                      QStringLiteral("项"),
              "PE0 量词：设置、按键和 MaLiLib 那几格热键说「项」、服务器和文件说「个」");
    }

    QTemporaryDir scratch;
    check(scratch.isValid(), "PE 建得出一个临时目录来摆夹具");
    if (!scratch.isValid()) {
        return;
    }

    // ---- 一份真的预设段：从 `options.txt` 的字节造出来（P2 那一层的原样产物）----
    const QByteArray sourceBytes = QByteArrayLiteral("fov:0.5\nkey_attack:key.keyboard.a\n");
    // 目标上比预设**多一个** `gamma` —— 那一个正是「覆盖」档要拿掉的（`fov` 是被同名
    // 覆盖掉的，不算"删"）。少了它，"覆盖会删东西"这条在夹具上根本演不出来。
    const QByteArray targetBytes =
        QByteArrayLiteral("fov:0.0\ngamma:1.0\nkey_jump:key.keyboard.a\n");
    const Presets::OptionsFile sourceFile = Presets::parseOptions(sourceBytes);
    const Presets::OptionsFile targetFile = Presets::parseOptions(targetBytes);
    QList<Presets::PresetSection> optionSections =
        Presets::sectionsFromOptions(sourceFile, QSet<QString>{QStringLiteral("fov"),
                                                              QStringLiteral("key_attack")});
    check(optionSections.size() == 2, "PE 夹具：一份 `options.txt` 造出两段（设置 + 按键）",
          QString::number(optionSections.size()));

    section("PE1. 没有目标实例 / 空预设");
    {
        Presets::PlanRequest empty;
        Presets::ApplyPlan plan = Presets::planPresetApplication(empty);
        check(!plan.ok && !plan.problem.isEmpty(),
              "★★ PE1 没说往哪个实例上落 ⇒ 整份计划算不出来（不是「一份空计划」）", plan.problem);

        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        plan = Presets::planPresetApplication(request);
        check(plan.ok && plan.isEmpty() && !plan.writesAnything() && plan.canApply() &&
                  !plan.needsConfirm(),
              "PE1 一份**一段都没有**的预设：算得出来、表是空的、没有拦路虎（空预设是合法的，"
              "不是错误）");
    }

    section("PE2. 两行真数（设置 + 按键，都合并）");
    {
        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.target.options = targetFile;
        request.sections = optionSections;

        const Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        check(plan.ok && plan.rows.size() == 2 &&
                  plan.rows.at(0).kind == Presets::SectionKind::VanillaOptions &&
                  plan.rows.at(1).kind == Presets::SectionKind::VanillaKeys,
              "★★ PE2 表上的次序 = 段在预设里的次序（那张表不重排）",
              QStringLiteral("%1 行").arg(plan.rows.size()));

        const Presets::ApplyPlanRow &optionsRow = plan.rows.at(0);
        check(optionsRow.count == 1 && optionsRow.removed == 0 && optionsRow.conflictCount == 0 &&
                  !optionsRow.risky() && !optionsRow.blocked && !optionsRow.needsConfirm,
              "★★ PE2 设置那一行：`fov` 从 0.0 改成 0.5 = 动 1 项、没删东西、没冲突",
              Presets::applyPlanRowText(optionsRow));
        check(Presets::applyPlanRowText(optionsRow) == QStringLiteral("合并 1 项"),
              "★★ PE2 那一行的右栏就是字面那个样子", Presets::applyPlanRowText(optionsRow));

        const Presets::ApplyPlanRow &keysRow = plan.rows.at(1);
        check(keysRow.count == 1 && keysRow.conflictCount == 1 && keysRow.risky(),
              "★★ PE2 按键那一行：`key_attack` 要绑 `key.keyboard.a`，而目标上 `key_jump` "
              "正绑着同一个键 ⇒ 冲突 1 项",
              Presets::applyPlanRowText(keysRow));
        check(Presets::applyPlanRowText(keysRow) == QStringLiteral("合并 1 项，冲突 1 项"),
              "★★ PE2 而且那一行就是这个形状（「合并 23 项，冲突 1 项」）",
              Presets::applyPlanRowText(keysRow));
        check(plan.totalCount() == 2 && plan.riskyRowCount() == 1 && plan.blockedRowCount() == 0 &&
                  !plan.needsConfirm() && plan.canApply(),
              "PE2 整份：一共动 2 项、有风险的那一行只有按键那一行、可以开工");
    }

    section("PE3. 「覆盖」的两趟协议（「Replace 必须明确确认」）");
    {
        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.target.options = targetFile;
        QList<Presets::PresetSection> sections = optionSections;
        sections[0].mode = Presets::ApplyMode::Replace;
        request.sections = sections;

        const Presets::ApplyPlan first = Presets::planPresetApplication(request);
        const Presets::ApplyPlanRow &row = first.rows.at(0);
        check(first.ok && row.needsConfirm && !row.blocked && row.risky() && !row.problem.isEmpty(),
              "★★ PE3 第一趟（还没点头）：「覆盖」那一行标着要确认", row.problem);
        check(row.count == 1 && row.removed == 1,
              "★★★ PE3 而且**数照样给出来**（动 1 项、删除 1 项）—— 只告诉用户「要确认」而不告诉"
              "他「会删除什么」，那个确认框等于没问",
              Presets::applyPlanRowText(row));
        check(Presets::applyPlanRowText(row) == QStringLiteral("覆盖 1 项，删除 1 项"),
              "PE3 那一行的右栏", Presets::applyPlanRowText(row));
        check(first.needsConfirm() && !first.canApply(),
              "★★ PE3 整份计划**不许开工**（`canApply()` 是那条闸的唯一出处）");
        check(!first.rows.at(1).needsConfirm,
              "PE3 而按键那一段是「合并」⇒ 不该陪着一起要确认（一格是一格）");

        request.replaceConfirmed = true;
        const Presets::ApplyPlan second = Presets::planPresetApplication(request);
        check(!second.needsConfirm() && second.canApply() && !second.rows.at(0).needsConfirm &&
                  second.rows.at(0).count == 1 && second.rows.at(0).removed == 1,
              "★★ PE3 用户点头之后再算一趟：那一道闸放开，其余的数一个字都没变",
              Presets::applyPlanRowText(second.rows.at(0)));
    }

    section("PE4. 空段铺上去 = 把那一片清空（这一行最容易被写成「覆盖 0 项」）");
    {
        Presets::PresetSection empty;
        empty.id = Presets::optionsSectionId(Presets::SectionKind::VanillaOptions);
        empty.kind = Presets::SectionKind::VanillaOptions;
        empty.mode = Presets::ApplyMode::Replace;

        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.target.options = targetFile;
        request.sections = {empty};
        request.replaceConfirmed = true;

        const Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        const Presets::ApplyPlanRow &row = plan.rows.at(0);
        check(plan.ok && !row.blocked && row.count == 0 && row.removed == 2,
              "★★ PE4 「覆盖成空」：一个键都不写、但目标上那一域认得出的 2 个键（`fov` 和 "
              "`gamma`）会被删除",
              Presets::applyPlanRowText(row));
        check(Presets::applyPlanRowText(row) == QStringLiteral("覆盖，删除 2 项"),
              "★★★ PE4 那一行**绝不能写成「覆盖 0 项」** —— 把「什么都写」和「什么都没留下」说成"
              "同一件事，是最要紧的那半截被藏起来了",
              Presets::applyPlanRowText(row));
        check(row.removed > 0 && row.risky() && row.count == 0 && !plan.writesAnything(),
              "★★ PE4 而且它算「有风险」的那一行（会删东西）—— 哪怕「会动的项数」是 0");
    }

    section("PE5. 服务器列表那一段");
    {
        const QList<NbtUtil::ServerEntry> sourceEntries = {
            NbtUtil::ServerEntry{QStringLiteral("甲服"), QStringLiteral("play.example.com")},
            NbtUtil::ServerEntry{QStringLiteral("乙服"), QStringLiteral("mc.example.org:25566")}};
        QList<Presets::PresetSection> sections =
            Presets::sectionsFromServerList(sourceEntries, Presets::ApplyMode::Append);
        check(sections.size() == 1, "PE5 夹具：一份预设里的服务器列表段");

        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.sections = sections;
        // 目标上已经有"甲服"了（地址一模一样）。
        request.target.servers = {
            NbtUtil::ServerEntry{QStringLiteral("甲服（旧的）"), QStringLiteral("play.example.com")},
            // 预设里没有的一台 ⇒ 「覆盖」档要拿掉的正是它（对得上的那一台是就地改名）。
            NbtUtil::ServerEntry{QStringLiteral("丙服（要被拿掉）"),
                                 QStringLiteral("old.example.net")}};

        Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        check(plan.rows.size() == 1 && !plan.rows.at(0).blocked && plan.rows.at(0).count == 1 &&
                  plan.rows.at(0).skipped == 1,
              "★★ PE5 追加 2 条、其中「甲服」已经在列表里了 ⇒ 添 1 条、跳 1 条",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("追加 1 个，跳过 1 个"),
              "★★ PE5 那一行的右栏",
              Presets::applyPlanRowText(plan.rows.at(0)));

        // 「跳过」那一档：适配器照样数得出"你这一段跳过了 2 条"，但一个字节都不会动。
        request.sections = Presets::sectionsFromServerList(sourceEntries,
                                                          Presets::ApplyMode::Skip);
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).count == 0 && plan.rows.at(0).skipped == 2 &&
                  !plan.writesAnything(),
              "★★ PE5 「跳过」：数得出跳过 2 条，`count` 清零（表上不许虚报会动的东西）",
              Presets::applyPlanRowText(plan.rows.at(0)));

        // 「覆盖」+ 没点头 ⇒ 要确认，但"会拿掉几条"照样给出来。
        request.sections = Presets::sectionsFromServerList(sourceEntries,
                                                          Presets::ApplyMode::Replace);
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).needsConfirm && plan.rows.at(0).removed == 1 &&
                  plan.rows.at(0).count == 2 && !plan.canApply(),
              "★★ PE5 「覆盖」没点头：要写 2 条、会拿掉 1 条（目标上对不上的那一台），先要确认",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("覆盖 2 个，删除 1 个"),
              "PE5 那一行的右栏", Presets::applyPlanRowText(plan.rows.at(0)));

        // 目标那一份读不动 ⇒ 这一段**做不了**（而不是"目标上是空的"）。
        request.target.serversProblem = QStringLiteral("servers.dat 读不动（不是 NBT）");
        plan = Presets::planPresetApplication(request);
        check(plan.ok && plan.rows.at(0).blocked &&
                  plan.rows.at(0).problem.contains(QStringLiteral("读不动")),
              "★★ PE5 目标那一份读不动 ⇒ 这一段做不了（整份计划照样算得出来 —— 做不了的是"
              "**这一段**）",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("无法完成"),
              "PE5 做不了的那一行右栏只说得出这三个字");
    }

    section("PE6. 通用文件组那一段（唯一会读盘的一档）");
    {
        const QString presetFiles = scratch.filePath(QStringLiteral("预设/files"));
        const QString instanceRoot = scratch.filePath(QStringLiteral("目标实例"));
        QDir().mkpath(presetFiles + QStringLiteral("/config"));
        QDir().mkpath(instanceRoot);
        check(writeBytesAt(presetFiles + QStringLiteral("/config/a.json"),
                           QByteArrayLiteral("{\"a\":1}")),
              "PE6 夹具：预设那一侧有一份 `config/a.json`");

        QList<Presets::GenericItem> items;
        QString why;
        check(Presets::scanFileGroup(presetFiles, Presets::FileGroup::Config, &items, nullptr,
                                     &why),
              "PE6 扫出预设那一侧的这一组", why);
        QList<Presets::PresetSection> sections;
        check(Presets::sectionsFromFileGroup(Presets::FileGroup::Config, items,
                                             Presets::ApplyMode::CopyIfMissing, &sections, &why),
              "PE6 造出「通用文件组」那一段", why);

        Presets::PlanRequest request;
        request.presetFilesRoot = presetFiles;
        request.target.instanceRoot = instanceRoot;
        request.sections = sections;

        Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        check(plan.rows.size() == 1 && !plan.rows.at(0).blocked && plan.rows.at(0).count == 1,
              "★★ PE6 目标上还没有那一份 ⇒ 要拷 1 个（`CopyIfMissing` 那一档真数了一遍目标）",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("仅在缺失时复制 1 个"),
              "PE6 那一行的右栏", Presets::applyPlanRowText(plan.rows.at(0)));

        QDir().mkpath(instanceRoot + QStringLiteral("/config"));
        check(writeBytesAt(instanceRoot + QStringLiteral("/config/a.json"),
                           QByteArrayLiteral("用户自己调过的")),
              "PE6 目标上摆一份同名的（用户自己调的）");
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).count == 0 && plan.rows.at(0).skipped == 1 &&
                  !plan.writesAnything(),
              "★★ PE6 目标上已经有了 ⇒ 这一项是「不用动」，**不是「拷了 0 个」**",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("跳过 1 个"),
              "★★ PE6 那一行的右栏说「跳过」，不说「仅在缺失时复制 0 个」",
              Presets::applyPlanRowText(plan.rows.at(0)));

        // 「合并」在一坨字节上做不了主 ⇒ 要用户选，而且**一个字节都不写**。
        QList<Presets::PresetSection> merged;
        const bool builtMerged = Presets::sectionsFromFileGroup(
            Presets::FileGroup::Config, items, Presets::ApplyMode::Merge, &merged, &why);
        check(builtMerged, "PE6 再用「合并」那一档造一段出来", why);
        if (!builtMerged) {
            return;
        }
        request.sections = merged;
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).needsChoice == 1 && plan.rows.at(0).count == 0 &&
                  !plan.writesAnything() && plan.rows.at(0).risky(),
              "★★★ PE6 「合并」：这一项只有复制和覆盖两种做法 ⇒ 那一行是「要你选」",
              Presets::applyPlanRowText(plan.rows.at(0)));
        check(Presets::applyPlanRowText(plan.rows.at(0)) == QStringLiteral("要你选 1 个"),
              "PE6 那一行的右栏", Presets::applyPlanRowText(plan.rows.at(0)));

        // 段的身份和它的内容对不上 ⇒ **不认**这一段（`fileGroupFromSection()` 判不了这一条）。
        QList<Presets::PresetSection> wrongId = sections;
        wrongId[0].id = Presets::sectionIdFor(Presets::SectionKind::GenericFiles,
                                             QStringLiteral("shaderpacks"));
        request.sections = wrongId;
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).blocked &&
                  plan.rows.at(0).problem.contains(QStringLiteral("不匹配")),
              "★★★ PE6 段说自己是光影那一组、可里面的路径全在 `config/` 底下 ⇒ 整段不认"
              "（两份说法对不上时，我们没有资格拿它去改用户的实例）",
              plan.rows.at(0).problem);

        // 没有源根：数不出"缺哪几份"，**不许**当成"没有东西要拷"。
        request.sections = sections;
        request.presetFilesRoot.clear();
        plan = Presets::planPresetApplication(request);
        check(plan.rows.at(0).blocked &&
                  plan.rows.at(0).problem.contains(QStringLiteral("files/ 根目录")),
              "★★ PE6 没给预设那一侧的文件根 ⇒ 这一段做不了（当成「没有东西要拷」的话，用户会"
              "以为预设里那几份配置是空的）",
              plan.rows.at(0).problem);
    }

    section("PE7. 还不认识的种类（`custom` 那一段落 blocked，不拖累别的行）");
    {
        // 拿 `Custom` 当那一档：MaLiLib 从 PF 那一节起**有适配器接手了**，
        // 不能再拿它当"没人认识的种类" —— 那样这一条就不是在测那条路，而是在测一个
        // 早就补上的缺口。
        Presets::PresetSection unknown;
        unknown.id = Presets::sectionIdFor(Presets::SectionKind::Custom);
        unknown.kind = Presets::SectionKind::Custom;
        unknown.mode = Presets::ApplyMode::Merge;

        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.target.options = targetFile;
        request.sections = {optionSections.at(1), unknown};

        const Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        check(plan.ok && !plan.rows.at(0).blocked &&
                  plan.rows.at(0).count == 1 && plan.rows.at(1).blocked &&
                  plan.rows.at(1).problem.contains(QStringLiteral("custom")),
              "★★★ PE7 没有任何适配器接手的种类（用户自己塞的 `custom`）⇒ 它单独落一行"
              "「做不了」，理由里带上**盘上那个词**（这句要能原样显示给用户看，不然他只知道"
              "「有一段做不了」，不知道该去预设里删哪一段）",
              plan.rows.at(1).problem);
        check(Presets::applyPlanRowText(plan.rows.at(1)) == QStringLiteral("无法完成") &&
                  plan.blockedRowCount() == 1 && plan.canApply() && plan.totalCount() == 1,
              "★★ PE7 做不了的那一段**不拦着别的段**（整份 still 可以开工）");
    }

    section("PE8. 实例正在跑");
    {
        Presets::PlanRequest request;
        request.target.instanceRoot = scratch.filePath(QStringLiteral("实例"));
        request.target.options = targetFile;
        request.sections = optionSections;
        request.gameRunning = true;

        const Presets::ApplyPlan plan = Presets::planPresetApplication(request);
        check(plan.ok && !plan.blockedReason.isEmpty() && !plan.canApply() &&
                  plan.rows.size() == 2 && plan.totalCount() == 2,
              "★★★ PE8 实例正在跑 ⇒ **整趟**不许开工（游戏退出时会把它自己那一份整片写回去，"
              "我们写的会被盖掉 —— 而用户看到的是「应用成功了、重启就没了」）；表照样算出来给他看",
              plan.blockedReason);
    }

    section("PE9. 右栏那几条规则的边角（手工搓行，不经过适配器）");
    {
        Presets::ApplyPlanRow row;
        row.kind = Presets::SectionKind::VanillaOptions;
        row.mode = Presets::ApplyMode::Merge;
        check(Presets::applyPlanRowText(row) == QStringLiteral("无变化"),
              "★★ PE9 什么都不动的那一行说「无变化」（而「合并 0 项」听起来像「有一段是空的但我"
              "「什么都没做」，两句话不一样）",
              Presets::applyPlanRowText(row));

        row.skipped = 3;
        check(Presets::applyPlanRowText(row) == QStringLiteral("跳过 3 项"),
              "PE9 有跳过的就说跳过了几个", Presets::applyPlanRowText(row));

        row.skipped = 0;
        row.mode = Presets::ApplyMode::Skip;
        check(Presets::applyPlanRowText(row) == QStringLiteral("跳过"),
              "★ PE9 「跳过」那一档：一个字，不带数字", Presets::applyPlanRowText(row));

        row.blocked = true;
        check(Presets::applyPlanRowText(row) == QStringLiteral("无法完成") &&
                  row.risky() && !Presets::applyPlanRowText(row).contains(QStringLiteral("跳过")),
              "PE9 做不了的那一行不再提「跳过」（那会让人以为这一档就是不用管）",
              Presets::applyPlanRowText(row));

        row = Presets::ApplyPlanRow{};
        row.kind = Presets::SectionKind::GenericFiles;
        row.mode = Presets::ApplyMode::CopyIfMissing;
        row.count = 8;
        row.skipped = 2;
        check(Presets::applyPlanRowText(row) == QStringLiteral("仅在缺失时复制 8 个，跳过 2 个"),
              "PE9 一行里那几个数用「，」接起来",
              Presets::applyPlanRowText(row));

        row.conflictCount = 1;
        row.removed = 3;
        check(Presets::applyPlanRowText(row) ==
                  QStringLiteral("仅在缺失时复制 8 个，删除 3 个，冲突 1 项，跳过 2 个"),
              "★★ PE9 次序是**动的 / 删的 / 冲突 / 跳过的**（先是「会怎样」、「会拿掉什么」，"
              "再是「要你注意什么」），而且冲突那一项永远说「项」（抢键位的是键）",
              Presets::applyPlanRowText(row));
    }
}

