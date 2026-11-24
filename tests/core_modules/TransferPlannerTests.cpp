#include "Support.hpp"
#include "TextMarks.hpp"

void runTransferPlannerTests() {
    section("TR. 迁移 / 升级副本：计划判据与只增不删的落盘");
    using DeclarationMatch = ModVersionDeclaration::Match;
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral(">=1.20 <1.22")), QStringLiteral("1.21.1")) == DeclarationMatch::Yes, "TR 声明范围支持目标版本");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("[1.20,1.21)")), QStringLiteral("1.21")) == DeclarationMatch::No, "TR Maven 排他上界不能当作兼容");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("1.21.x")), QStringLiteral("1.21.4")) == DeclarationMatch::Yes, "TR Fabric 通配范围");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("~1.21.1")), QStringLiteral("1.22")) == DeclarationMatch::No, "TR tilde 不越过次版本上界");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("^0.2.1")), QStringLiteral("0.3")) == DeclarationMatch::No, "TR 零主版本 caret 上界");
    check(ModVersionDeclaration::matches(QJsonValue(QJsonArray{QStringLiteral("1.20"), QStringLiteral("1.21")}), QStringLiteral("1.21")) == DeclarationMatch::Yes, "TR 数组声明是或关系");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("${minecraft_version}")), QStringLiteral("1.21")) == DeclarationMatch::Unknown, "TR 未展开声明保持未知");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral(">=1.20")), QStringLiteral("24w01a")) == DeclarationMatch::Unknown, "TR 非数字快照不猜兼容");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("999999999999999999999")), QStringLiteral("1.21")) == DeclarationMatch::Unknown, "TR 超范围版本号不截断比较");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("1.20 || 1.21")), QStringLiteral("1.21")) == DeclarationMatch::Unknown, "TR 未支持的逻辑语法不能误报不兼容");
    check(ModVersionDeclaration::matches(QJsonValue(QStringLiteral("24w01a")), QStringLiteral("24w01a")) == DeclarationMatch::Yes, "TR 完全相同的快照声明可精确匹配");

    QTemporaryDir dir;
    check(dir.isValid(), "TR0 临时目录");
    if (!dir.isValid()) {
        return;
    }
    const QString root = dir.path();
    {
        const QString game = root + QStringLiteral("/locked-game");
        const QString sourceInstance = game + QStringLiteral("/versions/source");
        const QString targetInstance = game + QStringLiteral("/versions/target");
        writeFileAt(sourceInstance + QStringLiteral("/mods/test.jar"), "MOD");
        QDir().mkpath(targetInstance);
        const bool enabledBefore = DirectorySessionLock::enabled;
        DirectorySessionLock::enabled = true;
        QLockFile other(QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock")
            .arg(DirectorySessionLock::keyFor(game))));
        other.setStaleLockTime(0);
        check(other.tryLock(0), "TR0 迁移目录占用夹具取得根目录锁");
        const auto mods = TransferApply::transferMods(sourceInstance, targetInstance,
            {sourceInstance + QStringLiteral("/mods/test.jar")}, true);
        const auto files = TransferApply::apply(sourceInstance, targetInstance, {}, {});
        DirectorySessionLock::enabled = enabledBefore;
        check(mods.failed == 1 && mods.copied == 0 && mods.moved == 0
                  && !mods.problems.isEmpty() && mods.problems.first().contains(QStringLiteral("占用"))
                  && QFileInfo::exists(sourceInstance + QStringLiteral("/mods/test.jar"))
                  && !QFileInfo::exists(targetInstance + QStringLiteral("/mods")),
              "TR0 模组迁移使用游戏根锁且拒绝后不建目录、不删源文件");
        check(!files.ok && files.copiedFiles == 0 && files.problem.contains(QStringLiteral("占用")),
              "TR0 普通实例文件迁移同样拒绝占用目录");
    }
    const QString source = root + QStringLiteral("/src-instance");
    writeFileAt(source + QStringLiteral("/saves/w1/level.dat"), QByteArrayLiteral("AAA"));
    writeFileAt(source + QStringLiteral("/saves/w2/level.dat"), QByteArrayLiteral("BBBBB"));
    writeFileAt(source + QStringLiteral("/mods/a.jar"), QByteArrayLiteral("jar"));
    writeFileAt(source + QStringLiteral("/config/mod.toml"), QByteArrayLiteral("cfg"));
    writeFileAt(source + QStringLiteral("/options.txt"), QByteArrayLiteral("opt"));
    writeFileAt(source + QStringLiteral("/logs/latest.log"), QByteArrayLiteral("log"));
    writeFileAt(source + QStringLiteral("/screenshots/shot.png"), QByteArrayLiteral("png"));

    const auto baseInputs = [&] {
        TransferPlanner::Inputs inputs;
        inputs.sourceDir = source;
        inputs.sourceGameVersion = QStringLiteral("1.20.1");
        inputs.sourceLoaderName = QStringLiteral("Forge");
        inputs.sourceLoaderVersion = QStringLiteral("47.2.0");
        inputs.targetGameVersion = QStringLiteral("1.20.1");
        inputs.targetLoaderName = QStringLiteral("Forge");
        inputs.targetLoaderVersion = QStringLiteral("47.2.0");
        return inputs;
    };
    const auto itemOf = [](const TransferPlanner::Plan &p, const QString &key)
        -> const TransferPlanner::Item * {
        for (const TransferPlanner::Item &item : p.items) {
            if (item.key == key) return &item;
        }
        return nullptr;
    };

    // ---------- TR1. 一致的这一档：默认勾什么 ----------
    {
        const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());
        check(plan.ok && plan.compatibility == TransferPlanner::Compatibility::Same,
              "★ TR1 版本与加载器一致 ⇒ `Same`", TransferPlanner::compatibilityName(plan.compatibility));
        const QStringList keys = plan.defaultSelectedKeys();
        check(keys.contains(QStringLiteral("saves")) && keys.contains(QStringLiteral("mods"))
                  && keys.contains(QStringLiteral("config"))
                  && keys.contains(QStringLiteral("options")),
              "TR1 存档 / 模组 / 配置 / 游戏设置有就默认勾", keys.join(QStringLiteral(",")));
        check(!keys.contains(QStringLiteral("logs"))
                  && !keys.contains(QStringLiteral("screenshots"))
                  && !keys.contains(QStringLiteral("crashreports")),
              "★★ TR1 日志 / 截图 / 崩溃报告**永不默认勾**", keys.join(QStringLiteral(",")));
        const TransferPlanner::Item *saves = itemOf(plan, QStringLiteral("saves"));
        check(saves != nullptr && saves->exists && saves->fileCount == 2 && saves->bytes == 8,
              "★ TR1 存档那一项数出来是 2 个文件 / 8 字节（只数不读内容）",
              saves == nullptr ? QString() : QStringLiteral("%1 / %2")
                                                   .arg(saves->fileCount)
                                                   .arg(saves->bytes));
        check(plan.selectedFileCount(keys) == 5 && plan.selectedBytes(keys) == 17,
              "★ TR1 默认勾选合计 5 个文件 / 17 字节",
              QStringLiteral("%1 / %2").arg(plan.selectedFileCount(keys)).arg(plan.selectedBytes(keys)));
        check(plan.headline().contains(QStringLiteral("迁移计划：一致"))
                  && plan.headline().contains(QStringLiteral("默认迁移")),
              "TR1 顶上那一句", plan.headline());
        check(plan.detailText().contains(TextMarks::checked() + QStringLiteral(" 存档"))
                  && plan.detailText().contains(TextMarks::unchecked() + QStringLiteral(" 日志")),
              "TR1 正文把勾与不勾都画出来");
        check(plan.detailText().contains(QStringLiteral("不覆盖"))
                  && plan.detailText().contains(QStringLiteral("不删除")),
              "★★ TR1 正文写明「只增加、不删除、不覆盖」");
        check(plan.confirmText(keys).contains(QStringLiteral("5 个文件"))
                  && plan.confirmText(keys).contains(QStringLiteral("不覆盖")),
              "TR1 动手之前那一句确认带上了数与规矩");
        check(TransferPlanner::neverDefaultSelected(QStringLiteral("logs"))
                  && !TransferPlanner::neverDefaultSelected(QStringLiteral("saves")),
              "TR1 「永不默认」的判据只有这一处");
    }

    // ---------- TR2. 不一致 / 认不出来 ----------
    {
        TransferPlanner::Inputs newer = baseInputs();
        newer.targetGameVersion = QStringLiteral("1.21.1");
        const TransferPlanner::Plan plan = TransferPlanner::plan(newer);
        check(plan.compatibility == TransferPlanner::Compatibility::GameVersionDiffers,
              "★ TR2 目标游戏版本不同 ⇒ `GameVersionDiffers`");
        const TransferPlanner::Item *mods = itemOf(plan, QStringLiteral("mods"));
        check(mods != nullptr && mods->exists && !mods->defaultSelected && !mods->note.isEmpty(),
              "★★ TR2 版本不同时**模组默认不勾**，并在那一项底下说明原因",
              mods == nullptr ? QString() : mods->note);
        check(plan.compatibilitySentence.contains(QStringLiteral("1.21.1"))
                  && plan.compatibilitySentence.contains(QStringLiteral("1.20.1")),
              "★ TR2 兼容性那句话把两边版本都写出来", plan.compatibilitySentence);

        TransferPlanner::Inputs loader = baseInputs();
        loader.targetLoaderName = QStringLiteral("Fabric");
        const TransferPlanner::Plan plan2 = TransferPlanner::plan(loader);
        check(plan2.compatibility == TransferPlanner::Compatibility::LoaderDiffers,
              "★ TR2 游戏版本相同、加载器不同 ⇒ `LoaderDiffers`");
        check(plan2.compatibilitySentence.contains(QStringLiteral("Fabric")),
              "TR2 加载器那一句点了名", plan2.compatibilitySentence);

        TransferPlanner::Inputs both = baseInputs();
        both.targetGameVersion = QStringLiteral("1.21.1");
        both.targetLoaderName = QStringLiteral("NeoForge");
        check(TransferPlanner::plan(both).compatibility
                  == TransferPlanner::Compatibility::BothDiffer,
              "★ TR2 两边都不同 ⇒ `BothDiffer`");

        TransferPlanner::Inputs unknown = baseInputs();
        unknown.targetGameVersion.clear();
        const TransferPlanner::Plan plan3 = TransferPlanner::plan(unknown);
        check(plan3.compatibility == TransferPlanner::Compatibility::Unknown,
              "★ TR2 目标版本读不出来 ⇒ `Unknown`（不猜）");
        const TransferPlanner::Item *mods3 = itemOf(plan3, QStringLiteral("mods"));
        check(mods3 != nullptr && !mods3->defaultSelected,
              "★★ TR2 认不出来时模组默认不勾", mods3 == nullptr ? QString() : mods3->note);
        check(plan3.compatibilitySentence.contains(QStringLiteral("默认不迁移模组")),
              "TR2 那一句明说「不做猜测」", plan3.compatibilitySentence);

        TransferPlanner::Inputs patch = baseInputs();
        patch.targetLoaderVersion = QStringLiteral("47.3.0");
        const TransferPlanner::Plan plan4 = TransferPlanner::plan(patch);
        check(plan4.compatibility == TransferPlanner::Compatibility::Same
                  && plan4.compatibilitySentence.contains(QStringLiteral("小版本不同")),
              "★ TR2 加载器小版本不同仍算 `Same`，但把事实说出来",
              plan4.compatibilitySentence);

        TransferPlanner::Inputs missing = baseInputs();
        missing.sourceDir = root + QStringLiteral("/nope");
        const TransferPlanner::Plan plan5 = TransferPlanner::plan(missing);
        check(!plan5.ok && plan5.problem.contains(QStringLiteral("未找到源实例目录")),
              "★ TR2 源目录不在盘上 ⇒ 计划不成立，报的是哪条路径", plan5.problem);
    }

    // ---------- TR3. 落盘：只增不删、不覆盖 ----------
    {
        const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());
        const QString target = root + QStringLiteral("/dst-instance");
        QDir().mkpath(target);
        // 目标里已经有一份同名的存档（**不能被改写**）和一个无关模组（**不能被删**）。
        writeFileAt(target + QStringLiteral("/saves/w1/level.dat"),
                    QByteArrayLiteral("OLD-CONTENT"));
        writeFileAt(target + QStringLiteral("/mods/keep.jar"), QByteArrayLiteral("keep"));

        const QStringList keys{QStringLiteral("saves"), QStringLiteral("mods"),
                               QStringLiteral("logs")};
        const TransferApply::Result result =
            TransferApply::apply(source, target, plan.items, keys);
        check(result.ok, "TR3 落盘成立", result.problem);
        check(result.copiedFiles == 3 && result.skippedFiles == 1 && result.failedFiles == 0,
              "★★ TR3 新文件 3 个（w2 / a.jar / latest.log）、同名 1 个跳过、0 失败",
              QStringLiteral("%1 / %2 / %3")
                  .arg(result.copiedFiles)
                  .arg(result.skippedFiles)
                  .arg(result.failedFiles));
        const auto contentOf = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            return file.readAll();
        };
        check(contentOf(target + QStringLiteral("/saves/w1/level.dat"))
                  == QByteArrayLiteral("OLD-CONTENT"),
              "★★ TR3 目标里已有的同名存档**一个字节都没被改**");
        check(contentOf(target + QStringLiteral("/saves/w2/level.dat"))
                  == QByteArrayLiteral("BBBBB"),
              "TR3 新存档真的搬过去了");
        check(QFileInfo::exists(target + QStringLiteral("/mods/keep.jar"))
                  && contentOf(target + QStringLiteral("/mods/keep.jar"))
                         == QByteArrayLiteral("keep"),
              "★★ TR3 目标里原有的模组**没有被删**（只增不删）");
        check(QFileInfo::exists(target + QStringLiteral("/logs/latest.log")),
              "TR3 手动勾上的日志也会搬（默认不勾 ≠ 不让勾）");
        check(!QFileInfo::exists(target + QStringLiteral("/screenshots")),
              "TR3 没勾的截图一个字节都不搬");
        check(result.text().contains(QStringLiteral("复制了 3 个文件"))
                  && result.text().contains(QStringLiteral("跳过 1 个")),
              "TR3 报告里把复制与跳过都说清", result.text());
    }

    // ---------- TR4. 路径闸 ----------
    {
        const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());
        const QStringList keys{QStringLiteral("saves")};
        const TransferApply::Result same = TransferApply::apply(source, source, plan.items, keys);
        check(!same.ok && same.problem.contains(QStringLiteral("相同或互相嵌套")),
              "★★ TR4 源和目标就是同一份 ⇒ 拒绝（不然就是自己抄自己）", same.problem);

        const QString nested = source + QStringLiteral("/nested");
        QDir().mkpath(nested);
        const TransferApply::Result inside =
            TransferApply::apply(source, nested, plan.items, keys);
        check(!inside.ok && inside.problem.contains(QStringLiteral("嵌套")),
              "★★ TR4 目标在源里面 ⇒ 拒绝", inside.problem);
        QDir().rmdir(nested);
    }

    // ---------- TR5. junction 一个都不跟 ----------
    {
        const QString link = source + QStringLiteral("/mods/linked-saves");
        const bool madeLink =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"),
                               QStringLiteral("/J"), QDir::toNativeSeparators(link),
                               QDir::toNativeSeparators(source + QStringLiteral("/saves"))})
                == 0
            && QFileInfo(link).isJunction();
        check(madeLink, "★ TR5 先造出一根真 junction：`mods/linked-saves` → `saves`");
        if (madeLink) {
            const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());
            const TransferPlanner::Item *mods = itemOf(plan, QStringLiteral("mods"));
            check(mods != nullptr && mods->fileCount == 1,
                  "★ TR5 计划里数文件**不跟 junction**（只有 a.jar 那一个）",
                  mods == nullptr ? QString() : QString::number(mods->fileCount));

            const QString target = root + QStringLiteral("/dst-link");
            QDir().mkpath(target);
            const TransferApply::Result result = TransferApply::apply(
                source, target, plan.items, {QStringLiteral("mods")});
            check(result.ok && result.copiedFiles == 1 && result.failedFiles == 1,
                  "★★ TR5 拷的时候也不跟：链接算失败、a.jar 照拷",
                  QStringLiteral("%1 / %2").arg(result.copiedFiles).arg(result.failedFiles));
            check(!QFileInfo::exists(target + QStringLiteral("/mods/linked-saves")),
                  "★★ TR5 目标里没有那个链接，也没有顺着它把存档抄一份进来");

            // 收尾：junction 只能 rmdir（删链接本体，不跟着删那一头）——
            // QTemporaryDir 的清理会跟着 junction 往下走。
            QDir().rmdir(link);
        }
    }

    // ---------- TR6. 逐项回调 + 中断之后再跑一遍是可续的 ----------
    {
        const QString target = root + QStringLiteral("/dst-progress");
        QDir().mkpath(target);
        const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());

        QStringList reportedKeys;
        QList<bool> reportedOk;
        const TransferApply::Result first = TransferApply::apply(
            source, target, plan.items,
            {QStringLiteral("saves"), QStringLiteral("mods")},
            [&reportedKeys, &reportedOk](const QString &key, bool ok) {
                reportedKeys << key;
                reportedOk << ok;
            });
        check(first.ok && first.copiedFiles == 3,
              "TR6 第一遍：两个勾选项一共 3 个文件",
              QStringLiteral("%1 / %2").arg(first.copiedFiles).arg(first.failedFiles));
        check(reportedKeys == QStringList({QStringLiteral("saves"), QStringLiteral("mods")}),
              "★ TR6 每搬完一块报一次，顺序跟着计划表走（存档在前、模组在后）",
              reportedKeys.join(QStringLiteral(",")));
        check(reportedOk.size() == 2 && reportedOk.at(0) && reportedOk.at(1),
              "TR6 两块都报成了（跳过不算失败）");

        QStringList secondKeys;
        const TransferApply::Result second = TransferApply::apply(
            source, target, plan.items,
            {QStringLiteral("saves"), QStringLiteral("mods")},
            [&secondKeys](const QString &key, bool) { secondKeys << key; });
        check(second.ok && second.copiedFiles == 0 && second.skippedFiles == 3,
              "★★ TR6 同样的 keys 再跑一遍：一个都不写、全按跳过（可恢复中断就靠这个）",
              QStringLiteral("copied=%1 skipped=%2 failed=%3")
                  .arg(second.copiedFiles)
                  .arg(second.skippedFiles)
                  .arg(second.failedFiles));
        check(secondKeys.size() == 2,
              "TR6 跳过的那一块也照样报一次（界面上的步骤不能停在「进行中」）",
              QString::number(secondKeys.size()));
    }

    // ---------- TR7. 收尾校验（收尾的最后一步）----------
    //
    // 校验的判据只有一条：**点名搬的那些文件，目标里是不是都有**。目标里原有的同名文件
    // （大小不同）不是失败 —— `apply()` 的规矩就是不覆盖，校验只如实报数。
    {
        const TransferPlanner::Plan plan = TransferPlanner::plan(baseInputs());
        const QStringList keys{QStringLiteral("saves"), QStringLiteral("mods")};

        const QString target = root + QStringLiteral("/dst-verify");
        QDir().mkpath(target);
        const TransferApply::Result applied = TransferApply::apply(source, target, plan.items, keys);
        check(applied.ok && applied.failedFiles == 0, "TR7 先正常搬一遍", applied.problem);

        const TransferApply::VerifySummary good =
            TransferApply::verify(source, target, plan.items, keys);
        check(good.ok && good.checkedFiles == 3 && good.missingFiles == 0
                  && good.sizeMismatchFiles == 0,
              "★★ TR7 搬完对账：3 个文件都在、大小都对",
              QStringLiteral("%1 checked / %2 missing / %3 mismatch")
                  .arg(good.checkedFiles)
                  .arg(good.missingFiles)
                  .arg(good.sizeMismatchFiles));
        check(good.text().contains(QStringLiteral("缺少 0 个")),
              "TR7 那句话把「缺 0 个」直接说出来", good.text());

        // 从目标里删掉一个 ⇒ 校验必须报缺，而不是静默通过。
        check(QFile::remove(target + QStringLiteral("/saves/w2/level.dat")),
              "TR7 造一个「目标里少了一个文件」的场景");
        const TransferApply::VerifySummary missing =
            TransferApply::verify(source, target, plan.items, keys);
        check(missing.ok && missing.missingFiles == 1 && missing.text().contains(QStringLiteral("缺少 1 个")),
              "★★ TR7 少一个文件 ⇒ 校验报缺（不是静默通过）", missing.text());
        check(missing.missing.size() == 1
                  && missing.missing.first().contains(QStringLiteral("level.dat"))
                  && missing.text().contains(QStringLiteral("目标中未找到的文件")),
              "TR7 缺失清单里点名了那一条路径", missing.text());

        // 目标里原有的同名文件大小不同 ⇒ 跳过不覆盖是设计，校验只单独报数。
        const QString mismatchTarget = root + QStringLiteral("/dst-verify-2");
        QDir().mkpath(mismatchTarget);
        writeFileAt(mismatchTarget + QStringLiteral("/saves/w1/level.dat"),
                    QByteArrayLiteral("OLD-CONTENT"));
        const TransferApply::Result applied2 =
            TransferApply::apply(source, mismatchTarget, plan.items, {QStringLiteral("saves")});
        check(applied2.ok && applied2.skippedFiles == 1,
              "TR7 目标原有的同名文件照旧跳过", applied2.problem);
        const TransferApply::VerifySummary mismatch =
            TransferApply::verify(source, mismatchTarget, plan.items, {QStringLiteral("saves")});
        check(mismatch.ok && mismatch.missingFiles == 0 && mismatch.sizeMismatchFiles == 1,
              "★ TR7 同名文件大小不同 ⇒ 不算缺，单独报「大小不同」",
              QStringLiteral("%1 missing / %2 mismatch")
                  .arg(mismatch.missingFiles)
                  .arg(mismatch.sizeMismatchFiles));
        check(mismatch.text().contains(QStringLiteral("大小不同")),
              "TR7 报告里那句「大小不同（没有覆盖）」说清了这是原有文件", mismatch.text());

        // 一项都没勾 ⇒ 什么都不该被算成"缺"。
        const TransferApply::VerifySummary empty =
            TransferApply::verify(source, target, plan.items, {});
        check(empty.ok && empty.checkedFiles == 0 && empty.missingFiles == 0,
              "TR7 一项都没勾 ⇒ 校验检查过 0 个、缺少 0 个（不冤枉目标）");
    }
}

