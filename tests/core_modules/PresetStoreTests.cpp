#include "Support.hpp"
#include "presets/PresetWorkspace.hpp"

void runPresetStoreTests() {
    {
        QTemporaryDir shaderScratch;
        const QString packSource = shaderScratch.filePath(QStringLiteral("source.zip"));
        QFile file(packSource);
        const bool sourceReady = file.open(QIODevice::WriteOnly) && file.write("pack-fixture") == 12;
        file.close();
        check(sourceReady, "Shader preset source fixture created");
        if (sourceReady) {
            const QString shaderRoot = shaderScratch.filePath(QStringLiteral("presets"));
            const auto fileSection = [&](const QString &folder) {
                return QJsonObject{{"id", Presets::sectionIdFor(Presets::SectionKind::GenericFiles, folder)},
                    {"kind", "genericFiles"}, {"mode", "copyIfMissing"},
                    {"items", QJsonArray{QJsonObject{{"path", folder + "/source.zip"}, {"_source", packSource}}}}};
            };
            QJsonObject document{{"id", Presets::newPresetId()}, {"name", "Resource and shader"},
                {"sections", QJsonArray{fileSection("resourcepacks"), fileSection("shaderpacks")}}};
            const auto saved = Presets::saveEditorDocument(shaderRoot, document, false);
            const auto components = saved.manifest.editorState.value("components").toObject();
            check(saved.ok && components.contains("resourcePacks") && components.contains("shaderPacks")
                      && components.size() == 2,
                  "Publishing resources and shaders creates two independent private components", saved.problem);
            if (saved.ok) {
                const auto firstListing = Presets::list(shaderRoot);
                const auto secondListing = Presets::list(shaderRoot);
                check(firstListing.size() == 1 && secondListing.size() == 1 &&
                          firstListing.first().itemCount == 2 && secondListing.first().itemCount == 2 &&
                          firstListing.first().componentItemCounts.value("resourcePacks") == 1 &&
                          firstListing.first().componentItemCounts.value("shaderPacks") == 1,
                      "Preset summaries count runtime items and each resource category without rescanning files");
                bool shaderOk = false, resourceOk = false;
                const auto shader = Presets::loadManifest(shaderRoot, components.value("shaderPacks").toString(), &shaderOk);
                const auto resource = Presets::loadManifest(shaderRoot, components.value("resourcePacks").toString(), &resourceOk);
                check(shaderOk && shader.category == Presets::PresetCategory::ShaderPacks && shader.sections.size() == 1
                          && resourceOk && resource.category == Presets::PresetCategory::ResourcePacks && resource.sections.size() == 1,
                      "Shader and resource component payloads never include the other category");
                auto legacy = saved.manifest;
                legacy.id = Presets::newPresetId();
                legacy.category = Presets::PresetCategory::ResourcePacks;
                legacy.editorState = {};
                const bool legacySaved = Presets::saveManifest(shaderRoot, legacy);
                QFile legacyFile(Presets::manifestPath(shaderRoot, legacy.id));
                const bool legacyOpened = legacyFile.open(QIODevice::ReadOnly);
                const QByteArray before = legacyOpened ? legacyFile.readAll() : QByteArray();
                legacyFile.close();
                bool legacyLoaded = false;
                const auto loaded = Presets::loadManifest(shaderRoot, legacy.id, &legacyLoaded);
                const bool legacyReopened = legacyFile.open(QIODevice::ReadOnly);
                const QByteArray after = legacyReopened ? legacyFile.readAll() : QByteArray();
                legacyFile.close();
                check(legacySaved && legacyOpened && legacyReopened && legacyLoaded && loaded.sections.size() == 2 && before == after,
                      "Legacy resource presets containing shaders remain readable and unchanged on disk");
                auto legacyComposite = saved.manifest;
                legacyComposite.id = Presets::newPresetId();
                legacyComposite.editorState["components"] = QJsonObject{{"resourcePacks", legacy.id}};
                const bool legacyCompositeSaved = Presets::saveManifest(shaderRoot, legacyComposite);
                bool legacyCountMatches = false;
                for (const auto &summary : Presets::list(shaderRoot)) {
                    if (summary.id == legacyComposite.id)
                        legacyCountMatches = summary.itemCount == 2 &&
                            summary.componentItemCounts.value("resourcePacks") == 2 &&
                            !summary.componentItemCounts.contains("shaderPacks");
                }
                check(legacyCompositeSaved && legacyCountMatches,
                      "Legacy combined resource projection keeps both resource and shader items in its summary");
                auto oldEditor = document;
                oldEditor["id"] = Presets::newPresetId();
                oldEditor["sections"] = QJsonArray{fileSection("shaderpacks"),
                    QJsonObject{{"id", "vanillaOptions"}, {"kind", "vanillaOptions"},
                        {"items", QJsonArray{QJsonObject{{"key", "guiScale"}, {"value", "2"}}}}}};
                oldEditor["enabledParts"] = QJsonObject{{"resourcePacks", false}};
                const auto disabled = Presets::saveEditorDocument(shaderRoot, oldEditor, false);
                check(disabled.ok && disabled.manifest.category == Presets::PresetCategory::GameSettings
                          && disabled.manifest.sections.size() == 1,
                      "Old resource toggle does not reactivate previously disabled shaders", disabled.problem);
                Presets::PresetManifest disabledLegacy;
                disabledLegacy.id = Presets::newPresetId();
                disabledLegacy.name = "Disabled legacy shader";
                disabledLegacy.editorState = oldEditor;
                const auto migratedEditor = Presets::editorDocument(disabledLegacy);
                check(migratedEditor.value("enabledParts").toObject().value("shaderPacks") == QJsonValue(false),
                      "Opening a legacy editor document preserves the disabled shader state");
                oldEditor["id"] = Presets::newPresetId();
                oldEditor["enabledParts"] = QJsonObject{{"resourcePacks", false}, {"shaderPacks", true}, {"gameSettings", false}};
                const auto independent = Presets::saveEditorDocument(shaderRoot, oldEditor, false);
                check(independent.ok && independent.manifest.category == Presets::PresetCategory::ShaderPacks
                          && independent.manifest.sections.size() == 1,
                      "Explicit shader toggle is independent from resources and saves a shader preset", independent.problem);
            }
        }
    }
    {
        QTemporaryDir dir;
        QJsonObject doc{{"id",Presets::newPresetId()},{"name",QStringLiteral("常用")}};
        auto empty=Presets::saveEditorDocument(dir.path(),doc,false);
        check(!empty.ok,"Empty editor documents cannot be published");
        auto draft=Presets::saveEditorDocument(dir.path(),doc,true);
        check(draft.ok&&Presets::list(dir.path()).isEmpty()&&Presets::list(dir.path()+"/drafts").size()==1,"Blank drafts are stored separately from installation candidates");
        doc["sections"]=QJsonArray{QJsonObject{{"id","vanillaOptions"},{"kind","vanillaOptions"},{"items",QJsonArray{QJsonObject{{"key","guiScale"},{"value","2"}}}}}};
        doc["modReferences"]=QJsonArray{QJsonObject{{"platform","modrinth"},{"projectId","sodium"},{"name","Sodium"}}};
        auto composite=Presets::saveEditorDocument(dir.path(),doc,false);
        check(composite.ok&&composite.manifest.editorState.value("components").toObject().size()==2&&Presets::list(dir.path()).size()==1,"Composite publishes two private parts and one public preset");
        const auto parts=composite.manifest.editorState.value("components").toObject();bool ok=false;
        auto modPart=Presets::loadManifest(dir.path(),parts.value("mods").toString(),&ok);
        check(ok&&modPart.internalComponent&&modPart.sections.isEmpty()&&modPart.modReferences.size()==1,"Selecting composite Mods applies only its mod component");
        auto oldBytes=Presets::manifestToBytes(Presets::loadManifest(dir.path(),doc.value("id").toString()));
        doc["name"]=QStringLiteral("未发布修改");auto newerDraft=Presets::saveEditorDocument(dir.path(),doc,true);
        check(newerDraft.ok&&Presets::manifestToBytes(Presets::loadManifest(dir.path(),doc.value("id").toString()))==oldBytes,"Saving a draft preserves the currently published preset");
        doc["enabledParts"]=QJsonObject{{"mods",false}};auto disabled=Presets::saveEditorDocument(dir.path(),doc,false);
        check(disabled.ok&&disabled.manifest.category==Presets::PresetCategory::GameSettings&&disabled.manifest.modReferences.isEmpty()&&!disabled.document.value("modReferences").toArray().isEmpty(),"Disabled content remains editable but never enters the runtime payload");
        doc["enabledParts"]=QJsonObject{{"mods",false},{"gameSettings",false}};
        check(!Presets::saveEditorDocument(dir.path(),doc,false).ok,"Publishing requires at least one enabled populated part");
        auto unsafe=doc;unsafe["id"]="../outside";check(!Presets::saveEditorDocument(dir.path(),unsafe,true).ok,"Editor saves reject unsafe preset IDs");
    }
    {
        Presets::PresetManifest good; good.id = Presets::newPresetId(); good.name = QStringLiteral("边界");
        const auto json = Presets::manifestToJson(good);
        for (const auto *field : {"id", "name", "description", "createdAt", "updatedAt", "sourceInstanceId", "category"}) {
            for (const auto &value : {QJsonValue(QJsonValue::Null), QJsonValue(true), QJsonValue(42), QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{})}) {
                auto malformed = json; malformed.insert(QLatin1String(field), value); bool ok = true;
                Presets::manifestFromJson(QJsonDocument(malformed).toJson(), &ok);
                check(!ok, "manifest scalar null/wrong type is rejected instead of silently becoming a legacy mixed preset", QLatin1String(field));
            }
        }
        auto legacy = json; legacy.remove("category"); bool ok = false;
        const auto restored = Presets::manifestFromJson(QJsonDocument(legacy).toJson(), &ok);
        check(ok && restored.category == Presets::PresetCategory::Unset, "missing category still preserves legitimate legacy mixed presets");
        const QString big = QStringLiteral("9007199254740993");
        auto ref = Presets::modReferenceFromJson(QJsonObject{{"platform", "curseforge"}, {"projectId", big}});
        check(ref.projectId == big && Presets::modReferenceToJson(ref).value("projectId").toString() == big,
            "large platform string identity round-trips without double rounding");
        ref = Presets::modReferenceFromJson(QJsonObject{{"platform", "curseforge"}, {"projectId", 9007199254740992.0}});
        check(ref.key().isEmpty(), "numeric platform identity is rejected rather than rounded into a different project");
        QTemporaryDir corruptRoot;
        auto corrupt = Presets::create(corruptRoot.path(), QStringLiteral("损坏样本"));
        QFile damaged(Presets::manifestPath(corruptRoot.path(), corrupt.id));
        const QByteArray broken("{\"id\": ]");
        check(damaged.open(QIODevice::WriteOnly) && damaged.write(broken) == broken.size(), "malformed JSON byte-position fixture is written");
        damaged.close(); QString reason; bool readable = true;
        Presets::loadManifest(corruptRoot.path(), corrupt.id, &readable, &reason);
        check(!readable && reason.contains(QStringLiteral("字节位置")), "malformed manifest reports parser byte offset");
        check(Presets::duplicate(corruptRoot.path(), corrupt.id).id.isEmpty() &&
            damaged.open(QIODevice::ReadOnly) && damaged.readAll() == broken,
            "corrupt manifest cannot be duplicated and original evidence is preserved byte-for-byte");
    }
    section("PA. 实例预设：UUID 当目录名 / 一份坏了不连坐 / 那五件增删改");

    QTemporaryDir temp;
    check(temp.isValid(), "PA0 临时目录建出来了");
    if (!temp.isValid()) {
        return;
    }
    // 预设那一层的每一个动盘函数都收一个 `root` 参数，所以这一整节连全局数据目录都不用
    // 改 —— 喂进来的就是 root（唯一那个"默认落点"单独在 PA4 念一次）。
    const QString root = temp.path() + QStringLiteral("/presets");
    const QString outside = temp.path() + QStringLiteral("/外面");

    // ---------- PA1. id 那一道闸 ----------
    const QString idA = Presets::newPresetId();
    const QString idB = Presets::newPresetId();
    check(idA != idB && idA.size() == 36 && Presets::isValidPresetId(idA)
              && Presets::isValidPresetId(idB),
          "PA1 造出来的 id 是规范 UUID（36 个字符、不带花括号），两次不一样", idA);

    const QStringList badIds = {
        QString(),                QStringLiteral(".."),
        QStringLiteral("../外面"), QStringLiteral("a/b"),
        QStringLiteral("C:/x"),   QStringLiteral("我的预设"),
        QStringLiteral("NUL"),    QStringLiteral("nul.txt"),
        QStringLiteral("{%1}").arg(idA), idA.toUpper(),
    };
    QStringList acceptedBadIds;
    for (const QString &bad : badIds) {
        if (Presets::isValidPresetId(bad)) {
            acceptedBadIds.append(bad.isEmpty() ? QStringLiteral("(空串)") : bad);
        }
    }
    check(acceptedBadIds.isEmpty(),
          "★★ PA1 不是规范 UUID 的一律不算 id —— 含 `..`、`C:/x`、中文、`NUL`（Windows 上那个"
          "『写下去就不见了』的设备名），以及花括号/大写那两种写法（同一个 id 有两种写法，"
          "在盘上就是两个目录）",
          acceptedBadIds.join(QStringLiteral(" / ")));

    // ---------- PA1. 显示名收一收 ----------
    const QString messyName = QStringLiteral("  我的  预设\n第二行\u3000尾巴  ");
    check(Presets::cleanDisplayName(messyName) == QStringLiteral("我的 预设 第二行 尾巴"),
          "★ PA1 显示名：去首尾空白 + 把换行/制表/全角空格都折成一个空格（名字要进任务标题，"
          "那边是按一行算的）",
          Presets::cleanDisplayName(messyName));
    check(Presets::cleanDisplayName(QString(120, QLatin1Char('x'))).size() == 80
              && Presets::cleanDisplayName(QStringLiteral("  \n\t ")).isEmpty(),
          "★ PA1 名字钳到 80 个字；清完是空串 = 用户什么都没输");

    // ---------- PA1. 条目里那个相对路径 ----------
    const QStringList safePaths = {
        QStringLiteral("config/foo.txt"),   QStringLiteral("config\\foo.txt"),
        QStringLiteral("控制台.txt"),       QStringLiteral("sub/dir/"),
        QStringLiteral("./a.txt"),          QStringLiteral("a/./b"),
        QStringLiteral("console.txt"),
    };
    const QStringList unsafePaths = {
        QString(),                          QStringLiteral("/绝对"),
        QStringLiteral("C:/绝对"),          QStringLiteral("\\\\服务器\\共享"),
        QStringLiteral(".."),               QStringLiteral("a/../../b"),
        QStringLiteral("a:b"),              QStringLiteral("a//b"),
        QStringLiteral("NUL"),              QStringLiteral("nul.txt"),
        QStringLiteral("COM1"),
    };
    const auto mismatched = [](const QStringList &list, bool want) {
        QStringList out;
        for (const QString &one : list) {
            if (Presets::isSafeRelativePath(one) != want) {
                out.append(one.isEmpty() ? QStringLiteral("(空串)") : one);
            }
        }
        return out;
    };
    const QStringList badSafe = mismatched(safePaths, true);
    const QStringList badUnsafe = mismatched(unsafePaths, false);
    check(badSafe.isEmpty(), "★ PA1 这几条条目路径放行（`\\` 和 `/` 都当分隔符认）",
          badSafe.join(QStringLiteral(" / ")));
    check(badUnsafe.isEmpty(),
          "★★ PA1 这五样挡掉：空串 / 绝对路径（含盘符和 UNC）/ 任何一段是 `..` / 任何一段带"
          "冒号 / Windows 那几个保留设备名（`nul.txt` 也算 —— 那个空设备跟后缀无关）",
          badUnsafe.join(QStringLiteral(" / ")));

    // ---------- PA2. 枚举 ⇄ 落盘用的词 ----------
    bool modeRoundTrip = true;
    for (Presets::ApplyMode mode :
         {Presets::ApplyMode::Merge, Presets::ApplyMode::Replace, Presets::ApplyMode::Append,
          Presets::ApplyMode::CopyIfMissing, Presets::ApplyMode::Skip}) {
        modeRoundTrip = modeRoundTrip && Presets::applyModeFromKey(Presets::applyModeKey(mode)) == mode;
    }
    check(modeRoundTrip, "★ PA2 五种应用方式：枚举 → 词 → 枚举，一格不差");

    bool kindRoundTrip = true;
    for (Presets::SectionKind kind :
         {Presets::SectionKind::VanillaOptions, Presets::SectionKind::VanillaKeys,
          Presets::SectionKind::ServerList, Presets::SectionKind::MalilibKeys,
          Presets::SectionKind::GenericFiles, Presets::SectionKind::Custom}) {
        kindRoundTrip = kindRoundTrip && Presets::sectionKindFromKey(Presets::sectionKindKey(kind)) == kind;
    }
    check(kindRoundTrip, "★ PA2 六种段：枚举 → 词 → 枚举，一格不差");

    bool modeKnown = true;
    bool kindKnown = true;
    const Presets::ApplyMode unknownMode =
        Presets::applyModeFromKey(QStringLiteral("以后加的档"), &modeKnown);
    const Presets::SectionKind unknownKind =
        Presets::sectionKindFromKey(QStringLiteral("以后加的档"), &kindKnown);
    check(!modeKnown && unknownMode == Presets::ApplyMode::Merge && !kindKnown
              && unknownKind == Presets::SectionKind::Custom,
          "★ PA2 认不出来的词：模式落 `Merge`（四个里最不容易弄坏用户东西的那一档）、种类落 "
          "`Custom`，而且都把 ok 按下去 —— 读的那一边要判得出『这一份坏了』");

    check(Presets::schemaVersion() == 1
              && Presets::manifestFormatTag() == QStringLiteral("wtfml-preset/1")
              && Presets::indexFormatTag() == QStringLiteral("wtfml-presets-index/1"),
          "PA2 版本号 1 和那两个格式标记（★ 这两个字面量就是『认不出来就整份不认』的门槛，"
          "改它们 = 改格式）");

    // ---------- PA3. 一份写下去读回来，逐格一样 ----------
    Presets::PresetSection options;
    options.id = Presets::newPresetId();
    options.kind = Presets::SectionKind::VanillaOptions;
    options.title = QStringLiteral("游戏设置");
    options.mode = Presets::ApplyMode::Merge;
    options.items = QJsonArray{QJsonObject{{QStringLiteral("key"), QStringLiteral("fov")},
                                           {QStringLiteral("value"), 80}}};

    Presets::PresetManifest full;
    full.id = Presets::newPresetId();
    full.name = QStringLiteral("我的习惯");
    full.description = QStringLiteral("视野和音量那一套");
    full.createdAt =
        QDateTime::fromString(QStringLiteral("2026-10-14T09:00:00.250"), Qt::ISODateWithMs);
    full.updatedAt =
        QDateTime::fromString(QStringLiteral("2026-10-14T10:30:00"), Qt::ISODateWithMs);
    full.sourceInstanceId = QStringLiteral("1.20.1-forge-47.2.0");
    full.sections = {options};

    bool valueOk = false;
    const Presets::PresetManifest readBack =
        Presets::manifestFromJson(Presets::manifestToBytes(full), &valueOk);
    check(valueOk && readBack.id == full.id && readBack.name == full.name
              && readBack.description == full.description
              && readBack.createdAt.toMSecsSinceEpoch() == full.createdAt.toMSecsSinceEpoch()
              && readBack.updatedAt.toMSecsSinceEpoch() == full.updatedAt.toMSecsSinceEpoch()
              && readBack.sourceInstanceId == full.sourceInstanceId,
          "★★ PA3 逐格往返：id / 名字 / 说明 / 两个时间（到毫秒）/ 从哪个实例提取来的");
    check(readBack.sections.size() == 1 && readBack.sections.first().id == options.id
              && readBack.sections.first().kind == options.kind
              && readBack.sections.first().title == options.title
              && readBack.sections.first().mode == options.mode
              && readBack.sections.first().items == options.items,
          "★★ PA3 段也逐格往返，载荷 `items` **原样**（这一层不解释它是什么）");
    check(readBack.section(Presets::SectionKind::VanillaOptions) != nullptr
              && readBack.section(Presets::SectionKind::ServerList) == nullptr,
          "PA3 `section(kind)` 按种类找那一截（没有回 nullptr）");

    // 空着的那几格**根本不写下去**。
    Presets::PresetManifest plain;
    plain.id = Presets::newPresetId();
    plain.name = QStringLiteral("手建的");
    const QJsonObject plainRoot = QJsonDocument::fromJson(Presets::manifestToBytes(plain)).object();
    check(!plainRoot.contains(QStringLiteral("description"))
              && !plainRoot.contains(QStringLiteral("sourceInstanceId")),
          "PA3 空着的字段根本不写下去（不是写一个空串 —— 写空串读的人会以为那是个开关）");
    check(plainRoot.contains(QStringLiteral("createdAt"))
              && plainRoot.contains(QStringLiteral("updatedAt"))
              && plainRoot.value(QStringLiteral("createdAt")).toString().isEmpty(),
          "★ PA3 两个时间**总是**写（没记上就写空串）：『字段不在』和『写了但认不出来』少一层歧义");
    bool plainOk = false;
    const Presets::PresetManifest plainBack =
        Presets::manifestFromJson(Presets::manifestToBytes(plain), &plainOk);
    check(plainOk && !plainBack.createdAt.isValid(),
          "★ PA3 缺时间**不算坏**（手写一份出来试的情况是存在的）");

    // 空段照样写下去（"我这一趟就是要覆盖成空"和"我没有这一段"是两件事）。
    Presets::PresetSection emptySection;
    emptySection.kind = Presets::SectionKind::ServerList;
    Presets::PresetManifest withEmpty = plain;
    withEmpty.sections = {emptySection};
    const QJsonObject sectionObject =
        Presets::sectionToJson(emptySection);
    check(!sectionObject.contains(QStringLiteral("items"))
              && !sectionObject.contains(QStringLiteral("title"))
              && sectionObject.value(QStringLiteral("mode")).toString() == QStringLiteral("merge")
              && withEmpty.sections.first().isEmpty(),
          "★ PA3 空段不写 `items`/`title`，但 `mode` 总是写；而『这一段是空的』由 `isEmpty()` 说");

    // ---------- PA3. 坏的那几档 ----------
    const auto bytesOf = [](const QString &json) { return json.toUtf8(); };
    const QByteArray goodBytes = Presets::manifestToBytes(full);

    QByteArray wrongTag = goodBytes;
    wrongTag.replace(QByteArrayLiteral("wtfml-preset/1"), QByteArrayLiteral("别人的/9"));
    bool tagOk = true;
    check(!Presets::manifestFromJson(wrongTag, &tagOk).isValid() && !tagOk,
          "★★ PA3 格式标记对不上 ⇒ 整份不认（不半信半疑地读成一堆空壳）");
    bool junkOk = true;
    check(!Presets::manifestFromJson(QByteArrayLiteral("这不是 json"), &junkOk).isValid() && !junkOk,
          "PA3 整个文件不是 JSON ⇒ 空壳 + ok=false");

    const auto versionAccepted = [&](const QString &version) {
        bool versionOk = true;
        Presets::manifestFromJson(
            bytesOf(QStringLiteral("{\"format\":\"wtfml-preset/1\",\"schemaVersion\":%1,"
                                   "\"id\":\"%2\",\"name\":\"x\"}")
                        .arg(version, full.id)),
            &versionOk);
        return versionOk;
    };
    check(!versionAccepted(QStringLiteral("\"1\"")),
          "★★ PA3 版本号写成字符串（\"1\"）⇒ 不认 —— 由着 Qt 读的话它会给出 0，于是"
          "『以后版本写的』被当成好的一路读下去");
    check(!versionAccepted(QStringLiteral("2")),
          "★★ PA3 版本比我们新 ⇒ 整份不认（猜着读 = 拿半懂的东西去改用户的实例）");
    check(!versionAccepted(QStringLiteral("0")) && !versionAccepted(QStringLiteral("-1")),
          "PA3 版本是 0 / 负数 ⇒ 也不认（『没写版本』不该被当成『第一代』）");
    check(versionAccepted(QStringLiteral("1")), "PA3 我们这一代（1）认");

    const auto sectionsAccepted = [&](const QJsonValue &sections) {
        QJsonObject object;
        object.insert(QStringLiteral("format"), Presets::manifestFormatTag());
        object.insert(QStringLiteral("schemaVersion"), 1);
        object.insert(QStringLiteral("id"), full.id);
        object.insert(QStringLiteral("name"), QStringLiteral("x"));
        object.insert(QStringLiteral("sections"), sections);
        bool sectionOk = true;
        Presets::manifestFromJson(QJsonDocument(object).toJson(), &sectionOk);
        return sectionOk;
    };
    check(!sectionsAccepted(QJsonArray{QJsonObject{
              {QStringLiteral("kind"), QStringLiteral("未来的种类")}}}),
          "★★ PA3 段的种类认不出来 ⇒ **整份**不认（读不懂还硬套 = 拿半懂的东西去改用户的实例）");
    check(!sectionsAccepted(QJsonArray{QJsonObject{{QStringLiteral("mode"), QStringLiteral("merge")}}}),
          "PA3 缺 `kind` 那一位走的是同一条（空串一样认不出来）");
    check(!sectionsAccepted(QJsonArray{QJsonObject{
              {QStringLiteral("kind"), QStringLiteral("serverList")},
              {QStringLiteral("mode"), QStringLiteral("以后加的档")}}}),
          "★★ PA3 `mode` 写了但我们不认识 ⇒ 也不认 —— 写侧那个词拼错是**不报错**的，"
          "只能靠读的时候挡住");
    check(sectionsAccepted(QJsonArray{QJsonObject{{QStringLiteral("kind"),
                                                   QStringLiteral("serverList")}}}),
          "★ PA3 `mode` **不在**的时候按默认档 `Merge` 算，而且不算坏（可选字段缺了不是错）");
    check(!sectionsAccepted(QJsonArray{QJsonObject{
              {QStringLiteral("kind"), QStringLiteral("serverList")},
              {QStringLiteral("items"), QStringLiteral("不是数组")}}}),
          "PA3 `items` 不是数组 ⇒ 不认");
    check(!sectionsAccepted(QJsonArray{QJsonValue(7)}) && !sectionsAccepted(QStringLiteral("不是数组")),
          "PA3 段不是对象 / `sections` 不是数组 ⇒ 不认");

    bool noNameOk = true;
    check(!Presets::manifestFromJson(
               bytesOf(QStringLiteral("{\"format\":\"wtfml-preset/1\",\"schemaVersion\":1,"
                                      "\"id\":\"%1\"}")
                           .arg(full.id)),
               &noNameOk)
               .isValid()
              && !noNameOk,
          "★ PA3 缺名字（或者缺 id）⇒ 不认：身份那两格是这一份拿不拿得出去的全部");

    // ---------- PA3. 下标往返 ----------
    Presets::PresetIndex index;
    index.order = QStringList{idA, idB};
    index.defaultPresetId = idB;
    bool indexOk = false;
    const Presets::PresetIndex indexBack =
        Presets::indexFromJson(Presets::indexToBytes(index), &indexOk);
    check(indexOk && indexBack.order == index.order && indexBack.defaultPresetId == idB
              && indexBack.contains(idA),
          "PA3 下标往返（次序 + 那个唯一的默认项）");
    check(!Presets::indexToJson(Presets::PresetIndex()).contains(QStringLiteral("default")),
          "PA3 没有默认项时 `default` 那一位根本不写");
    bool dupOk = false;
    const Presets::PresetIndex dupBack = Presets::indexFromJson(
        bytesOf(QStringLiteral("{\"format\":\"wtfml-presets-index/1\",\"schemaVersion\":1,"
                               "\"order\":[\"%1\",\"%1\",\"%2\"]}")
                    .arg(idA, idB)),
        &dupOk);
    check(dupOk && dupBack.order.size() == 2 && dupBack.order.first() == idA,
          "★ PA3 次序表里同一个 id 出现两遍只留第一处（留两遍界面就会多出一行）",
          QString::number(dupBack.order.size()));
    bool badIndexOk = true;
    check(!Presets::indexFromJson(QByteArrayLiteral("这不是 json"), &badIndexOk).contains(idA)
              && !badIndexOk,
          "PA3 下标读不回来 ⇒ 空下标 + ok=false（调用方那一下要知道）");
    for (const auto &fields : {QByteArray("\"order\":null"), QByteArray("\"order\":{}"),
                              QByteArray("\"order\":[42]"), QByteArray("\"default\":false")}) {
        bool typedOk = true;
        const auto malformed = QByteArray("{\"format\":\"wtfml-presets-index/1\",\"schemaVersion\":1,") + fields + '}';
        Presets::indexFromJson(malformed, &typedOk);
        check(!typedOk, "PA3 已存在的排序/默认字段类型错误不能当空值", QString::fromUtf8(fields));
    }

    // ---------- PA4. 落点 + 盘上那一趟 ----------
    const QString originalDir = AppPaths::minecraftDir();
    const QString presetRootBefore = Presets::presetsRoot();
    AppPaths::setMinecraftDir(temp.path() + QStringLiteral("/别的游戏文件夹"));
    check(Presets::presetsRoot()
              == AppPaths::minecraftDir() + QStringLiteral("/presets")
              && Presets::presetsRoot() != presetRootBefore,
          "★★ PA4 预设跟着**当前游戏文件夹**走（`<当前游戏文件夹>/presets`）—— "
          "和实例同一层：换一个文件夹就是另一批预设",
          Presets::presetsRoot());
    AppPaths::setMinecraftDir(originalDir);

    check(Presets::list(root).isEmpty(),
          "PA4 一次都没做过预设的新用户：根目录都不在，列表还是空的（不崩）");

    const Presets::PresetManifest created =
        Presets::create(root, QStringLiteral("  我的习惯  "), QStringLiteral("视野那一套"));
    check(!created.id.isEmpty() && created.name == QStringLiteral("我的习惯")
              && created.description == QStringLiteral("视野那一套") && created.createdAt.isValid()
              && created.updatedAt.isValid(),
          "★ PA4 新建：新 id、名字收过（首尾空白去了）、两个时间都记上了", created.id);
    check(QFileInfo::exists(Presets::manifestPath(root, created.id))
              && QFileInfo(Presets::filesDir(root, created.id)).isDir()
              && QFileInfo::exists(Presets::indexPath(root)),
          "★★ PA4 落成的形状：`<root>/<id>/manifest.json` + `<root>/<id>/files/` + "
          "`index.json`");
    check(Presets::isValidPresetId(
              QFileInfo(Presets::presetDir(root, created.id)).fileName()),
          "★★ PA4 **显示名一个字节都不进路径**（目录名就是那个 UUID）");

    check(Presets::create(root, QStringLiteral("   \n\t ")).id.isEmpty(),
          "★ PA4 名字清完是空串 ⇒ 不建（回一个空壳，判据是 `id.isEmpty()`）");
    check(Presets::list(root).size() == 1, "PA4 那一次没建出东西来");

    const Presets::PresetManifest escaped =
        Presets::create(root, QStringLiteral("../../外面"));
    check(!escaped.id.isEmpty() && Presets::isValidPresetId(escaped.id)
              && QDir(Presets::presetDir(root, escaped.id)).exists() && !QDir(outside).exists(),
          "★★ PA4 名字叫 `../../外面` 也照建 —— 它只出现在界面上，路径里那一位还是 UUID，"
          "外面那个目录连建都没被建出来");
    check(Presets::remove(root, escaped.id), "PA4 把那一份收掉");

    const Presets::PresetManifest second = Presets::create(root, QStringLiteral("按键那一套"));
    const Presets::PresetManifest third = Presets::create(root, QStringLiteral("服务器那一套"));
    QList<Presets::PresetSummary> listed = Presets::list(root);
    check(listed.size() == 3 && listed.at(0).id == created.id && listed.at(1).id == second.id
              && listed.at(2).id == third.id,
          "★★ PA4 新建的排在**末尾**，列表次序 = 用户排的次序（不抢他排好的位置）",
          QString::number(listed.size()));
    // 夹具写入失败时列表为空，保留前面的失败记录，不再解引用空列表导致整套段错。
    check(!listed.isEmpty() && listed.first().name == QStringLiteral("我的习惯")
              && listed.first().description == QStringLiteral("视野那一套")
              && listed.first().sectionCount == 0 && !listed.first().isDefault,
          "PA4 列表那一栏要的那几样：显示名 / 说明 / 段数 / 是不是默认项");

    // ---------- PA4b. 四类（「不串类别」） ----------
    bool categoryRoundTrip = true;
    for (Presets::PresetCategory category :
         {Presets::PresetCategory::GameSettings, Presets::PresetCategory::Mods,
          Presets::PresetCategory::ResourcePacks, Presets::PresetCategory::DataPacks,
          Presets::PresetCategory::ShaderPacks}) {
        categoryRoundTrip =
            categoryRoundTrip
            && Presets::presetCategoryFromKey(Presets::presetCategoryKey(category)) == category;
    }
    bool unknownCategoryOk = true;
    const Presets::PresetCategory unknownCategory =
        Presets::presetCategoryFromKey(QStringLiteral("futureCategory"), &unknownCategoryOk);
    check(categoryRoundTrip
              && Presets::presetCategoryKey(Presets::PresetCategory::Unset).isEmpty(),
          "★ PA4b 五类：枚举 → 词 → 枚举一格不差；`Unset`（旧数据）**不写下去**（空串）");
    check(!unknownCategoryOk && unknownCategory == Presets::PresetCategory::Unset,
          "★★ PA4b 以后版本写的类别词 ⇒ ok=false（读不懂那一类的载荷，不能猜着当成混合预设）");

    Presets::PresetManifest legacyManifest;
    legacyManifest.id = Presets::newPresetId();
    legacyManifest.name = QStringLiteral("旧版混合");
    const QJsonObject legacyRoot =
        QJsonDocument::fromJson(Presets::manifestToBytes(legacyManifest)).object();
    bool legacyOk = false;
    const Presets::PresetManifest legacyBack =
        Presets::manifestFromJson(Presets::manifestToBytes(legacyManifest), &legacyOk);
    check(legacyOk && legacyBack.category == Presets::PresetCategory::Unset
              && !legacyRoot.contains(QStringLiteral("category")),
          "★★ PA4b 旧数据（manifest 里没有 `category` 这一位）照读、算 `Unset`；"
          "`Unset` 也绝不写下去 —— 回退一版启动器时新做的预设还在，只是不认类别");

    Presets::PresetManifest modManifest;
    modManifest.id = Presets::newPresetId();
    modManifest.name = QStringLiteral("模组那一套");
    modManifest.category = Presets::PresetCategory::Mods;
    const QJsonObject modRoot =
        QJsonDocument::fromJson(Presets::manifestToBytes(modManifest)).object();
    bool modOk = false;
    check(modRoot.value(QStringLiteral("category")).toString() == QStringLiteral("mods")
              && Presets::manifestFromJson(Presets::manifestToBytes(modManifest), &modOk)
                         .category
                     == Presets::PresetCategory::Mods
              && modOk,
          "★ PA4b 写了类别的那一份：`category: \"mods\"` 落下去、读回来还是 `Mods`");

    QByteArray futureCategoryBytes = Presets::manifestToBytes(modManifest);
    futureCategoryBytes.replace(QByteArrayLiteral("\"mods\""), QByteArrayLiteral("\"future\""));
    bool futureCategoryOk = true;
    check(!Presets::manifestFromJson(futureCategoryBytes, &futureCategoryOk).isValid()
              && !futureCategoryOk,
          "★★ PA4b 盘上写着我们认不出的类别 ⇒ **整份**不认（和段种类那一条同一条理由）");

    Presets::PresetSection optionsSection;
    optionsSection.kind = Presets::SectionKind::VanillaOptions;
    Presets::PresetSection keysSection;
    keysSection.kind = Presets::SectionKind::VanillaKeys;
    Presets::PresetSection serversSection;
    serversSection.kind = Presets::SectionKind::ServerList;
    Presets::PresetSection malilibSection;
    malilibSection.kind = Presets::SectionKind::MalilibKeys;
    Presets::PresetSection configSection;
    configSection.kind = Presets::SectionKind::GenericFiles;
    configSection.id = Presets::sectionIdFor(Presets::SectionKind::GenericFiles,
                                             QStringLiteral("config"));
    Presets::PresetSection defaultConfigsSection;
    defaultConfigsSection.kind = Presets::SectionKind::GenericFiles;
    defaultConfigsSection.id = Presets::sectionIdFor(Presets::SectionKind::GenericFiles,
                                                     QStringLiteral("defaultconfigs"));
    Presets::PresetSection resourcePacksSection;
    resourcePacksSection.kind = Presets::SectionKind::GenericFiles;
    resourcePacksSection.id = Presets::sectionIdFor(Presets::SectionKind::GenericFiles,
                                                    QStringLiteral("resourcepacks"));
    Presets::PresetSection shaderPacksSection;
    shaderPacksSection.kind = Presets::SectionKind::GenericFiles;
    shaderPacksSection.id = Presets::sectionIdFor(Presets::SectionKind::GenericFiles,
                                                  QStringLiteral("shaderpacks"));
    Presets::PresetSection customSection;
    customSection.kind = Presets::SectionKind::Custom;

    using Cat = Presets::PresetCategory;
    check(Presets::sectionFitsCategory(Cat::GameSettings, optionsSection)
              && Presets::sectionFitsCategory(Cat::GameSettings, keysSection)
              && Presets::sectionFitsCategory(Cat::GameSettings, serversSection)
              && Presets::sectionFitsCategory(Cat::GameSettings, malilibSection)
              && Presets::sectionFitsCategory(Cat::GameSettings, configSection)
              && Presets::sectionFitsCategory(Cat::GameSettings, defaultConfigsSection)
              && !Presets::sectionFitsCategory(Cat::GameSettings, resourcePacksSection)
              && !Presets::sectionFitsCategory(Cat::GameSettings, shaderPacksSection),
          "★★ PA4b 「游戏设置」= 选项 / 按键 / 服务器列表 / MaLiLib 热键 + config 那一类"
          "高级内容；资源包 / 光影包**不属于**它");
    check(Presets::sectionFitsCategory(Cat::ResourcePacks, resourcePacksSection)
              && Presets::sectionFitsCategory(Cat::ResourcePacks, shaderPacksSection)
              && !Presets::sectionFitsCategory(Cat::ResourcePacks, optionsSection)
              && !Presets::sectionFitsCategory(Cat::ResourcePacks, configSection),
          "★★ PA4b 「资源包」= resourcepacks/ + shaderpacks/（光影跟资源包同一摊文件），"
          "别的一概不收");
    check(Presets::sectionFitsCategory(Cat::ShaderPacks, shaderPacksSection)
              && !Presets::sectionFitsCategory(Cat::ShaderPacks, resourcePacksSection)
              && !Presets::sectionFitsCategory(Cat::ShaderPacks, optionsSection),
          "Independent shader presets accept only shader file sections");
    check(Presets::editorSectionPart(shaderPacksSection) == QStringLiteral("shaderPacks")
              && Presets::editorSectionPart(resourcePacksSection) == QStringLiteral("resourcePacks"),
          "Editor classifies resource and shader sections separately even when empty");
    check(Presets::sectionFitsCategory(Cat::Mods, customSection)
              && Presets::sectionFitsCategory(Cat::DataPacks, customSection)
              && !Presets::sectionFitsCategory(Cat::Mods, optionsSection)
              && !Presets::sectionFitsCategory(Cat::DataPacks, resourcePacksSection),
          "★ PA4b 模组 / 数据包首版只放得下 `Custom`（模组引用与数据包载荷在 P4 / P5）");
    check(Presets::sectionFitsCategory(Cat::Unset, optionsSection)
              && Presets::sectionFitsCategory(Cat::Unset, resourcePacksSection),
          "★★ PA4b 旧版混合预设（`Unset`）什么都放得下 —— 老数据一个字节都不动");

    Presets::PresetManifest mismatchManifest;
    mismatchManifest.id = Presets::newPresetId();
    mismatchManifest.name = QStringLiteral("串了类");
    mismatchManifest.category = Cat::Mods;
    mismatchManifest.sections = {optionsSection};
    check(!Presets::categoryMismatch(mismatchManifest).isEmpty(),
          "★ PA4b 类别与段表对不上 ⇒ `categoryMismatch()` 给出一句人话");
    mismatchManifest.category = Cat::GameSettings;
    check(Presets::categoryMismatch(mismatchManifest).isEmpty()
              && Presets::categoryMismatch(legacyManifest).isEmpty(),
          "PA4b 对得上 / 旧数据 ⇒ 空串");

    const QString catRoot = temp.path() + QStringLiteral("/presets-cat");
    Presets::PresetManifest badWrite;
    badWrite.id = Presets::newPresetId();
    badWrite.name = QStringLiteral("写不进去的");
    badWrite.category = Cat::Mods;
    badWrite.sections = {optionsSection};
    check(!Presets::saveManifest(catRoot, badWrite)
              && !QFileInfo::exists(Presets::manifestPath(catRoot, badWrite.id)),
          "★★ PA4b 写侧那道闸：类别和内容对不上的一份**一个字节都不写**（宁可这一趟存不下去，"
          "也不要把串了类的预设写进盘里）");
    const Presets::PresetManifest catCreated =
        Presets::create(catRoot, QStringLiteral("资源包那一套"), QString(), Cat::ResourcePacks);
    check(!catCreated.id.isEmpty() && catCreated.category == Cat::ResourcePacks
              && Presets::loadManifest(catRoot, catCreated.id).category == Cat::ResourcePacks,
          "★ PA4b `create(..., category)`：新的一份带着类别落盘、读回来还在");
    const QList<Presets::PresetSummary> catListed = Presets::list(catRoot);
    check(catListed.size() == 1 && catListed.first().category == Cat::ResourcePacks,
          "★ PA4b 列表摘要里也带着类别（管理页靠它本地筛）");

    // 手写一份"类别说 A、内容其实是 B"的 manifest：读侧也必须报坏（不是只管写侧）。
    const QString tamperedId = Presets::newPresetId();
    QDir().mkpath(Presets::presetDir(catRoot, tamperedId));
    QJsonObject tampered;
    tampered.insert(QStringLiteral("format"), Presets::manifestFormatTag());
    tampered.insert(QStringLiteral("schemaVersion"), 1);
    tampered.insert(QStringLiteral("id"), tamperedId);
    tampered.insert(QStringLiteral("name"), QStringLiteral("手改过的"));
    tampered.insert(QStringLiteral("category"), QStringLiteral("mods"));
    tampered.insert(QStringLiteral("sections"),
                    QJsonArray{QJsonObject{{QStringLiteral("kind"),
                                            QStringLiteral("vanillaOptions")}}});
    writeFileAt(Presets::manifestPath(catRoot, tamperedId),
                QJsonDocument(tampered).toJson());
    bool tamperedOk = true;
    QString tamperedReason;
    Presets::loadManifest(catRoot, tamperedId, &tamperedOk, &tamperedReason);
    check(!tamperedOk && tamperedReason.contains(QStringLiteral("类别")),
          "★★ PA4b 读侧那道闸：盘上那份类别和内容对不上 ⇒ 报坏（拿『类别说 A、内容其实是 B』"
          "的预设去改用户实例，比少一份预设危险得多）",
          tamperedReason);

    // 这一整段用的是自己的 root，不碰 PA4 那三份。
    Presets::remove(catRoot, catCreated.id);
    QDir(catRoot).removeRecursively();

    // ---------- PA5. 那一道逃逸闸 ----------
    QDir().mkpath(outside);
    writeFileAt(outside + QStringLiteral("/重要的东西.txt"), QByteArrayLiteral("别删我"));
    check(!Presets::remove(root, QStringLiteral("../外面")),
          "★★ PA5 `remove(root, \"../外面\")` 回 false —— 那道闸在动盘之前先把它挡了");
    check(QFileInfo::exists(outside + QStringLiteral("/重要的东西.txt")),
          "★★ PA5 外面那份东西**还在**（这条路的症状不是报错，是『删掉一份预设』顺手删了别的东西）");
    check(!Presets::remove(root, QStringLiteral("../../.."))
              && !Presets::remove(root, QString()),
          "PA5 别的几种写法（`../../..`、空串）一样挡在门口");

    bool escapeOk = true;
    QString escapeReason;
    Presets::loadManifest(root, QStringLiteral("../外面"), &escapeOk, &escapeReason);
    check(!escapeOk && escapeReason.contains(QStringLiteral("不是预设")),
          "★ PA5 读那一边也一样，而且说得出为什么", escapeReason);

    Presets::PresetManifest forged;
    forged.id = QStringLiteral("../外面");
    forged.name = QStringLiteral("伪造的");
    check(!Presets::saveManifest(root, forged)
              && !QFileInfo::exists(outside + QStringLiteral("/manifest.json")),
          "★★ PA5 写那一边也挡（`presetDir()` 拼出来的路径正好落在外面那个目录里，"
          "而它一个字节都没写进去）");
    Presets::PresetManifest nameless = created;
    nameless.name = QStringLiteral("   ");
    check(!Presets::saveManifest(root, nameless),
          "★ PA5 名字清完是空的 manifest 写不下去");
    check(Presets::loadManifest(root, created.id).name == QStringLiteral("我的习惯"),
          "PA5 刚才那一发一个字都没落到盘上");

    // ---------- PA6. 改名只改显示名 ----------
    writeFileAt(Presets::filesDir(root, created.id) + QStringLiteral("/config/keys.txt"),
                QByteArrayLiteral("W=前进"));
    const Presets::PresetManifest before = Presets::loadManifest(root, created.id);
    check(Presets::rename(root, created.id, QStringLiteral("  新名字  ")), "PA6 改名成功");
    const Presets::PresetManifest renamed = Presets::loadManifest(root, created.id);
    check(renamed.name == QStringLiteral("新名字") && renamed.id == before.id
              && renamed.createdAt == before.createdAt && renamed.updatedAt >= before.updatedAt,
          "★★ PA6 改名**只改显示名**：id 和创建时间一个字都不动，目录名还是那个 UUID");
    check(QFileInfo::exists(Presets::manifestPath(root, created.id))
              && QFileInfo::exists(Presets::filesDir(root, created.id)
                                   + QStringLiteral("/config/keys.txt")),
          "★★ PA6 目录、`manifest.json`、`files/` 里用户放的东西**一个都没动**（普通重命名不改"
          "磁盘上的身份）");
    check(!Presets::rename(root, created.id, QStringLiteral("   ")), "PA6 清完是空串回 false");
    check(Presets::rename(root, created.id, QStringLiteral("新名字"))
              && Presets::loadManifest(root, created.id).updatedAt == renamed.updatedAt,
          "★ PA6 改成同一个名字算成功，但『改了一次』这个动作没发生过 ⇒ 时间也不动");
    check(!Presets::rename(root, QStringLiteral("../外面"), QStringLiteral("x"))
              && !Presets::rename(root, Presets::newPresetId(), QStringLiteral("x")),
          "PA6 非法 / 不存在的 id 回 false");

    // ---------- PA7. 排序 + 那个唯一的默认项 ----------
    check(Presets::move(root, third.id, -1), "PA7 往上挪一位");
    listed = Presets::list(root);
    check(listed.size() == 3 && listed.at(0).id == created.id && listed.at(1).id == third.id
              && listed.at(2).id == second.id,
          "★★ PA7 挪动真的换了位（次序表的次序就是列表的次序）");
    check(!Presets::move(root, third.id, -5) && !Presets::move(root, second.id, +1)
              && !Presets::move(root, Presets::newPresetId(), -1),
          "★ PA7 挪不动的那一头回 false（界面按这个判『那一侧的三角画不画』）");

    check(Presets::setDefault(root, third.id), "PA7 设默认项");
    check(Presets::defaultPresetId(root) == third.id, "PA7 那个唯一的默认项就是它");
    listed = Presets::list(root);
    check(listed.size() == 3 && listed.at(1).isDefault && !listed.at(0).isDefault && !listed.at(2).isDefault,
          "★★ PA7 列表里**只有一份**顶着默认项（『设为唯一默认项』）");
    check(!Presets::setDefault(root, Presets::newPresetId()),
          "PA7 不在下标里的 id 设不成默认");
    check(Presets::setDefault(root, QString()) && Presets::defaultPresetId(root).isEmpty(),
          "PA7 空串 = 取消默认");
    check(Presets::setDefault(root, third.id), "PA7 再设回来");

    check(Presets::remove(root, third.id), "PA7 删掉那一份");
    check(Presets::defaultPresetId(root).isEmpty(),
          "★★ PA7 删掉的正好是默认项 ⇒ **默认位空出来**，不偷偷换给列表里的下一份"
          "（那一下用户看不见，而默认项是个有后果的开关）");
    check(Presets::list(root).size() == 2 && !QDir(Presets::presetDir(root, third.id)).exists(),
          "★ PA7 目录连同它的东西一起收掉了，别的两份还在");
    check(Presets::remove(root, third.id), "PA7 再删一次：目录已经不在了也算删掉了（幂等）");

    // ---------- PA8. 复制一份 ----------
    Presets::PresetManifest sourceWithSection = Presets::loadManifest(root, created.id);
    sourceWithSection.sections = {options};
    check(Presets::saveManifest(root, sourceWithSection), "PA8 把源那一份改成一个带段的");

    const Presets::PresetManifest copy = Presets::duplicate(root, created.id);
    check(!copy.id.isEmpty() && copy.id != created.id
              && copy.name == QStringLiteral("新名字 副本"),
          "★★ PA8 复制出来的是**新 id**（副本是另一份，不是同一份的两个名字）", copy.name);
    check(copy.sections.size() == 1
              && copy.sections.first().kind == Presets::SectionKind::VanillaOptions
              && copy.sections.first().items == options.items,
          "★★ PA8 段原样搬过去（载荷也是原样）");
    check(QFileInfo(Presets::filesDir(root, copy.id) + QStringLiteral("/config/keys.txt")).size()
              == QByteArrayLiteral("W=前进").size(),
          "★ PA8 `files/` 里用户放的东西也搬过来了（那份目录是这一份的一部分）");
    check(Presets::list(root).last().id == copy.id, "PA8 复制出来的排在末尾");
    const Presets::PresetManifest named = Presets::duplicate(root, created.id, QStringLiteral("  另一个  "));
    check(named.name == QStringLiteral("另一个"), "PA8 给了名字就用给的（一样要收一遍）");
    check(Presets::duplicate(root, QStringLiteral("../外面")).id.isEmpty()
              && Presets::duplicate(root, Presets::newPresetId()).id.isEmpty(),
          "★ PA8 源读不回来 ⇒ 空壳（不拿半懂的东西去造一份新的）");

    // ---------- PA9. 一份坏了不许连坐别的（P1 点名的验收）----------
    const Presets::PresetManifest doomed = Presets::create(root, QStringLiteral("坏掉的那一份"));
    const int aliveBefore = Presets::list(root).size();
    writeFileAt(Presets::manifestPath(root, doomed.id), QByteArrayLiteral("{ 这不是 json"));
    QList<Presets::BrokenPreset> broken;
    const QList<Presets::PresetSummary> survivors = Presets::list(root, &broken);
    check(survivors.size() == aliveBefore - 1 && broken.size() == 1
              && broken.first().id == doomed.id,
          "★★ PA9 一份坏了**只丢它自己**：别的照旧列得出来",
          QStringLiteral("活了 %1 份、坏了 %2 份").arg(survivors.size()).arg(broken.size()));
    check(!broken.isEmpty() && broken.first().reason.contains(QStringLiteral("manifest.json")),
          "★ PA9 坏的那一份说得出**是哪个文件**（这句人话原样能显示给用户，界面不再加工）",
          broken.isEmpty() ? QStringLiteral("未生成损坏记录，检查前置夹具失败") : broken.first().reason);
    check(!Presets::loadManifest(root, doomed.id).isValid(), "PA9 直接读它也是空壳");

    // 下标里有、盘上已经没有了（「默认项丢失」那个组合要的正是这一档）。
    const QString ghostId = Presets::newPresetId();
    check(Presets::saveIndex(root, Presets::appendToIndex(Presets::loadIndex(root), ghostId)),
          "PA9 往下标里塞一个盘上没有的 id");
    QList<Presets::BrokenPreset> withGhost;
    const QList<Presets::PresetSummary> survivors2 = Presets::list(root, &withGhost);
    QString ghostReason;
    for (const Presets::BrokenPreset &one : withGhost) {
        if (one.id == ghostId) {
            ghostReason = one.reason;
        }
    }
    check(survivors2.size() == survivors.size() && !ghostReason.isEmpty()
              && ghostReason.contains(QStringLiteral("已经不在了")),
          "★★ PA9 下标里有、盘上没了的 ⇒ 进 broken **而不进列表**", ghostReason);
    check(Presets::loadIndex(root).contains(ghostId),
          "★★ PA9 读的那一趟**没有替用户改下标**（读的时候顺手改盘是最难查的那一类）");

    // ---------- PA10. 盘上有、下标里没有的 → 照样列出来 ----------
    Presets::PresetManifest loose;
    loose.id = Presets::newPresetId();
    loose.name = QStringLiteral("用户自己拷进来的");
    check(Presets::saveManifest(root, loose), "PA10 直接在盘上放一份（不进下标）");
    listed = Presets::list(root);
    check(listed.size() == survivors.size() + 1 && listed.last().id == loose.id,
          "★★ PA10 『盘上有、下标里没有』的那一份**照样列出来、排在末尾** —— 不丢东西是这一条"
          "的全部理由",
          listed.last().name);

    // ---------- PA11. 下标本身坏了：读不回来就不许写回去 ----------
    {
        const QString inaccessibleRoot = temp.path() + QStringLiteral("/unreadable-index");
        QDir().mkpath(Presets::indexPath(inaccessibleRoot));
        bool readable = true;
        Presets::loadIndex(inaccessibleRoot, &readable);
        check(!readable, "PA11 存在但不可读的索引不能冒充新用户空索引");
        const QString oversizedRoot = temp.path() + QStringLiteral("/oversized-index");
        QDir().mkpath(oversizedRoot);
        writeFileAt(Presets::indexPath(oversizedRoot), QByteArray(4 * 1024 * 1024 + 1, ' '));
        readable = true;
        Presets::loadIndex(oversizedRoot, &readable);
        check(!readable && !Presets::setDefault(oversizedRoot, created.id) &&
                  QFileInfo(Presets::indexPath(oversizedRoot)).size() == 4 * 1024 * 1024 + 1,
              "PA11 超大索引拒绝读取且默认项操作不覆盖原文件");
        QDir().mkpath(Presets::presetDir(oversizedRoot, created.id));
        const QString hugeManifest = Presets::manifestPath(oversizedRoot, created.id);
        writeFileAt(hugeManifest, QByteArray(16 * 1024 * 1024 + 1, ' '));
        QString reason;
        readable = true;
        Presets::loadManifest(oversizedRoot, created.id, &readable, &reason);
        check(!readable && reason.contains(QStringLiteral("16 MiB")) &&
                  QFileInfo(hugeManifest).size() == 16 * 1024 * 1024 + 1,
              "PA11 超大预设清单给出大小限制且不修改原文件", reason);
    }
    const QByteArray rottenIndex = QByteArrayLiteral("{\"format\":\"别人的/9\"}");
    writeFileAt(Presets::indexPath(root), rottenIndex);
    bool rottenOk = true;
    const Presets::PresetIndex rotten = Presets::loadIndex(root, &rottenOk);
    check(!rottenOk && rotten.order.isEmpty() && rotten.defaultPresetId.isEmpty(),
          "★ PA11 下标读不回来 ⇒ 空下标 + ok=false（那一位是给调用方看的：这一趟别写回去）");
    check(Presets::list(root).size() >= survivors.size(),
          "★★ PA11 下标整个废掉之后，盘上那些预设**还是列得出来**（读不回来的下标不连坐用户"
          "的东西）",
          QString::number(Presets::list(root).size()));
    check(Presets::remove(root, loose.id), "PA11 下标坏着也能删一份（目录那一趟照做）");
    QFile rottenFile(Presets::indexPath(root));
    const bool rottenOpened = rottenFile.open(QIODevice::ReadOnly);
    check(rottenOpened, "PA11 那份坏下标读得出来（接下来比的是**字节**，不是『能读回一点』）");
    const QByteArray afterRemove = rottenFile.readAll();
    rottenFile.close();
    check(afterRemove == rottenIndex,
          "★★ PA11 那一趟**没有把空下标写回去** —— 写回去 = 把用户排好的顺序抹平，而屏幕上"
          "什么都不说");
    check(!Presets::move(root, created.id, 1) && !Presets::setDefault(root, created.id),
          "PA11 次序和默认项那两件在下标坏着时**回 false**（不动盘、也不假装成功）");

    // ---------- PA12. 列表摘要的缓存：盘上变了必须当场看见 ----------
    //
    // `list()` 每一趟都要把每份 manifest 读回来解析，1000 份实测 2.3 秒，所以摘要要缓存
    // （`PresetStore.cpp` 的 `SummaryCacheEntry`）。缓存能出错的地方只有一处：**该失效的
    // 时候没失效**。下面五条都围着这一处：外改、内改、坏转好、默认项、内部部件。
    {
        const QString cacheRoot = temp.path() + QStringLiteral("/summary-cache");
        const Presets::PresetManifest first = Presets::create(cacheRoot, QStringLiteral("甲甲甲"));
        const Presets::PresetManifest second = Presets::create(cacheRoot, QStringLiteral("乙乙"));
        check(Presets::list(cacheRoot).size() == 2,
              "PA12 先正常列一趟（缓存就是这一趟填起来的）");

        // 12a. **外面**改了这份文件、字节数还一样 ⇒ 只剩"修改时刻"那一格能发现。
        const QString firstPath = Presets::manifestPath(cacheRoot, first.id);
        const QFileInfo before(firstPath);
        Presets::PresetManifest renamed = Presets::loadManifest(cacheRoot, first.id);
        renamed.name = QStringLiteral("丙丙丙");
        writeFileAt(firstPath, Presets::manifestToBytes(renamed));
        {
            QFile touch(firstPath);
            check(touch.open(QIODevice::ReadWrite), "PA12 那份文件打得开（下面的比较靠它）");
            // 写和上一次读可能落在同一毫秒里（时间戳粒度），显式往后拨一下：
            // 测的是"会不会比大小和时刻"，不是运气。
            touch.setFileTime(before.lastModified().addSecs(30), QFileDevice::FileModificationTime);
        }
        QStringList names;
        for (const Presets::PresetSummary &one : Presets::list(cacheRoot)) {
            names.append(one.name);
        }
        check(QFileInfo(firstPath).size() == before.size()
                  && names.contains(QStringLiteral("丙丙丙")),
              QStringLiteral("★★ PA12 外面改了一份、字节数没变 ⇒ 列表里就是新的（键是"
                             "『大小 + 修改时刻』，不是『路径』）"),
              names.join(QStringLiteral(" / ")));

        // 12b. **本进程**写的那一份：把时间戳拨回去、大小也一样，也不许见到旧的。
        //      靠的是写盘那条路（`writeBytesAtomic`）当场作废缓存那一格。
        const QFileInfo written(firstPath);
        Presets::PresetManifest third = Presets::loadManifest(cacheRoot, first.id);
        third.name = QStringLiteral("丁丁丁");
        check(Presets::saveManifest(cacheRoot, third), "PA12 本进程改一次名字");
        {
            QFile touch(firstPath);
            check(touch.open(QIODevice::ReadWrite), "PA12 那份文件打得开（下面的比较靠它）");
            touch.setFileTime(written.lastModified(), QFileDevice::FileModificationTime);
        }
        names.clear();
        for (const Presets::PresetSummary &one : Presets::list(cacheRoot)) {
            names.append(one.name);
        }
        check(names.contains(QStringLiteral("丁丁丁")) && QFileInfo(firstPath).size() == written.size(),
              QStringLiteral("★★ PA12 本进程写的那一下：大小和修改时刻都跟缓存里记的一样，"
                             "照样看得见新名字（作废靠写盘，不靠时间戳）"),
              names.join(QStringLiteral(" / ")));

        // 12c. 坏的那一份**不进缓存**：否则把它修好之后还得等别的东西来撞醒缓存。
        writeFileAt(firstPath, QByteArrayLiteral("{ 这不是 json"));
        QList<Presets::BrokenPreset> brokenAfter;
        const QList<Presets::PresetSummary> whileBroken = Presets::list(cacheRoot, &brokenAfter);
        check(whileBroken.size() == 1 && brokenAfter.size() == 1,
              "PA12 坏掉的那一份这一趟就掉出列表（别的照旧）");
        brokenAfter.clear();
        check(Presets::saveManifest(cacheRoot, third), "PA12 把那份修回去");
        const QList<Presets::PresetSummary> afterFix = Presets::list(cacheRoot, &brokenAfter);
        check(afterFix.size() == 2 && brokenAfter.isEmpty(),
              "★★ PA12 修好之后**下一趟**就回来了（读不回来的那份不进缓存）");

        // 12d. 默认项不进缓存：它由**每一次调用**的下标决定。
        Presets::setDefault(cacheRoot, second.id);
        bool firstDefault = false;
        bool secondDefault = false;
        for (const Presets::PresetSummary &one : Presets::list(cacheRoot)) {
            if (one.id == first.id) firstDefault = one.isDefault;
            if (one.id == second.id) secondDefault = one.isDefault;
        }
        Presets::setDefault(cacheRoot, first.id);
        bool firstDefault2 = false;
        bool secondDefault2 = false;
        for (const Presets::PresetSummary &one : Presets::list(cacheRoot)) {
            if (one.id == first.id) firstDefault2 = one.isDefault;
            if (one.id == second.id) secondDefault2 = one.isDefault;
        }
        check(!firstDefault && secondDefault && firstDefault2 && !secondDefault2,
              "★★ PA12 换默认项之后下一趟就换过来了（那一格从来没进过缓存）");

        // 12e. 内部部件（合成预设的私立零件）不进列表 —— 缓存也不能把它放进来。
        Presets::PresetManifest part = Presets::create(cacheRoot, QStringLiteral("零件"));
        part.internalComponent = true;
        check(Presets::saveManifest(cacheRoot, part), "PA12 造一个内部部件");
        check(Presets::list(cacheRoot).size() == 2,
              "★★ PA12 内部部件不进列表（这一趟填了缓存）");
        check(Presets::list(cacheRoot).size() == 2,
              "★★ PA12 第二趟走的是缓存，还是不进列表");
        part.internalComponent = false;
        check(Presets::saveManifest(cacheRoot, part) && Presets::list(cacheRoot).size() == 3,
              "★★ PA12 它不再是内部部件之后**下一趟**就列出来了");
    }

    // ---------- PA13. 一次读很多份（`list()` 走并行那一档） ----------
    // `loadManifests()` 在份数 >= 8 且线程池不止一个线程时并行读。这几条钉住的是：
    // 并行读完了**次序还是 `ids` 的次序**、错了/坏了的那几份各自归位、缓存照样生效。
    {
        const QString manyRoot = temp.path() + "/parallel-list";
        QDir().mkpath(manyRoot);

        QStringList ids;
        QStringList names;
        for (int i = 0; i < 12; ++i) {
            const QString name = QStringLiteral("并行 %1").arg(i, 2, 10, QLatin1Char('0'));
            const Presets::PresetManifest made = Presets::create(manyRoot, name);
            check(!made.id.isEmpty(), QStringLiteral("PA13 造得出来第 %1 份").arg(i));
            ids.append(made.id);
            names.append(name);
        }
        QList<Presets::BrokenPreset> brokenMany;
        const QList<Presets::PresetSummary> listed = Presets::list(manyRoot, &brokenMany);
        QStringList listedNames;
        for (const Presets::PresetSummary &one : listed) {
            listedNames.append(one.name);
        }
        check(listed.size() == 12 && brokenMany.isEmpty(),
              QStringLiteral("PA13 十二份一次列全（坏的一份都没有）[%1]").arg(listed.size()));
        check(listedNames == names,
              "★★ PA13 并行读回来的次序就是下标的次序（不是读完的先后）");

        // 13b. 改一份（只改名字、字节数多半不变）：靠写盘作废旧那一格，重读回来还是新名字。
        Presets::PresetManifest renamed = Presets::loadManifest(manyRoot, ids.at(5));
        renamed.name = QStringLiteral("并行 05（改过）");
        check(Presets::saveManifest(manyRoot, renamed), "PA13 改掉中间那一份的名字");
        bool sawRenamed = false;
        for (const Presets::PresetSummary &one : Presets::list(manyRoot)) {
            if (one.id == ids.at(5)) {
                sawRenamed = (one.name == renamed.name);
            }
        }
        check(sawRenamed, "★★ PA13 并行那一趟里改过的那一份照样看得见");

        // 13c. 坏一份：它掉出去、进 broken，别的十一份原地不动。
        writeFileAt(Presets::manifestPath(manyRoot, ids.at(7)), QByteArrayLiteral("{ 这不是 json"));
        QList<Presets::BrokenPreset> brokenV;
        const QList<Presets::PresetSummary> listedV = Presets::list(manyRoot, &brokenV);
        check(listedV.size() == 11 && brokenV.size() == 1 && brokenV.at(0).id == ids.at(7),
              "★★ PA13 坏的那一份只掉它自己（十一份照列、那一份进 broken）");
    }

    // ---------- PA14. 每类一个默认项（旧版那个单默认项只剩"读旧数据"） ----------
    {
        const QString defRoot = temp.path() + "/category-defaults";
        QDir().mkpath(defRoot);
        const Presets::PresetManifest game =
            Presets::create(defRoot, QStringLiteral("游戏设置那份"), {}, Presets::PresetCategory::GameSettings);
        const Presets::PresetManifest modA =
            Presets::create(defRoot, QStringLiteral("模组甲"), {}, Presets::PresetCategory::Mods);
        const Presets::PresetManifest pack =
            Presets::create(defRoot, QStringLiteral("资源包那份"), {}, Presets::PresetCategory::ResourcePacks);
        const Presets::PresetManifest modB =
            Presets::create(defRoot, QStringLiteral("模组乙"), {}, Presets::PresetCategory::Mods);
        check(!game.id.isEmpty() && !modA.id.isEmpty() && !pack.id.isEmpty() && !modB.id.isEmpty(),
              "PA14 四份夹具（两模组 + 游戏设置 + 资源包）造得出来");

        const auto defaultsOf = [&defRoot](const QString &id) {
            bool found = false;
            bool value = false;
            for (const Presets::PresetSummary &one : Presets::list(defRoot)) {
                if (one.id == id) {
                    found = true;
                    value = one.isDefault;
                }
            }
            return found ? value : false;
        };

        // 14a. 每类各设一个：两份不同类的预设可以同时是默认。
        check(Presets::setDefault(defRoot, Presets::PresetCategory::Mods, modA.id),
              "PA14 给『模组』这一类设默认");
        check(Presets::setDefault(defRoot, Presets::PresetCategory::ResourcePacks, pack.id),
              "★ PA14 再给『资源包』设一个 —— 各管各的，不是替换掉上一个");
        check(defaultsOf(modA.id) && defaultsOf(pack.id) && !defaultsOf(game.id) && !defaultsOf(modB.id),
              "★★ PA14 列表里两份各顶着自己那一类的默认（同类的另一份不跟着亮）");

        // 14b. 同一类里换一个 = 替换，不是两份都亮。
        check(Presets::setDefault(defRoot, Presets::PresetCategory::Mods, modB.id),
              "PA14 模组那一类换一份");
        check(defaultsOf(modB.id) && !defaultsOf(modA.id) && defaultsOf(pack.id),
              "★★ PA14 同一类只有一个默认项（换的那一下把上一份换下来，别的类不受影响）");

        // 14c. 读法：显式那格优先；旧版那个单默认项只在"没表过态"时按自己的类别顶上来。
        Presets::PresetDefaults read = Presets::defaults(defRoot);
        check(read.byCategory.value(Presets::presetCategoryKey(Presets::PresetCategory::Mods)) == modB.id
                  && Presets::defaultFor(read, Presets::PresetCategory::Mods) == modB.id,
              "★ PA14 显式那一格读得回来");
        check(Presets::defaultFor(read, Presets::PresetCategory::GameSettings).isEmpty()
                  && Presets::defaultFor(read, Presets::PresetCategory::DataPacks).isEmpty(),
              "★★ PA14 没设过的类没有默认（不拿别的那份凑数）");

        // 14d. 旧数据：只有一个 `default`（游戏设置那份）。它只在自己那一类顶上来。
        const QString legacyRoot = temp.path() + "/legacy-single-default";
        QDir().mkpath(legacyRoot);
        const Presets::PresetManifest legacyGame =
            Presets::create(legacyRoot, QStringLiteral("老的游戏设置"), {}, Presets::PresetCategory::GameSettings);
        const Presets::PresetManifest legacyMods =
            Presets::create(legacyRoot, QStringLiteral("老的模组"), {}, Presets::PresetCategory::Mods);
        check(Presets::setDefault(legacyRoot, legacyGame.id), "PA14 旧写法：设那个唯一的默认项");
        const Presets::PresetDefaults legacy = Presets::defaults(legacyRoot);
        check(legacy.legacyId == legacyGame.id && legacy.byCategory.isEmpty(),
              "★★ PA14 旧下标里只有 `default` 一位，`defaults` 一个键都不会凭空长出来（不静默迁移）");
        check(Presets::defaultFor(legacy, Presets::PresetCategory::GameSettings,
                                  Presets::PresetCategory::GameSettings) == legacyGame.id,
              "★★ PA14 旧默认项**自己属于哪一类**，那一类没表过态时才由它顶上");
        check(Presets::defaultFor(legacy, Presets::PresetCategory::Mods,
                                  Presets::PresetCategory::GameSettings).isEmpty(),
              "★★ PA14 别的类不会被它顶上（旧默认项只点亮它自己那一行）");
        check(Presets::defaultFor(legacy, Presets::PresetCategory::Mods,
                                  Presets::PresetCategory::Unset).isEmpty(),
              "★★ PA14 旧默认项是混合预设（`Unset`）⇒ 一类都不推导：不静默重组混合默认");

        // 14e. 显式"这一类不要默认"挡得住旧默认项的推导。
        check(Presets::setDefault(legacyRoot, Presets::PresetCategory::GameSettings, QString()),
              "PA14 游戏设置这一类明确设成『不要默认』");
        const Presets::PresetDefaults blocked = Presets::defaults(legacyRoot);
        check(blocked.byCategory.contains(Presets::presetCategoryKey(Presets::PresetCategory::GameSettings))
                  && Presets::defaultFor(blocked, Presets::PresetCategory::GameSettings,
                                         Presets::PresetCategory::GameSettings).isEmpty(),
              "★★ PA14 显式『不要』不会被旧默认项重新顶上来（键在 = 表过态，值空 = 不要）");
        check(Presets::list(legacyRoot).size() == 2, "PA14 拉黑默认项不删预设（两份都还在）");

        // 14f. 删掉默认那一份：那一类空出来，**不**回退到旧默认项。
        check(Presets::setDefault(legacyRoot, Presets::PresetCategory::Mods, legacyMods.id)
                  && Presets::remove(legacyRoot, legacyMods.id),
              "PA14 模组的默认项被删掉");
        const Presets::PresetDefaults afterRemove = Presets::defaults(legacyRoot);
        check(afterRemove.byCategory.contains(Presets::presetCategoryKey(Presets::PresetCategory::Mods))
                  && afterRemove.byCategory.value(Presets::presetCategoryKey(Presets::PresetCategory::Mods)).isEmpty()
                  && Presets::list(legacyRoot).size() == 1,
              "★★ PA14 删掉默认项 ⇒ 那一类明确『没有默认』（不偷偷换给同类的下一份）");

        // 14g. 设不成的几种：混合预设没有"这一类"、坏 id、乱填的类别键。
        check(!Presets::setDefault(defRoot, Presets::PresetCategory::Unset, modA.id),
              "PA14 混合预设（`Unset`）没有分类默认，回 false");
        check(!Presets::setDefault(defRoot, Presets::PresetCategory::Mods, QStringLiteral("../外面"))
                  && !Presets::setDefault(defRoot, Presets::PresetCategory::Mods, QStringLiteral("不是 id")),
              "★ PA14 不是 id 的值写不进去（那是拼路径的东西）");
        // 指着一个盘上已经没有的 id：写是写得下（"默认缺失回退"由调用方落成"没有默认"），
        // 但是**不许**因此把一个坏值写进下标。
        const QString ghost = Presets::newPresetId();
        check(Presets::setDefault(defRoot, Presets::PresetCategory::DataPacks, ghost)
                  && Presets::defaultFor(Presets::defaults(defRoot), Presets::PresetCategory::DataPacks) == ghost
                  && !defaultsOf(ghost),
              "★★ PA14 默认项指着一份盘上没了的预设：下标照记、列表里一份都不亮（回退在调用方）");

        // 14h. 下标往返 + 坏形状不认。
        const Presets::PresetIndex roundTrip = Presets::loadIndex(defRoot);
        check(roundTrip.categoryDefaults.value(Presets::presetCategoryKey(Presets::PresetCategory::Mods)) == modB.id
                  && roundTrip.categoryDefaults.contains(
                      Presets::presetCategoryKey(Presets::PresetCategory::DataPacks)),
              "PA14 `defaults` 那一格往返之后还在");
        check(!Presets::indexToJson(Presets::PresetIndex()).contains(QStringLiteral("defaults")),
              "PA14 一格都没表过态时 `defaults` 不写下去（旧版本读它也不认识）");

        const auto indexBytes = [](const QString &defaultsJson) {
            QJsonObject root;
            root.insert(QStringLiteral("format"), Presets::indexFormatTag());
            root.insert(QStringLiteral("schemaVersion"), 1);
            root.insert(QStringLiteral("order"), QJsonArray());
            // 原样放进那份 JSON（数组 / 对象都照原形状塞，别先 `.object()` 抹成空表）。
            const QJsonDocument defaults = QJsonDocument::fromJson(defaultsJson.toUtf8());
            root.insert(QStringLiteral("defaults"), defaults.isArray() ? QJsonValue(defaults.array())
                                                                      : QJsonValue(defaults.object()));
            return QJsonDocument(root).toJson();
        };
        bool badOk = true;
        Presets::indexFromJson(indexBytes(QStringLiteral("[1,2,3]")), &badOk);
        check(!badOk, "★★ PA14 `defaults` 不是对象 ⇒ 整份下标不认（和 `order` / `default` 同一条规矩）");
        bool typeOk = true;
        Presets::indexFromJson(indexBytes(QStringLiteral("{\"mods\":3}")), &typeOk);
        check(!typeOk, "★★ PA14 某一个值不是字符串 ⇒ 整份下标不认（不悄悄变成『没有默认项』）");
        bool futureOk = true;
        const Presets::PresetIndex future =
            Presets::indexFromJson(indexBytes(QStringLiteral("{\"futureCategory\":\"%1\"}").arg(ghost)),
                                   &futureOk);
        check(futureOk && future.categoryDefaults.isEmpty(),
              "★ PA14 认不出的类别键（将来版本加的类）跳过那一格，整份下标照读");
        // 坏 id 那一格由 `loadIndex()` 的归一化兜住（`indexFromJson` 只认形状，不认 id 长相）。
        const QString dirtyRoot = temp.path() + "/dirty-defaults";
        QDir().mkpath(dirtyRoot);
        writeFileAt(Presets::indexPath(dirtyRoot),
                    indexBytes(QStringLiteral("{\"%1\":\"不是 id\",\"%2\":\"\"}")
                                   .arg(Presets::presetCategoryKey(Presets::PresetCategory::Mods),
                                        Presets::presetCategoryKey(Presets::PresetCategory::DataPacks))));
        bool dirtyOk = true;
        const Presets::PresetIndex dirty = Presets::loadIndex(dirtyRoot, &dirtyOk);
        check(dirtyOk && !dirty.categoryDefaults.contains(Presets::presetCategoryKey(Presets::PresetCategory::Mods))
                  && dirty.categoryDefaults.value(
                         Presets::presetCategoryKey(Presets::PresetCategory::DataPacks), QStringLiteral("x")).isEmpty(),
              "★★ PA14 坏 id 那一格当『没表过态』丢掉，明确『不要』那一格留着"
              "（不让一格坏数据把整份下标作废，也不让坏格混成『不要』）");
    }

    // ---------- PA15. 摘要缓存落盘：下一次启动的第一趟不用再读 manifest ----------
    //
    // 进程里那张缓存只帮第二趟；冷启动第一趟仍要读每份 manifest（1000 份并行 0.40 s）。
    // 盘上那份（`summaryCachePath()`）记的是"上一次读出来的摘要 + 那一份的字节数/毫秒"。
    // 这几条钉住三件事：**真能从盘上灌回来**、**stat 对不上就重读**、**本进程写过的
    // 不被盘上那份顶掉**（同字节数、同一毫秒时 stat 认不出来，只能靠写侧作废）。
    {
        const QString cachedRoot = temp.path() + "/persisted-summaries";
        QDir().mkpath(cachedRoot);
        const Presets::PresetManifest made = Presets::create(cachedRoot, QStringLiteral("落盘摘要"));
        check(!made.id.isEmpty(), "PA15 夹具造得出来");

        // 15a. 正常一趟之后盘上有那份文件；`forgetSummaryCache()` 连它一起删。
        check(Presets::list(cachedRoot).size() == 1, "PA15 第一趟列得出来");
        const QString cachePath = Presets::summaryCachePath(cachedRoot);
        check(QFileInfo::exists(cachePath), "★ PA15 读过 manifest 之后盘上有一份摘要缓存");
        Presets::forgetSummaryCache(cachedRoot, true);
        check(QFileInfo::exists(cachePath),
              "★ PA15 只清进程里那张时，盘上那份留着（量『下一次启动』走这一档）");
        Presets::forgetSummaryCache(cachedRoot);
        check(!QFileInfo::exists(cachePath),
              "★ PA15 `forgetSummaryCache()` 把盘上那份也删了（否则『冷读』测的还是缓存）");

        // 15b. 造一份"上一次进程留下的"摘要，manifest 本身写坏但字节数/时刻与摘要对得上：
        //      这一趟只有走盘上摘要才列得出来（真去读 manifest 就会进 broken）。
        const QString manifest = Presets::manifestPath(cachedRoot, made.id);
        const QByteArray badBytes = QByteArrayLiteral("{ 这不是 json");
        writeFileAt(manifest, badBytes);
        const QFileInfo badInfo(manifest);
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), made.id);
        entry.insert(QStringLiteral("size"), static_cast<double>(badInfo.size()));
        entry.insert(QStringLiteral("mtime"), static_cast<double>(badInfo.lastModified().toMSecsSinceEpoch()));
        entry.insert(QStringLiteral("listed"), true);
        entry.insert(QStringLiteral("name"), QStringLiteral("盘上摘要"));
        entry.insert(QStringLiteral("description"), QString());
        entry.insert(QStringLiteral("updatedAt"), QString());
        entry.insert(QStringLiteral("sections"), 0);
        entry.insert(QStringLiteral("items"), 9);
        entry.insert(QStringLiteral("componentItems"), QJsonObject{{"mods", 9}});
        entry.insert(QStringLiteral("category"), QString());
        entry.insert(QStringLiteral("components"), QJsonArray());
        entry.insert(QStringLiteral("icon"), QString());
        const QJsonObject cacheDoc{
            {QStringLiteral("format"), QStringLiteral("wtfml.preset-summaries")},
            {QStringLiteral("version"), 2},
            {QStringLiteral("entries"), QJsonArray{entry}}};
        writeFileAt(cachePath, QJsonDocument(cacheDoc).toJson(QJsonDocument::Compact));
        QList<Presets::BrokenPreset> brokenCached;
        const QList<Presets::PresetSummary> fromDisk = Presets::list(cachedRoot, &brokenCached);
        check(fromDisk.size() == 1 && brokenCached.isEmpty()
                  && fromDisk.at(0).name == QStringLiteral("盘上摘要")
                  && fromDisk.at(0).itemCount == 9 && fromDisk.at(0).componentItemCounts.value("mods") == 9,
              "★★ PA15 下一次启动第一趟直接吃盘上那份摘要（manifest 坏着也没去读 —— "
              "字节数/时刻对得上就当没变）",
              fromDisk.isEmpty() ? QStringLiteral("（一份都没列出来）") : fromDisk.at(0).name);

        // 15c. 盘上那份跟文件对不上了 ⇒ 照样去读 manifest，坏的进 broken（缓存只许变慢，
        //      不许把一份读不回来的东西说成好的）。
        writeFileAt(manifest, badBytes + QByteArrayLiteral(" "));
        QList<Presets::BrokenPreset> brokenMismatch;
        check(Presets::list(cachedRoot, &brokenMismatch).isEmpty() && brokenMismatch.size() == 1
                  && brokenMismatch.at(0).id == made.id,
              "★★ PA15 字节数对不上 ⇒ 重读 manifest，坏的那一份进 broken");

        // 15d. 本进程写过的：盘上那份"大小 + 毫秒"正好撞上新文件，也不许拿旧名字顶上。
        //      （写侧作废那一格，灌盘上缓存时跳过。）
        const Presets::PresetManifest fresh = Presets::create(cachedRoot, QStringLiteral("甲甲甲"));
        check(!fresh.id.isEmpty() && Presets::list(cachedRoot, nullptr).size() == 1,
              "PA15 造一份好的、列一趟（坏的那份掉出去）");
        Presets::forgetSummaryCache(cachedRoot);
        Presets::PresetManifest renamed = fresh;
        renamed.name = QStringLiteral("乙乙乙");  // 与"甲甲甲"同为三字节
        check(Presets::saveManifest(cachedRoot, renamed), "PA15 本进程改名（字节数不变）");
        const QFileInfo renamedInfo(Presets::manifestPath(cachedRoot, fresh.id));
        QJsonObject stale;
        stale.insert(QStringLiteral("id"), fresh.id);
        stale.insert(QStringLiteral("size"), static_cast<double>(renamedInfo.size()));
        stale.insert(QStringLiteral("mtime"), static_cast<double>(renamedInfo.lastModified().toMSecsSinceEpoch()));
        stale.insert(QStringLiteral("listed"), true);
        stale.insert(QStringLiteral("name"), QStringLiteral("甲甲甲"));
        stale.insert(QStringLiteral("description"), QString());
        stale.insert(QStringLiteral("updatedAt"), QString());
        stale.insert(QStringLiteral("sections"), 0);
        stale.insert(QStringLiteral("items"), 0);
        stale.insert(QStringLiteral("componentItems"), QJsonObject());
        stale.insert(QStringLiteral("category"), QString());
        stale.insert(QStringLiteral("components"), QJsonArray());
        stale.insert(QStringLiteral("icon"), QString());
        writeFileAt(cachePath, QJsonDocument(QJsonObject{
                                     {QStringLiteral("format"), QStringLiteral("wtfml.preset-summaries")},
                                     {QStringLiteral("version"), 2},
                                     {QStringLiteral("entries"), QJsonArray{stale}}})
                                 .toJson(QJsonDocument::Compact));
        bool sawRenamed = false;
        for (const Presets::PresetSummary &one : Presets::list(cachedRoot)) {
            if (one.id == fresh.id) {
                sawRenamed = (one.name == renamed.name);
            }
        }
        check(sawRenamed,
              "★★ PA15 本进程写过的那一份不被盘上摘要顶掉（大小/时刻撞上也只认新读的）");

        Presets::forgetSummaryCache(cachedRoot);
        writeFileAt(cachePath, QJsonDocument(QJsonObject{
            {QStringLiteral("format"), QStringLiteral("wtfml.preset-summaries")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("entries"), QJsonArray{stale}}}).toJson(QJsonDocument::Compact));
        bool oldCountCacheIgnored = false;
        for (const auto &one : Presets::list(cachedRoot)) {
            if (one.id == fresh.id) oldCountCacheIgnored = one.name == renamed.name && one.itemCount == 0;
        }
        check(oldCountCacheIgnored, "Older summary cache is rebuilt rather than inventing missing item counts");

        // 15e. 格式标记不认识 / 版本比我们新 ⇒ 当没有缓存，照旧读 manifest。
        Presets::forgetSummaryCache(cachedRoot);
        writeFileAt(cachePath, QJsonDocument(QJsonObject{
                                     {QStringLiteral("format"), QStringLiteral("别的东西")},
                                     {QStringLiteral("version"), 2},
                                     {QStringLiteral("entries"), QJsonArray{stale}}})
                                 .toJson(QJsonDocument::Compact));
        bool cacheIgnored = false;
        for (const Presets::PresetSummary &one : Presets::list(cachedRoot)) {
            if (one.id == fresh.id) {
                cacheIgnored = (one.name == renamed.name);
            }
        }
        check(cacheIgnored, "★ PA15 格式标记不认识的那份当没有缓存（不拿它当数据）");
    }

    // ---------- PA16. 内部投影：先核对引用，再回收 ----------
    {
        QTemporaryDir dir;
        check(dir.isValid(), "PA16 临时预设根目录");
        if (dir.isValid()) {
            const auto internalPart = [](const QString &name) {
                Presets::PresetManifest part;
                part.id = Presets::newPresetId();
                part.name = name;
                part.category = Presets::PresetCategory::GameSettings;
                part.internalComponent = true;
                part.createdAt = part.updatedAt = QDateTime::currentDateTime();
                return part;
            };
            const auto parentWith = [](const QString &root, const QString &name,
                                       const QJsonObject &components) {
                Presets::PresetManifest parent = Presets::create(root, name);
                parent.editorState.insert(QStringLiteral("components"), components);
                Presets::saveManifest(root, parent);
                return parent;
            };

            // 16a. 发布 / 重新发布 / 删除父预设：子项只在没人引用时跟着走。
            const QString rootA = dir.path() + QStringLiteral("/a");
            QJsonObject doc{{"id", Presets::newPresetId()}, {"name", QStringLiteral("综合")}};
            doc["sections"] = QJsonArray{QJsonObject{{"id", "vanillaOptions"}, {"kind", "vanillaOptions"},
                {"items", QJsonArray{QJsonObject{{"key", "guiScale"}, {"value", "2"}}}}}};
            doc["modReferences"] = QJsonArray{QJsonObject{{"platform", "modrinth"},
                {"projectId", "sodium"}, {"name", "Sodium"}}};
            const auto first = Presets::saveEditorDocument(rootA, doc, false);
            const QJsonObject firstParts =
                first.manifest.editorState.value("components").toObject();
            const QString firstGame = firstParts.value("gameSettings").toString();
            const QString firstMods = firstParts.value("mods").toString();
            check(first.ok && !firstGame.isEmpty() && !firstMods.isEmpty(),
                  "PA16 综合预设发布出两个内部投影");
            const Presets::OrphanComponents keep = Presets::orphanComponentIds(rootA);
            check(keep.unreadable == 0 && keep.ids.isEmpty(),
                  "★★ PA16 刚发布的两份都有人引用，核对结果为空");
            check(Presets::sweepOrphanComponents(rootA).removedIds.isEmpty(),
                  "PA16 回收那一趟一份都不动");

            const auto second = Presets::saveEditorDocument(rootA, doc, false);
            const QJsonObject secondParts =
                second.manifest.editorState.value("components").toObject();
            check(second.ok && !QFileInfo::exists(Presets::presetDir(rootA, firstGame)) &&
                      !QFileInfo::exists(Presets::presetDir(rootA, firstMods)),
                  "★★ PA16 重新发布换上新的投影，上一版那两份整棵移进回收站");
            check(Presets::list(rootA).size() == 1,
                  "★★ PA16 回收子项之后列表里还是只有那一份公开预设");
            const QString secondGame = secondParts.value("gameSettings").toString();
            const QString secondMods = secondParts.value("mods").toString();
            check(Presets::remove(rootA, doc.value("id").toString()) &&
                      !QFileInfo::exists(Presets::presetDir(rootA, secondGame)) &&
                      !QFileInfo::exists(Presets::presetDir(rootA, secondMods)),
                  "★★ PA16 删掉父预设，只剩它引用的那两份投影跟着回收");

            // 16b. 两份父预设共享一个子项：删掉其中一份不许带走共享的那份。
            const QString rootB = dir.path() + QStringLiteral("/b");
            const Presets::PresetManifest shared = internalPart(QStringLiteral("共享子项"));
            const Presets::PresetManifest onlyB = internalPart(QStringLiteral("乙的子项"));
            check(Presets::saveManifest(rootB, shared) && Presets::saveManifest(rootB, onlyB),
                  "PA16 造两份内部投影");
            const Presets::PresetManifest parentA = parentWith(
                rootB, QStringLiteral("甲"),
                QJsonObject{{QStringLiteral("gameSettings"), shared.id}});
            const Presets::PresetManifest parentB = parentWith(
                rootB, QStringLiteral("乙"),
                QJsonObject{{QStringLiteral("gameSettings"), shared.id},
                            {QStringLiteral("mods"), onlyB.id}});
            check(Presets::orphanComponentIds(rootB).ids.isEmpty(),
                  "★★ PA16 两个父预设都引用着的子项不算孤儿");
            check(Presets::remove(rootB, parentB.id) &&
                      QFileInfo::exists(Presets::presetDir(rootB, shared.id)) &&
                      !QFileInfo::exists(Presets::presetDir(rootB, onlyB.id)),
                  "★★ PA16 只回收被删那份独占的投影，共享的那份留下");
            check(Presets::remove(rootB, parentA.id) &&
                      !QFileInfo::exists(Presets::presetDir(rootB, shared.id)),
                  "★★ PA16 最后一个父预设删掉，共享投影才回收");

            // 16c. 默认项与下标顺序表都算「有人要」，不拿它们去回收。
            const QString rootC = dir.path() + QStringLiteral("/c");
            const Presets::PresetManifest asDefault = internalPart(QStringLiteral("当默认的投影"));
            check(Presets::saveManifest(rootC, asDefault) &&
                      Presets::setDefault(rootC, Presets::PresetCategory::ResourcePacks,
                                          asDefault.id),
                  "PA16 把一份内部投影设成这一类默认项");
            check(Presets::sweepOrphanComponents(rootC).removedIds.isEmpty() &&
                      QFileInfo::exists(Presets::presetDir(rootC, asDefault.id)),
                  "★★ PA16 下标里当默认项的那份不回收（默认项不是\"没人引用\"）");
            check(Presets::setDefault(rootC, Presets::PresetCategory::ResourcePacks, QString()),
                  "PA16 取消这一类默认");
            check(Presets::sweepOrphanComponents(rootC).removedIds.contains(asDefault.id) &&
                      !QFileInfo::exists(Presets::presetDir(rootC, asDefault.id)),
                  "★★ PA16 取消默认之后它才成为孤儿");

            const Presets::PresetManifest inOrder = internalPart(QStringLiteral("序列表里的投影"));
            bool orderIndexOk = false;
            Presets::PresetIndex orderIndex = Presets::loadIndex(rootC, &orderIndexOk);
            check(orderIndexOk && Presets::saveManifest(rootC, inOrder) &&
                      Presets::saveIndex(rootC, Presets::appendToIndex(orderIndex, inOrder.id)),
                  "PA16 把一份内部投影写进下标顺序表");
            check(Presets::sweepOrphanComponents(rootC).removedIds.isEmpty() &&
                      QFileInfo::exists(Presets::presetDir(rootC, inOrder.id)),
                  "★★ PA16 下标顺序表里出现的一律不动（不替用户改下标）");

            // 16d. 有一份读不回来 ⇒ 不知道它引用了谁，整趟一个都不回收。
            const QString rootD = dir.path() + QStringLiteral("/d");
            const Presets::PresetManifest orphanD = internalPart(QStringLiteral("没人引用"));
            const Presets::PresetManifest broken = Presets::create(rootD, QStringLiteral("坏的那份"));
            check(Presets::saveManifest(rootD, orphanD) && !broken.id.isEmpty(),
                  "PA16 造一个孤儿和一个稍后读不回来的预设");
            writeFileAt(Presets::manifestPath(rootD, broken.id), QByteArrayLiteral("{ 这不是 json"));
            const Presets::OrphanComponents unreadableAudit = Presets::orphanComponentIds(rootD);
            check(unreadableAudit.unreadable > 0 && unreadableAudit.ids.isEmpty(),
                  "★★ PA16 有一份读不回来，核对结果为空");
            const Presets::OrphanSweepResult unreadableSweep =
                Presets::sweepOrphanComponents(rootD);
            check(unreadableSweep.unreadable > 0 && unreadableSweep.removedIds.isEmpty() &&
                      QFileInfo::exists(Presets::presetDir(rootD, orphanD.id)),
                  "★★ PA16 读不回来的情况下不回收任何投影（不知道它引用了谁）");

            // 16e. 保存失败那条路：只收「这一趟刚建出来」的那几份。
            const QString rootE = dir.path() + QStringLiteral("/e");
            const Presets::PresetManifest failedPart =
                internalPart(QStringLiteral("失败那趟落下的"));
            const Presets::PresetManifest normal = Presets::create(rootE, QStringLiteral("普通"));
            const QString half = Presets::newPresetId();
            QDir().mkpath(Presets::filesDir(rootE, half));
            check(Presets::saveManifest(rootE, failedPart),
                  "PA16 造一份保存失败那趟会落下的投影");
            check(Presets::discardInternalComponent(rootE, failedPart.id) &&
                      !QFileInfo::exists(Presets::presetDir(rootE, failedPart.id)),
                  "PA16 失败那趟刚建出的投影可以单独收掉");
            check(Presets::discardInternalComponent(rootE, half) &&
                      !QFileInfo::exists(Presets::presetDir(rootE, half)),
                  "PA16 拷贝到一半、还没有清单的目录也能收");
            check(!Presets::discardInternalComponent(rootE, normal.id) &&
                      QFileInfo::exists(Presets::presetDir(rootE, normal.id)),
                  "★★ PA16 不是内部投影的那份，单独收口的入口也不碰");
        }
    }
}

