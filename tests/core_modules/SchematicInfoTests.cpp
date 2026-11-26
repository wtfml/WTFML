#include "Support.hpp"
#include "TextMarks.hpp"

void runSchematicInfoTests() {
    section("SG. 结构 / 原理图的元数据与方块统计");

    const auto compoundNode = [] {
        NbtTree::Node node;
        node.type = NbtTree::Type::Compound;
        return node;
    };
    const auto intNode = [](qint64 value) {
        NbtTree::Node node;
        node.type = NbtTree::Type::Int;
        node.number = value;
        return node;
    };
    const auto stringNode = [](const QString &text) {
        NbtTree::Node node;
        node.type = NbtTree::Type::String;
        node.text = text;
        return node;
    };
    const auto addField = [](NbtTree::Node *parent, const QString &name,
                             const NbtTree::Node &child) {
        parent->fields.append(qMakePair(name, child));
    };
    const auto writeTree = [](const QString &path, const NbtTree::Node &root) {
        NbtTree::Document document;
        document.root = root;
        writeFileAt(path, NbtTree::serialize(document));
    };
    // Litematica 的位打包（高位在前）。**测试自己的一份**：实现那边有一份同样的算法，
    // 两边独立写才对得上"跨 long"那一条的意义。
    const auto packLongs = [](const QList<int> &values, int bits, qint64 volume) {
        QList<qint64> words;
        words.fill(0, static_cast<int>((volume * bits + 63) / 64));
        const quint64 mask = (quint64(1) << bits) - 1;
        for (int i = 0; i < values.size(); ++i) {
            const quint64 value = quint64(values.at(i)) & mask;
            const qint64 start = qint64(i) * bits;
            const qint64 wordIndex = start >> 6;
            const int offset = int(start & 63);
            if (offset + bits <= 64) {
                words[int(wordIndex)] |= qint64(value << (64 - offset - bits));
            } else {
                const int firstBits = 64 - offset;
                const int rest = bits - firstBits;
                words[int(wordIndex)] |= qint64(value >> rest);
                words[int(wordIndex) + 1] |=
                    qint64((value & ((quint64(1) << rest) - 1)) << (64 - rest));
            }
        }
        return words;
    };

    QTemporaryDir dir;
    check(dir.isValid(), "SG0 临时目录");
    if (!dir.isValid()) {
        return;
    }
    const QString root = dir.path();
    {
        const auto assets = root + QStringLiteral("/assets");
        const QString hash(40, QLatin1Char('a'));
        const QString englishHash(40, QLatin1Char('b'));
        QDir().mkpath(assets + QStringLiteral("/indexes"));
        QDir().mkpath(assets + QStringLiteral("/objects/aa"));
        QDir().mkpath(assets + QStringLiteral("/objects/bb"));
        writeFileAt(assets + QStringLiteral("/indexes/current.json"),
                    QStringLiteral("{\"objects\":{\"minecraft/lang/zh_cn.json\":{\"hash\":\"%1\"},"
                                   "\"minecraft/lang/en_us.json\":{\"hash\":\"%2\"}}}")
                        .arg(hash, englishHash).toUtf8());
        writeFileAt(assets + QStringLiteral("/objects/aa/") + hash,
                    QStringLiteral("{\"block.minecraft.stone\":\"石头\",\"block.example.blue.metal\":\"蓝色金属\"}").toUtf8());
        writeFileAt(assets + QStringLiteral("/objects/bb/") + englishHash,
                    QStringLiteral("{\"block.minecraft.stone\":\"Stone\",\"block.example.blue.metal\":\"Blue Metal\"}").toUtf8());
        const auto names = SchematicInfo::loadChineseBlockNames(assets, QStringLiteral("current"));
        check(SchematicInfo::chineseBlockName(names, QStringLiteral("minecraft:stone")) == QStringLiteral("石头") &&
                  SchematicInfo::chineseBlockName(names, QStringLiteral("example:blue/metal")) == QStringLiteral("蓝色金属") &&
                  SchematicInfo::chineseBlockName(names, QStringLiteral("missing:rock")).isEmpty(),
              "原理图材料从本地语言资源取中文名，未知方块由界面保留 ID");
        const auto english = SchematicInfo::loadBlockNames(assets, QStringLiteral("current"), QStringLiteral("en_us"));
        check(SchematicInfo::chineseBlockName(english, QStringLiteral("minecraft:stone")) == QStringLiteral("Stone") &&
                  SchematicInfo::chineseBlockName(english, QStringLiteral("example:blue/metal")) == QStringLiteral("Blue Metal"),
              "原理图材料按界面语言取名称：英文界面读 en_us.json");
        check(SchematicInfo::loadBlockNames(assets, QStringLiteral("current"), QStringLiteral("../outside")).isEmpty(),
              "非法语言代号不会读到资源目录之外");
        check(SchematicInfo::loadChineseBlockNames(assets, QStringLiteral("../outside")).isEmpty(),
              "非法资源索引号不会读到资源目录之外");
    }
    const auto namedEntry = [&](const QString &name) {
        NbtTree::Node entry = compoundNode();
        addField(&entry, QStringLiteral("Name"), stringNode(name));
        return entry;
    };
    const auto sizeCompound = [&](qint64 x, qint64 y, qint64 z) {
        NbtTree::Node size = compoundNode();
        addField(&size, QStringLiteral("x"), intNode(x));
        addField(&size, QStringLiteral("y"), intNode(y));
        addField(&size, QStringLiteral("z"), intNode(z));
        return size;
    };

    // ---------- SG1. 原版结构（palette + blocks）----------
    {
        NbtTree::Node structure = compoundNode();
        NbtTree::Node size;
        size.type = NbtTree::Type::List;
        size.itemType = NbtTree::Type::Int;
        const int kinds = SchematicInfo::kTopBlockLimit + 5;
        size.items = {intNode(kinds), intNode(1), intNode(1)};
        addField(&structure, QStringLiteral("size"), size);
        NbtTree::Node palette, blocks;
        palette.type = blocks.type = NbtTree::Type::List;
        palette.itemType = blocks.itemType = NbtTree::Type::Compound;
        for (int index = 0; index < kinds; ++index) {
            palette.items.append(namedEntry(QStringLiteral("test:block_%1").arg(index)));
            auto block = compoundNode();
            addField(&block, QStringLiteral("state"), intNode(index));
            blocks.items.append(block);
        }
        addField(&structure, QStringLiteral("palette"), palette);
        addField(&structure, QStringLiteral("blocks"), blocks);
        const auto path = root + QStringLiteral("/all-block-kinds.nbt");
        writeTree(path, structure);
        const auto info = SchematicInfo::read(path);
        check(info.ok && info.countsComplete && info.blocks.size() == kinds
              && info.topBlocks.size() == SchematicInfo::kTopBlockLimit,
              "SG1 完整详情保留排行截断以外的全部方块种类", info.problem);
    }
    {
        NbtTree::Node structure = compoundNode();
        NbtTree::Node size;
        size.type = NbtTree::Type::List;
        size.itemType = NbtTree::Type::Int;
        size.items.append(intNode(3));
        size.items.append(intNode(2));
        size.items.append(intNode(4));
        addField(&structure, QStringLiteral("size"), size);

        NbtTree::Node palette;
        palette.type = NbtTree::Type::List;
        palette.itemType = NbtTree::Type::Compound;
        palette.items.append(namedEntry(QStringLiteral("minecraft:air")));
        palette.items.append(namedEntry(QStringLiteral("minecraft:stone")));
        palette.items.append(namedEntry(QStringLiteral("minecraft:oak_planks")));
        addField(&structure, QStringLiteral("palette"), palette);

        NbtTree::Node blocks;
        blocks.type = NbtTree::Type::List;
        blocks.itemType = NbtTree::Type::Compound;
        for (const int state : {0, 1, 1, 2, 2, 2}) {
            NbtTree::Node block = compoundNode();
            addField(&block, QStringLiteral("state"), intNode(state));
            blocks.items.append(block);
        }
        addField(&structure, QStringLiteral("blocks"), blocks);
        addField(&structure, QStringLiteral("DataVersion"), intNode(3465));

        const QString path = root + QStringLiteral("/house.nbt");
        writeTree(path, structure);
        const SchematicInfo::Info info = SchematicInfo::read(path);
        check(info.ok && info.problem.isEmpty()
                  && info.format == SchematicInfo::Format::Structure,
              "SG1 原版结构：认得出是 `palette` + `blocks` 那一档",
              info.ok ? QString() : info.problem);
        check(info.width == 3 && info.height == 2 && info.length == 4 && info.volume == 24,
              "SG1 `size` 三个数就是 宽 × 高 × 长（体积 24）",
              QStringLiteral("%1×%2×%3/%4")
                  .arg(info.width).arg(info.height).arg(info.length).arg(info.volume));
        check(info.blocks.size() == 2 && info.blocks.at(0).count + info.blocks.at(1).count == info.blockCount,
              "SG1 完整方块清单数量与非空气总数一致");
        check(info.paletteSize == 3, "SG1 调色板条目数（**含空气**那一格）",
              QString::number(info.paletteSize));
        check(info.countsComplete && info.blockCount == 5,
              "★ SG1 方块数**不含空气**：6 个里 1 个 air ⇒ 5",
              QString::number(info.blockCount));
        check(info.topBlocks.size() == 2 && info.topBlocks.at(0).id == QStringLiteral("minecraft:oak_planks")
                  && info.topBlocks.at(0).count == 3 && info.topBlocks.at(1).count == 2,
              "★ SG1 排行按个数降序（3 个木板压过 2 个石头）",
              info.topBlocks.isEmpty() ? QStringLiteral("空") : info.topBlocks.at(0).id);
        const QString summary = SchematicInfo::summaryText(info, QStringLiteral("1.20.1-Forge_47.2.0"));
        check(summary.contains(QStringLiteral("3 × 2 × 4"))
                  && summary.contains(QStringLiteral("5 个方块"))
                  && summary.contains(QStringLiteral("1.20.1（数据版本 3465）")),
              "★ SG1 那一行字：尺寸 / 方块数 / 数据版本对应的游戏版本",
              summary);
        check(!summary.contains(TextMarks::warning()),
              "SG1 同一个版本（1.20.1-Forge_47.2.0）⇒ **不出**兼容提示");
        const QString other = SchematicInfo::summaryText(info, QStringLiteral("1.18.2"));
        check(other.contains(TextMarks::warning()) && other.contains(QStringLiteral("1.18.2")),
              "★ SG1 换到 1.18.2 ⇒ 明说「来自哪一版、实例是哪一版」，不把话说死",
              other);
        check(SchematicInfo::topBlocksText(info).contains(QStringLiteral("数量最多的方块："))
                  && SchematicInfo::topBlocksText(info).contains(QStringLiteral("minecraft:oak_planks：3")),
              "SG1 悬停提示里的方块排行",
              SchematicInfo::topBlocksText(info));
        QJsonObject language;
        language.insert(QStringLiteral("block.minecraft.oak_planks"), QStringLiteral("橡木木板"));
        const QString localized = SchematicInfo::topBlocksText(info, language);
        check(localized.contains(QStringLiteral("橡木木板：3")) && !localized.contains(QStringLiteral("minecraft:oak_planks")),
              "SG1 方块排行有中文名时显示中文名", localized);
        check(SchematicInfo::topBlocksText(info, QJsonObject()).contains(QStringLiteral("minecraft:oak_planks：3")),
              "SG1 方块排行取不到中文名时回退显示 ID");
    }

    // ---------- SG2. Sponge（Palette + varint BlockData）----------
    const auto buildSponge = [&]() {
        NbtTree::Node sponge = compoundNode();
        addField(&sponge, QStringLiteral("Width"), intNode(2));
        addField(&sponge, QStringLiteral("Height"), intNode(1));
        addField(&sponge, QStringLiteral("Length"), intNode(2));
        addField(&sponge, QStringLiteral("DataVersion"), intNode(2586));
        NbtTree::Node metadata = compoundNode();
        addField(&metadata, QStringLiteral("Name"), stringNode(QStringLiteral("测试原理图")));
        addField(&metadata, QStringLiteral("Author"), stringNode(QStringLiteral("作者甲")));
        addField(&sponge, QStringLiteral("Metadata"), metadata);
        NbtTree::Node palette = compoundNode();
        addField(&palette, QStringLiteral("minecraft:air"), intNode(0));
        for (int i = 1; i < 130; ++i) {
            addField(&palette, QStringLiteral("minecraft:test%1").arg(i), intNode(i));
        }
        addField(&sponge, QStringLiteral("Palette"), palette);
        return sponge;
    };
    {
        NbtTree::Node sponge = buildSponge();
        NbtTree::Node blockData;
        blockData.type = NbtTree::Type::ByteArray;
        // 0（空气）、1（单字节）、128（0x80 0x01 —— **两字节的 varint**）
        blockData.bytes = QByteArray::fromHex("00018001");
        addField(&sponge, QStringLiteral("BlockData"), blockData);
        const QString path = root + QStringLiteral("/sponge.schematic");
        writeTree(path, sponge);
        const SchematicInfo::Info info = SchematicInfo::read(path);
        check(info.ok && info.format == SchematicInfo::Format::SpongeSchematic,
              "SG2 Sponge：认得出 `Palette` + `BlockData` 那一档",
              info.ok ? QString() : info.problem);
        check(info.paletteSize == 130 && info.volume == 4,
              "SG2 调色板 130 条 / 体积 2×1×2",
              QStringLiteral("%1 / %2").arg(info.paletteSize).arg(info.volume));
        check(info.countsComplete && info.blockCount == 2,
              "★ SG2 方块数：空气不算；剩下两格分别是下标 1 和**两字节写出来的** 128",
              QStringLiteral("%1 个 / 完整 %2").arg(info.blockCount).arg(info.countsComplete));
        check(info.topBlocks.size() == 2
                  && info.topBlocks.at(0).id == QStringLiteral("minecraft:test1")
                  && info.topBlocks.at(1).id == QStringLiteral("minecraft:test128"),
              "★★ SG2 两字节 varint 解出来的正是 128（下标没被读成 0）",
              info.topBlocks.size() == 2 ? info.topBlocks.at(1).id : QStringLiteral("少"));
        const QString summary = SchematicInfo::summaryText(info, QString());
        check(summary.contains(QStringLiteral("「测试原理图」"))
                  && summary.contains(QStringLiteral("作者 作者甲"))
                  && summary.contains(QStringLiteral("1.16.4（数据版本 2586）")),
              "★ SG2 名字 / 作者 / 版本都在那一行里",
              summary);
    }

    // ---------- SG4. Litematica（多 region + 位打包，含跨 long）----------
    {
        NbtTree::Node litematic = compoundNode();
        addField(&litematic, QStringLiteral("MinecraftDataVersion"), intNode(2865));
        NbtTree::Node metadata = compoundNode();
        addField(&metadata, QStringLiteral("Name"), stringNode(QStringLiteral("小房子")));
        addField(&metadata, QStringLiteral("Author"), stringNode(QStringLiteral("作者乙")));
        addField(&metadata, QStringLiteral("EnclosingSize"), sizeCompound(9, 5, 1));
        addField(&litematic, QStringLiteral("Metadata"), metadata);

        NbtTree::Node regions = compoundNode();

        // region A：体积 24、调色板 3 条 ⇒ 2 位一组（48 位正好 1 个 long）
        NbtTree::Node regionA = compoundNode();
        addField(&regionA, QStringLiteral("Size"), sizeCompound(4, 3, 2));
        NbtTree::Node paletteA;
        paletteA.type = NbtTree::Type::List;
        paletteA.itemType = NbtTree::Type::Compound;
        paletteA.items.append(namedEntry(QStringLiteral("minecraft:air")));
        paletteA.items.append(namedEntry(QStringLiteral("minecraft:stone")));
        paletteA.items.append(namedEntry(QStringLiteral("minecraft:oak_stairs[facing=north]")));
        addField(&regionA, QStringLiteral("BlockStatePalette"), paletteA);
        QList<int> valuesA;
        valuesA.append(0);
        for (int i = 0; i < 21; ++i) {
            valuesA.append(1);
        }
        valuesA.append(2);
        valuesA.append(2);  // 24 个值：1 空气 + 21 石头 + 2 楼梯
        NbtTree::Node statesA;
        statesA.type = NbtTree::Type::LongArray;
        statesA.longs = packLongs(valuesA, 2, 24);
        addField(&regionA, QStringLiteral("BlockStates"), statesA);
        addField(&regions, QStringLiteral("main"), regionA);

        // region B：体积 40、调色板 17 条 ⇒ 5 位一组；第 13 个值从第 60 位开始（**跨 long**）
        NbtTree::Node regionB = compoundNode();
        addField(&regionB, QStringLiteral("Size"), sizeCompound(8, 5, 1));
        NbtTree::Node paletteB;
        paletteB.type = NbtTree::Type::List;
        paletteB.itemType = NbtTree::Type::Compound;
        paletteB.items.append(namedEntry(QStringLiteral("minecraft:air")));
        for (int i = 1; i <= 16; ++i) {
            paletteB.items.append(namedEntry(QStringLiteral("minecraft:b%1").arg(i)));
        }
        addField(&regionB, QStringLiteral("BlockStatePalette"), paletteB);
        QList<int> valuesB;
        valuesB.fill(0, 40);
        valuesB[12] = 3;
        valuesB[13] = 4;
        valuesB[39] = 16;
        NbtTree::Node statesB;
        statesB.type = NbtTree::Type::LongArray;
        statesB.longs = packLongs(valuesB, 5, 40);
        addField(&regionB, QStringLiteral("BlockStates"), statesB);
        addField(&regions, QStringLiteral("wing"), regionB);

        addField(&litematic, QStringLiteral("Regions"), regions);

        const QString path = root + QStringLiteral("/house.litematic");
        writeTree(path, litematic);
        const SchematicInfo::Info info = SchematicInfo::read(path);
        check(info.ok && info.format == SchematicInfo::Format::Litematic,
              "SG4 Litematica：认得出 `Regions` 那一档",
              info.ok ? QString() : info.problem);
        check(info.name == QStringLiteral("小房子") && info.author == QStringLiteral("作者乙"),
              "SG4 `Metadata` 里的名字 / 作者", info.name);
        check(info.width == 9 && info.height == 5 && info.length == 1,
              "★ SG4 尺寸取 `EnclosingSize`（不是拿 region 边长瞎凑）",
              QStringLiteral("%1×%2×%3").arg(info.width).arg(info.height).arg(info.length));
        check(info.volume == 64, "SG4 体积 = 两个 region 之和（24 + 40）",
              QString::number(info.volume));
        check(info.paletteSize == 20 && info.countsComplete,
              "SG4 调色板条目数按 region 累加（3 + 17）、方块表解出来了",
              QStringLiteral("%1 / %2").arg(info.paletteSize).arg(info.countsComplete));
        check(info.blockCount == 26,
              "★ SG4 方块数：空气两处都不算（23 + 3）",
              QString::number(info.blockCount));
        check(info.topBlocks.size() == 5 && info.topBlocks.at(0).id == QStringLiteral("minecraft:stone")
                  && info.topBlocks.at(0).count == 21
                  && info.topBlocks.at(1).id == QStringLiteral("minecraft:oak_stairs")
                  && info.topBlocks.at(1).count == 2,
              "★ SG4 排行按 id 聚合（状态后缀 `[facing=north]` 被摘掉）",
              info.topBlocks.size() >= 2 ? info.topBlocks.at(1).id : QStringLiteral("少"));
        bool sawB3 = false;
        bool sawB4 = false;
        bool sawB16 = false;
        for (const SchematicInfo::BlockCount &block : info.topBlocks) {
            sawB3 = sawB3 || block.id == QStringLiteral("minecraft:b3");
            sawB4 = sawB4 || block.id == QStringLiteral("minecraft:b4");
            sawB16 = sawB16 || block.id == QStringLiteral("minecraft:b16");
        }
        check(sawB3 && sawB4 && sawB16,
              "★★ SG4 5 位那一档解对了：**跨 long** 的第 13 个值（b3 / b4）和最后一个（b16）都在",
              QStringLiteral("b3=%1 b4=%2 b16=%3").arg(sawB3).arg(sawB4).arg(sawB16));
        const QString summary = SchematicInfo::summaryText(info, QStringLiteral("1.20.1"));
        check(summary.contains(QStringLiteral("「小房子」"))
                  && summary.contains(QStringLiteral("9 × 5 × 1"))
                  && summary.contains(QStringLiteral("1.18.2（数据版本 2865）")),
              "★ SG4 那一行字：名字 / 尺寸 / 版本", summary);
        const QString compat = SchematicInfo::compatibilityText(info, QStringLiteral("1.20.1"));
        check(compat.contains(QStringLiteral("1.18.2"))
                  && compat.contains(QStringLiteral("1.20.1"))
                  && compat.contains(QStringLiteral("部分方块无法识别")),
              "★ SG4 版本不一样 ⇒ 说清两边分别是什么、不把话说死", compat);
    }

    // ---------- SG5. gzip 那一层 + 后缀骗人 ----------
    {
        const QString raw = root + QStringLiteral("/house.litematic");
        const QString gz = root + QStringLiteral("/renamed.schematic");
        QString why;
        check(ZipUtil::gzipFrom(raw, gz, &why), "SG5 把那份 Litematica 压成 gzip", why);
        const SchematicInfo::Info info = SchematicInfo::read(gz);
        check(info.ok && info.format == SchematicInfo::Format::Litematic,
              "★★ SG5 后缀写着 `.schematic`、内容其实是 Litematica ⇒ 按**字段**认，不看后缀",
              info.ok ? SchematicInfo::formatName(info.format) : info.problem);
        const SchematicInfo::Info direct = SchematicInfo::read(raw);
        check(SchematicInfo::summaryText(info, QStringLiteral("1.20.1"))
                  == SchematicInfo::summaryText(direct, QStringLiteral("1.20.1")),
              "★ SG5 gzip 解出来和裸 NBT 逐字一样（解压那一层没吃掉东西）");
    }

    // ---------- SG6. 三种"读不动"与"只读到元数据" ----------
    {
        NbtTree::Node mystery = compoundNode();
        addField(&mystery, QStringLiteral("hello"), stringNode(QStringLiteral("world")));
        const QString mysteryPath = root + QStringLiteral("/mystery.nbt");
        writeTree(mysteryPath, mystery);
        const SchematicInfo::Info info = SchematicInfo::read(mysteryPath);
        check(!info.ok && info.problem.contains(QStringLiteral("无法识别结构文件格式")),
              "★ SG6 根里没有那三格 ⇒ 如实说「认不出」，不猜格式", info.problem);
        check(SchematicInfo::summaryText(info, QString()) == info.problem,
              "SG6 读失败时那一行字就是 `problem` 原文");

        const QString emptyPath = root + QStringLiteral("/empty.nbt");
        writeFileAt(emptyPath, QByteArray());
        const SchematicInfo::Info empty = SchematicInfo::read(emptyPath);
        check(!empty.ok && empty.problem.contains(QStringLiteral("空")),
              "SG6 零字节文件：说「空的」", empty.problem);

        NbtTree::Node small = compoundNode();
        addField(&small, QStringLiteral("size"), intNode(1));
        NbtTree::Document document;
        document.root = small;
        QByteArray truncatedBytes = NbtTree::serialize(document);
        truncatedBytes.chop(4);
        const QString truncatedPath = root + QStringLiteral("/truncated.nbt");
        writeFileAt(truncatedPath, truncatedBytes);
        const SchematicInfo::Info truncated = SchematicInfo::read(truncatedPath);
        check(!truncated.ok && truncated.problem.contains(QStringLiteral("NBT")),
              "★ SG6 截断的 NBT：说「不是能读懂的 NBT」，不硬解半份", truncated.problem);

        const QString hugePath = root + QStringLiteral("/huge.nbt");
        writeFileAt(hugePath, QByteArray("x"));
        QFile huge(hugePath);
        if (huge.open(QIODevice::ReadWrite)) {
            huge.resize(97ll * 1024 * 1024);  // NTFS 上是稀疏的，不真占 97 MB
            huge.close();
        }
        const SchematicInfo::Info oversized = SchematicInfo::read(hugePath);
        check(!oversized.ok && oversized.problem.contains(QStringLiteral("超出预览上限")),
              "★ SG6 超过 96 MB 的：明说太大、不展开读（同步解析不能把界面按在那儿）",
              oversized.problem);

        // Sponge 少了 BlockData：元数据照样读得出来，**方块表那一段如实说不完整**。
        NbtTree::Node sponge = buildSponge();
        const QString noDataPath = root + QStringLiteral("/nodata.schematic");
        writeTree(noDataPath, sponge);
        const SchematicInfo::Info noData = SchematicInfo::read(noDataPath);
        check(noData.ok && !noData.countsComplete && noData.blockCount == 0,
              "★ SG6 只有 Palette、没有 BlockData ⇒ ok 但 `countsComplete` 为假、方块数不编",
              QStringLiteral("ok=%1 完整=%2 数=%3")
                  .arg(noData.ok).arg(noData.countsComplete).arg(noData.blockCount));
        const QString noDataSummary = SchematicInfo::summaryText(noData, QString());
        check(noDataSummary.contains(QStringLiteral("方块数据读取失败")),
              "★ SG6 那种情况那一行字要**明说**只读到元数据", noDataSummary);
    }

    // ---------- SG7. 数据版本那张表 ----------
    {
        check(SchematicInfo::gameVersionOfDataVersion(3465) == QStringLiteral("1.20.1")
                  && SchematicInfo::gameVersionOfDataVersion(4440) == QStringLiteral("1.21.8"),
              "SG7 数据版本 → 正式版号（3465→1.20.1、4440→1.21.8）");
        check(SchematicInfo::gameVersionOfDataVersion(1234).isEmpty(),
              "★ SG7 表里没有的号**只报数字**（不猜成某个版本）");
        check(SchematicInfo::dataVersionOfGameVersion(QStringLiteral("1.20.1-Forge_47.2.0")) == 3465
                  && SchematicInfo::dataVersionOfGameVersion(QStringLiteral("原版 1.18.2 整合")) == 2865,
              "SG7 从实例的显示版本里抠出版本号（前面后面带什么都行）");
        check(SchematicInfo::dataVersionOfGameVersion(QStringLiteral("26.2")) == 0
                  && SchematicInfo::dataVersionOfGameVersion(QStringLiteral("原版ATT2地图（无模组）")) == 0,
              "★ SG7 认不出来就回 0 ⇒ 兼容提示那一句整句不出（宁可不说话也不说错）");
    }
}

