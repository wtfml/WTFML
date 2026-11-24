#include "Support.hpp"

int runInstanceGroupTests(const QString &sandbox) {
    section("Q. 实例分组（wtfml.folders）");

    const QString versionsRoot = sandbox + "/versions";
    const QString groupFile = AppPaths::instanceGroupFilePath();

    // 这个文件只有这一节会造，但"起点是干净的"得自己保证 —— Q1 那两条断言（读到空、
    // 光读不写盘）都建立在"文件不存在"之上。
    QDir().remove(groupFile);
    check(!QFile::exists(groupFile), "Q1 前提：沙盒里本来没有 wtfml.folders");

    const auto readBytes = [&]() {
        QFile file(groupFile);
        if (!file.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        const QByteArray bytes = file.readAll();
        file.close();  // Windows：别攥着句柄，后面还要删它
        return bytes;
    };
    const auto writeRaw = [&](const QByteArray &bytes) { writeFileAt(groupFile, bytes); };
    const auto readRaw = [&]() { return QJsonDocument::fromJson(readBytes()).object(); };

    const QString packId = QStringLiteral("11111111-1111-1111-1111-111111111111");
    const QString modId = QStringLiteral("22222222-2222-2222-2222-222222222222");

    // ---------- Q1 没有文件时读到空，而且不写盘 ----------
    {
        InstanceGroupStore store;
        check(store.groups().isEmpty(), "Q1 没有文件时组清单是空的");
        check(store.groupIdOf(QStringLiteral("随便什么实例")).isEmpty(), "Q1 归属也是空的");
        check(!QFile::exists(groupFile), "Q1 光是读不会把文件造出来");
    }

    // ---------- Q2 建两个组：落盘、读得回来、读不发声 ----------
    {
        InstanceGroupStore store;
        int changed = 0;
        QObject::connect(&store, &InstanceGroupStore::groupsChanged, [&changed] { ++changed; });

        QList<InstanceGroupStore::Group> groups;
        groups.append({packId, QStringLiteral("整合包")});
        groups.append({modId, QStringLiteral("模组")});
        QString error;
        check(store.replaceGroups(groups, &error), "Q2 写入两个组成功", error);
        check(changed == 1, "Q2 写成功发了一次 groupsChanged", QString::number(changed));
        check(QFile::exists(groupFile), "Q2 文件落盘了");
    }
    {
        InstanceGroupStore other;
        int changed = 0;
        QObject::connect(&other, &InstanceGroupStore::groupsChanged, [&changed] { ++changed; });

        const QList<InstanceGroupStore::Group> groups = other.groups();
        check(groups.size() == 2, "Q2 另一个对象读回来两个组", QString::number(groups.size()));
        check(!groups.isEmpty() && groups.at(0).id == packId &&
                  groups.at(0).name == QStringLiteral("整合包"),
              "Q2 第一个组的 id 和名字都对");
        check(groups.size() > 1 && groups.at(1).id == modId &&
                  groups.at(1).name == QStringLiteral("模组"),
              "Q2 第二个组也对，而且顺序就是用户排的那个（不是按 id 排）");
        check(readRaw().value(QStringLiteral("version")).toInt() == 1,
              "Q2 文件里带着格式版本 1");
        check(changed == 0, "Q2 光是读不发 groupsChanged", QString::number(changed));
    }

    // ---------- Q3 不认识的字段原样留着 ----------
    // 模拟"更新版本的启动器往里写了别的东西"：我们碰一次之后，那些字段必须还在文件里。
    // 这是我们唯一能做的向后兼容 —— 反过来（把不认识的键抹掉）在用户那边的表现是
    // "换回旧版启动器打开一次，新版的分组颜色/图标就没了"，而且完全无声。
    {
        QJsonObject root = readRaw();
        root.insert(QStringLiteral("将来的字段"), QStringLiteral("别弄丢我"));
        writeRaw(QJsonDocument(root).toJson(QJsonDocument::Indented));

        InstanceGroupStore store;
        QList<InstanceGroupStore::Group> groups = store.groups();
        check(groups.size() == 2, "Q3 前提：带着未知字段的文件照样读得出两个组");
        groups[0].name = QStringLiteral("整合包（改过名）");
        QString error;
        check(store.replaceGroups(groups, &error), "Q3 改完组名写回去成功", error);

        const QJsonObject after = readRaw();
        check(after.value(QStringLiteral("将来的字段")).toString() == QStringLiteral("别弄丢我"),
              "Q3 不认识的字段没被抹掉");
        check(after.value(QStringLiteral("groups")).toArray().at(0).toObject()
                  .value(QStringLiteral("name")).toString() == QStringLiteral("整合包（改过名）"),
              "Q3 名字确实改了");
        const QList<InstanceGroupStore::Group> reread = InstanceGroupStore().groups();
        check(reread.size() == 2 && reread.at(0).id == packId,
              "Q3 改名不动 id —— 归属挂的是 id，不是名字");
    }

    // ---------- Q3b 手改过的归属（值两端多了空格）：读得出来，而且下一次写盘不许丢 ----------
    // 读侧（groupIdOf）读的是 trim 过的值，写侧（writeThrough）原来**不 trim** 就把它拿去
    // 和清洗过的组 id 比 —— 两边不一致时，记事本里多敲两个空格的那一条在界面上是一条完全
    // 正常的归属，而**任何一次写盘**（改别的实例的分组就会重写整份文件）都会把它当成
    // "指向一个不存在的组"悄悄清掉：用户下次打开就是"这个实例怎么又回未分组了"，
    // 而且没有任何提示（ 修，写侧那一句见 writeThrough）。
    //
    // NOTE: 这一小段把**整份文件**换成它自己那一份（要的就是"手改过的文件"那个形状），
    // 所以先留一份原样，验完再写回去 —— 下一节 Q4 用的那两个组就在里面，
    // 不还回去的话它会以"找不到那个分组"红给你看（而那是这一段的锅）。
    const QByteArray q3bBefore = readBytes();
    {
        const QString paddedKey = QStringLiteral("Q3b-实例");
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        QJsonArray groupArray;
        QJsonObject groupObject;
        groupObject.insert(QStringLiteral("id"), QStringLiteral("q3b-group"));
        groupObject.insert(QStringLiteral("name"), QStringLiteral("手改的组"));
        groupArray.append(groupObject);
        root.insert(QStringLiteral("groups"), groupArray);
        QJsonObject membersObject;
        membersObject.insert(paddedKey, QStringLiteral("  q3b-group  "));
        root.insert(QStringLiteral("members"), membersObject);
        writeRaw(QJsonDocument(root).toJson(QJsonDocument::Indented));

        InstanceGroupStore reader;
        check(reader.groupIdOf(paddedKey) == QStringLiteral("q3b-group"),
              "Q3b 值两端带空格的归属读得出来（读侧本来就 trim）",
              reader.groupIdOf(paddedKey));
        // 写一次盘：给**另一个**实例归组，整份文件就会被重写（用户那边就是这个形状）。
        QString error;
        check(reader.assign(QStringLiteral("Q3b-别的实例"), QStringLiteral("q3b-group"), &error),
              "Q3b 顺手改一个别的实例的归属（这一次写盘会把整份文件重写一遍）", error);
    }
    {
        // **换一个对象**读回来：同一个对象读的是它内存里那份，盘上丢没丢看不出来。
        InstanceGroupStore after;
        check(after.groupIdOf(QStringLiteral("Q3b-实例")) == QStringLiteral("q3b-group"),
              "Q3b 写盘之后那一条还在（不许当成「指向不存在的组」清掉）",
              after.groupIdOf(QStringLiteral("Q3b-实例")));
        check(after.groupIdOf(QStringLiteral("Q3b-别的实例")) == QStringLiteral("q3b-group"),
              "Q3b 这一次新归的那条也在");
        writeRaw(q3bBefore);
        check(InstanceGroupStore().groupIdOf(QStringLiteral("Q3b-实例")).isEmpty(),
              "Q3b 收尾：把文件还回去（Q4 那一节用的两个组就在那一份里）");
    }

    // ---------- Q4 归组 ----------
    {
        InstanceGroupStore store;
        QString error;
        check(store.assign(QStringLiteral("Q4-实例A"), packId, &error), "Q4 归组成功", error);
        check(store.assign(QStringLiteral("Q4-实例B"), packId, &error),
              "Q4 第二个实例也归进同一个组", error);
        check(store.groupIdOf(QStringLiteral("Q4-实例A")) == packId, "Q4 同一个对象里读得到");
        check(store.groupIdOf(QStringLiteral("Q4-实例C")).isEmpty(), "Q4 没归过的是未分组");
    }
    {
        InstanceGroupStore other;
        check(other.groupIdOf(QStringLiteral("Q4-实例A")) == packId,
              "Q4 另一个对象也读得到（真的落盘了）");
        check(other.groupIdOf(QStringLiteral("Q4-实例B")) == packId, "Q4 实例 B 也是");
        const QString stored = readRaw().value(QStringLiteral("members")).toObject()
                                   .value(QStringLiteral("Q4-实例A")).toString();
        check(stored == packId, "Q4 成员表里存的就是组 id（不是组名）", stored);
    }

    // ---------- Q5 改名搬运（rekey） ----------
    {
        InstanceGroupStore store;
        QString error;
        check(store.rekey(QStringLiteral("Q4-实例A"), QStringLiteral("Q5-实例A-新名"), &error),
              "Q5 改名搬运成功", error);
    }
    {
        InstanceGroupStore other;
        check(other.groupIdOf(QStringLiteral("Q4-实例A")).isEmpty(), "Q5 老键上没有归属了");
        check(other.groupIdOf(QStringLiteral("Q5-实例A-新名")) == packId, "Q5 归属搬到新键上");
        check(other.groupIdOf(QStringLiteral("Q4-实例B")) == packId,
              "Q5 别人的归属一个都没动");
    }

    // ---------- Q6 无事可做的几个空操作**一个字节都不写盘** ----------
    // 这条只能这么验：**把文件删掉**，再调那几个空操作 —— 只要有一个真的写了盘，
    // 文件就会重新出现。比"内容没变"强，因为内容没变也可能是重写了一遍一模一样的。
    //
    // 为什么在乎：用户只是改了个名（没分过组），不该平白多出一个 wtfml.folders。
    {
        QDir().remove(groupFile);
        check(!QFile::exists(groupFile), "Q6 前提：文件删掉了");

        InstanceGroupStore store;
        QString error;
        check(store.rekey(QStringLiteral("Q6-同名"), QStringLiteral("Q6-同名"), &error),
              "Q6 两个键一样时返回成功");
        check(store.rekey(QStringLiteral("Q6-压根没这个键"), QStringLiteral("Q6-新"), &error),
              "Q6 老键本来就没归属时返回成功");
        check(store.rekey(QString(), QStringLiteral("Q6-新"), &error), "Q6 空键时返回成功");
        check(store.rekey(QStringLiteral("Q6-新"), QString(), &error), "Q6 新键为空时返回成功");
        check(!QFile::exists(groupFile), "Q6 这几个空操作一个字节都没写盘");
    }

    // ---------- Q7 删组：清掉指向它的成员条目，实例一个不动 ----------
    {
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"},"
            "{\"id\":\"g2\",\"name\":\"乙\"}],"
            "\"members\":{\"Q7-实例A\":\"g1\",\"Q7-实例B\":\"g2\",\"Q7-实例C\":\"g1\"}}"));

        // 造两个真目录，证明"删组不删实例"不是嘴上说说 —— 这是全段唯一碰 versions/ 的地方。
        const QString dirA = versionsRoot + "/Q7-实例A";
        writeFileAt(dirA + "/Q7-实例A.jar", "not a real jar");
        writeFileAt(dirA + "/q7-留个记号.txt", "别删我");

        InstanceGroupStore store;
        QList<InstanceGroupStore::Group> kept;
        kept.append({QStringLiteral("g2"), QStringLiteral("乙")});
        QString error;
        check(store.replaceGroups(kept, &error), "Q7 全量替换成只剩乙组成功", error);

        InstanceGroupStore other;
        const QList<InstanceGroupStore::Group> groups = other.groups();
        check(groups.size() == 1 && groups.first().id == QStringLiteral("g2"),
              "Q7 另一个对象读回来只剩一个组", QString::number(groups.size()));
        check(other.groupIdOf(QStringLiteral("Q7-实例A")).isEmpty(),
              "Q7 甲组里的实例变成未分组");
        check(other.groupIdOf(QStringLiteral("Q7-实例C")).isEmpty(), "Q7 另一个也是");
        check(other.groupIdOf(QStringLiteral("Q7-实例B")) == QStringLiteral("g2"),
              "Q7 别的组的归属一点没动");
        check(!readRaw().value(QStringLiteral("members")).toObject()
                   .contains(QStringLiteral("Q7-实例A")),
              "Q7 指向已删组的成员条目从文件里清掉了（不留一条谁都查不到的残留）");
        check(QFile::exists(dirA + "/Q7-实例A.jar") &&
                  QFile::exists(dirA + "/q7-留个记号.txt"),
              "Q7 实例目录里一个文件都没少 —— 删组从来不删实例");

        QDir(dirA).removeRecursively();
    }

    // ---------- Q8 assign 的失败路径 ----------
    {
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}],\"members\":{}}"));
        const QByteArray before = readBytes();

        InstanceGroupStore store;
        QString error;
        check(!store.assign(QStringLiteral("Q8-实例"), QStringLiteral("没有这个组"), &error),
              "Q8 归到一个不存在的组时拒绝");
        check(!error.isEmpty(), "Q8 而且给了原因", error);
        check(!store.assign(QString(), QStringLiteral("g1"), &error), "Q8 空实例键时拒绝");
        check(!error.isEmpty(), "Q8 也给了原因", error);
        check(readBytes() == before, "Q8 两次失败的写入一个字节都没落盘");

        // 取消分组（空 groupId）不需要那个组存在，本来就该成功
        error.clear();  // 免得上面那两条失败的原因被当成这两条的 detail 打出来
        check(store.assign(QStringLiteral("Q8-实例"), QStringLiteral("g1"), &error),
              "Q8 归到存在的组成功", error);
        check(store.assign(QStringLiteral("Q8-实例"), QString(), &error),
              "Q8 取消分组成功（不要求任何组存在）", error);
        check(InstanceGroupStore().groupIdOf(QStringLiteral("Q8-实例")).isEmpty(),
              "Q8 取消之后这个实例读回来还是未分组（空串）");

        // 但文件里那一条**留着**，写的是那个保留值。不能写成"成员表里那条没了"：
        // 删掉条目 = 用户从来没表过态，于是 MainWindow 下一次刷新就按版本类型把它归类
        // 回去 —— 表现是"点了确定没生效"。
        // 两者在界面上是同一格，在文件里必须分得开，理由见 ungroupedMarker() 那段。
        const QJsonObject markedMembers =
            readRaw().value(QStringLiteral("members")).toObject();
        check(markedMembers.size() == 1 &&
                  markedMembers.value(QStringLiteral("Q8-实例")).toString() ==
                      InstanceGroupStore::ungroupedMarker(),
              "Q8 取消分组在文件里写成那个保留值（不是把条目删掉）",
              markedMembers.value(QStringLiteral("Q8-实例")).toString());
    }

    // ---------- Q9 读不懂的文件：读到空 + 拒绝写 ----------
    // 拒绝写和 wtfml.meta 是同一条规矩：往一份读不懂的内容上合并 = 把用户的东西丢掉。
    // 今天能造出这种文件的只有"用户手改"和"文件坏了"，两种都不该被我们悄悄抹掉。
    {
        const QByteArray broken = QByteArrayLiteral("{ \"version\": 1, \"groups\": [ ");
        writeRaw(broken);
        InstanceGroupStore store;
        check(store.groups().isEmpty(), "Q9 坏 JSON 时当没有组");
        check(store.groupIdOf(QStringLiteral("Q8-实例")).isEmpty(), "Q9 坏 JSON 时归属也算没有");

        QList<InstanceGroupStore::Group> groups;
        groups.append({packId, QStringLiteral("想覆盖它")});
        QString error;
        check(!store.replaceGroups(groups, &error), "Q9 读不懂的文件拒绝写");
        check(!error.isEmpty(), "Q9 而且说清了原因", error);
        check(!store.assign(QStringLiteral("Q8-实例"), QString(), &error),
              "Q9 另一条写路径（assign）同样拒绝");
        check(readBytes() == broken, "Q9 拒绝写之后文件逐字节没动");
    }
    {
        const QByteArray future = QByteArrayLiteral(
            "{\"version\":2,\"groups\":[{\"id\":\"g1\",\"name\":\"将来的组\"}]}");
        writeRaw(future);
        InstanceGroupStore store;
        check(store.groups().isEmpty(), "Q9 格式版本不认识时当空（不猜新旧格式的差别）");

        QList<InstanceGroupStore::Group> groups;
        groups.append({packId, QStringLiteral("我")});
        QString error;
        check(!store.replaceGroups(groups, &error), "Q9 版本不认识时拒绝写（别把它降级成 v1）");
        check(readBytes() == future, "Q9 文件逐字节没动");
    }

    // ---------- Q10 不可信数据清洗 ----------
    {
        // 成员指向一个**已经不存在的组**：这是"组被删了但两边不同步"的正常残留，
        // 当未分组处理、不报错；清理交给下一次写。
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}],"
            "\"members\":{\"Q10-实例A\":\"g1\",\"Q10-实例B\":\"早就没了的组\"}}"));
        InstanceGroupStore store;
        check(store.groupIdOf(QStringLiteral("Q10-实例A")) == QStringLiteral("g1"),
              "Q10 正常的那条读得到");
        check(store.groupIdOf(QStringLiteral("Q10-实例B")).isEmpty(),
              "Q10 指向不存在的组时当未分组（它不在 groups() 里）");

        QString error;
        QList<InstanceGroupStore::Group> groups = store.groups();
        check(store.replaceGroups(groups, &error), "Q10 随便写一次成功", error);
        check(!readRaw().value(QStringLiteral("members")).toObject()
                   .contains(QStringLiteral("Q10-实例B")),
              "Q10 下一次写的时候顺手把那条悬空的清掉了");
        check(InstanceGroupStore().groupIdOf(QStringLiteral("Q10-实例A")) ==
                  QStringLiteral("g1"),
              "Q10 而正常那条留着");
    }
    {
        // 组条目本身的问题：没 id 的认不出来、id 重复的说不清指谁、名字为空至少退回 id。
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"name\":\"没有 id 的组\"},"
            "{\"id\":\"g1\",\"name\":\"甲\"},{\"id\":\"g1\",\"name\":\"乙\"},"
            "{\"id\":\"g2\"},{\"id\":\"\",\"name\":\"空 id\"}]}"));
        const QList<InstanceGroupStore::Group> groups = InstanceGroupStore().groups();
        check(groups.size() == 2, "Q10 没 id / 空 id 的丢掉了，重复 id 的只留一个",
              QString::number(groups.size()));
        check(!groups.isEmpty() && groups.at(0).id == QStringLiteral("g1") &&
                  groups.at(0).name == QStringLiteral("甲"),
              "Q10 重复 id 留的是第一个");
        check(groups.size() > 1 && groups.at(1).id == QStringLiteral("g2") &&
                  groups.at(1).name == QStringLiteral("g2"),
              "Q10 名字为空时退回 id（界面上至少还认得出来是哪一个）");
    }
    {
        // 成员表里塞了非字符串的值：认不出来就当没分过组，别让 QJsonValue::toString()
        // 的"空串"悄悄变成一个合法键。
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}],"
            "\"members\":{\"Q10-实例A\":{\"这是\":\"对象\"},\"Q10-实例B\":\"g1\",\"\":\"g1\"}}"));
        InstanceGroupStore store;
        check(store.groupIdOf(QStringLiteral("Q10-实例A")).isEmpty(),
              "Q10 成员值不是字符串时当没有");
        check(store.groupIdOf(QString()).isEmpty(), "Q10 空键当成没有");
        check(store.groupIdOf(QStringLiteral("Q10-实例B")) == QStringLiteral("g1"),
              "Q10 同一个文件里正常那条照样读得到");
    }
    {
        // 文件在但内容是空的（用户清空过、或者写了一半）：不该崩，当空。
        writeRaw(QByteArray());
        InstanceGroupStore store;
        check(store.groups().isEmpty(), "Q10 空文件当没有组");
    }

    // ---------- Q11 换游戏文件夹 = 自动换一套分组 ----------
    // 没有任何"重置"钩子，靠的是访问器内部比较 AppPaths::minecraftDir()。所以要验的
    // 恰恰是"**同一个对象**在目录变了之后自己重读了" —— 换个新对象来验就绕过了这条。
    {
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"第一份的组\"}],"
            "\"members\":{\"Q11-实例\":\"g1\"}}"));
        const QString second = QCoreApplication::applicationDirPath() + "/.minecraft-Q11";
        QDir(second).removeRecursively();
        QDir().mkpath(second);

        const QString first = AppPaths::minecraftDir();
        const QByteArray firstBytes = readBytes();
        InstanceGroupStore store;
        check(store.groups().size() == 1 &&
                  store.groups().first().name == QStringLiteral("第一份的组"),
              "Q11 前提：读到的是第一份的组");

        AppPaths::setMinecraftDir(second);
        check(AppPaths::instanceGroupFilePath() == second + "/wtfml.folders",
              "Q11 文件路径跟着目录走");
        check(store.groups().isEmpty(), "Q11 换到第二份（还没有文件）时读到空");
        QString error;
        QList<InstanceGroupStore::Group> groups;
        groups.append({QStringLiteral("g-second"), QStringLiteral("第二份的组")});
        check(store.replaceGroups(groups, &error), "Q11 在第二份里建组成功", error);
        check(QFile::exists(second + "/wtfml.folders"),
              "Q11 文件落在第二份目录里，没有写回第一份");

        AppPaths::setMinecraftDir(first);
        check(readBytes() == firstBytes, "Q11 第一份那个文件逐字节没被动过");
        check(store.groups().size() == 1 &&
                  store.groups().first().name == QStringLiteral("第一份的组"),
              "Q11 切回来读到的是第一份的组（同一个对象，没人通知它）");
        check(store.groupIdOf(QStringLiteral("Q11-实例")) == QStringLiteral("g1"),
              "Q11 第一份的归属也回来了");

        QDir(second).removeRecursively();
        check(!QDir(second).exists(), "Q11 第二份目录删干净了（没攥着文件句柄）");
    }

    // ---------- Q9b 文件读不懂时，assign 报的是**真因**，不是「找不到那个分组」 ----------
    {
        const QByteArray broken = QByteArrayLiteral("{ 这不是 JSON");
        writeRaw(broken);
        InstanceGroupStore store;

        QString assignError;
        check(!store.assign(QStringLiteral("Q9b-实例"), QStringLiteral("随便哪个组"), &assignError),
              "Q9b 文件读不懂时 assign 拒绝写");
        QString replaceError;
        check(!store.replaceGroups(store.groups(), &replaceError),
              "Q9b 同一条路上 replaceGroups 也拒绝");
        // 两条路的理由必须是**同一句话**：它们说的是同一件事（写侧那道闸就在
        // writeThrough() 开头），措辞不一样的话用户看到的解释会随他点了哪个按钮变。
        check(assignError == replaceError,
              "Q9b assign 和 replaceGroups 报的是同一句话（都是那份文件读不懂的真因）",
              QStringLiteral("assign=[%1] replace=[%2]").arg(assignError, replaceError));
        check(!assignError.isEmpty() &&
                  !assignError.contains(QStringLiteral("找不到指定分组")),
              "Q9b 报的不是「找不到指定分组」（那是被挡住的那个假因）", assignError);
        check(!assignError.contains(QStringLiteral("没有指定要给哪个实例分组")),
              "Q9b 也不是判错成「空键」那条内部错误");
        check(readBytes() == broken, "Q9b 拒绝之后文件逐字节没动");
    }

    // ---------- Q13 写盘失败不许在内存里留下幽灵状态 ----------
    //
    // 这一条验的是 writeThrough() 里那份 candidate 副本（ 改的）。原来那三句
    // 直接盖在 m_root 上，而后面三条失败路径都不回滚 —— 于是"告知失败"的那个操作会先在
    // 内存里生效（groups() / groupIdOf() 读的正是 m_root），接着**另一次成功的写盘**会把
    // 这份幽灵状态一起提交：「说失败的操作在稍后静默生效」。
    //
    // 怎么造"读没事、只有写失败"：先让那个对象**正常加载**一份文件，再把文件路径占成
    // 一个**目录** —— QSaveFile 提交不了。顺序很讲究：反过来（一开始就是目录）的话加载那
    // 一步就会失败，对象会记住"这个文件打不开"并从此拒绝一切写（见 ensureLoaded 里那段），
    // 那样验到的就不是幽灵、而是另一条闸了。
    {
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}]}"));
        InstanceGroupStore store;
        check(store.groups().size() == 1, "Q13 前提：这个对象正常加载了那份文件");

        // 目录一占上，写的每一条路都得断，而加载那一步不会再跑（对象已经认下了目录）。
        QDir().remove(groupFile);
        check(QDir().mkpath(groupFile), "Q13 前提：把 wtfml.folders 占成一个目录");

        QList<InstanceGroupStore::Group> groups;
        groups.append({QStringLiteral("g1"), QStringLiteral("甲")});
        groups.append({QStringLiteral("g-幽灵"), QStringLiteral("幽灵组")});
        QString error;
        check(!store.replaceGroups(groups, &error), "Q13 写盘失败时如实返回 false", error);
        check(!error.isEmpty(), "Q13 失败原因说的是那句「打不开 / 写不进去」", error);
        // 这一条就是幽灵本身：写盘失败了，那它就不该在内存里出现。
        check(store.groups().size() == 1 &&
                  store.groups().first().id == QStringLiteral("g1"),
              "Q13 写失败之后内存里还是原来那一个组（失败不许先在内存里生效）",
              QStringLiteral("%1 个组").arg(store.groups().size()));
        check(store.groupIdOf(QStringLiteral("Q13-实例")).isEmpty(),
              "Q13 内存里也没多出别的归属");

        // 失败之后**另一次成功的写盘**：落盘的内容里不能带着上一次那笔。
        check(QDir().rmdir(groupFile), "Q13 把那个占位的目录拿掉");
        check(QDir().mkpath(AppPaths::minecraftDir()), "Q13 数据目录还在");
        QString error2;
        check(store.replaceGroups(store.groups(), &error2),
              "Q13 路径恢复之后这个对象写成功（它没被那次失败卡死）", error2);
        check(store.assign(QStringLiteral("Q13-实例"), QStringLiteral("g1"), &error2),
              "Q13 正常的赋值也成功", error2);

        const QByteArray bytes = readBytes();
        const QString text = QString::fromUtf8(bytes);
        check(!text.contains(QStringLiteral("幽灵")),
              "★ Q13 落盘那份里没有那次失败留下的「幽灵组」");
        const QJsonObject raw = QJsonDocument::fromJson(bytes).object();
        check(raw.value(QStringLiteral("groups")).toArray().size() == 1,
              "Q13 落盘那份里正好一个组（不是两个）",
              QString::number(raw.value(QStringLiteral("groups")).toArray().size()));
        // 换一个新对象读回来（同一个对象自证等于没验，见这一段开头那两条 NOTE）。
        InstanceGroupStore reread;
        check(reread.groups().size() == 1 &&
                  reread.groupIdOf(QStringLiteral("Q13-实例")) == QStringLiteral("g1"),
              "Q13 新对象读回来的就是刚写的那一份");
    }

    // ---------- Q14 五个默认组：什么时候种、什么时候一个字节都不写 ----------
    // 要求：「把实例分组做个默认（可安装mod，原版，快照版，远古版，置顶），
    // 默认可以删除」。**"默认可以删除"这件要求的全部实现就在 Q14b/Q14c 那两条上**：
    // 只有"文件不存在"才种，删过之后就再也不会长回来。
    {
        QDir().remove(groupFile);
        check(!QFile::exists(groupFile), "Q14 前提：先把文件拿掉");

        InstanceGroupStore store;
        int changed = 0;
        QObject::connect(&store, &InstanceGroupStore::groupsChanged, [&changed] { ++changed; });
        QString error;
        check(store.seedDefaultGroups(&error), "Q14 文件不在时种一次", error);
        check(changed == 1, "Q14 种成功发了一次 groupsChanged", QString::number(changed));
        check(QFile::exists(groupFile), "Q14 种完文件真的落盘了");

        // 五个 id 和名字都写死在这里（**不是**拿 defaultGroups() 跟自己比）：id 写歪了
        // 后果是"昨天分好的今天全回未分组"（自动归类按 id 找组，见 defaultGroupId()），
        // 那是这份文件里最不该漂的东西，值得在这里逐条钉住。
        const QList<InstanceGroupStore::Group> seeded = InstanceGroupStore().groups();
        QStringList seededPairs;
        for (const InstanceGroupStore::Group &group : seeded) {
            seededPairs << group.id + QLatin1Char('=') + group.name;
        }
        check(seededPairs == QStringList({QStringLiteral("default-pinned=收藏"),
                                          QStringLiteral("default-moddable=可安装mod"),
                                          QStringLiteral("default-vanilla=原版"),
                                          QStringLiteral("default-snapshot=快照版"),
                                          QStringLiteral("default-old=远古版")}),
              "Q14 种下去的就是那五个（id 写死、顺序就是界面上的顺序，收藏在最前）",
              seededPairs.join(QLatin1Char('|')));
        check(InstanceGroupStore::defaultGroups().size() == 5,
              "Q14 defaultGroups() 是五条",
              QString::number(InstanceGroupStore::defaultGroups().size()));

        // 读路径上的"表过态"：种子没给任何实例写归属。
        check(!InstanceGroupStore().isAssigned(QStringLiteral("Q14-没提过")),
              "Q14 刚种完，谁都没表过态（自动归类才有事可做）");
    }

    // ---------- Q14b 文件在（哪怕组全被删光了）就一个字节都不写 ----------
    {
        const QByteArray before = readBytes();
        InstanceGroupStore store;
        int changed = 0;
        QObject::connect(&store, &InstanceGroupStore::groupsChanged, [&changed] { ++changed; });
        check(store.seedDefaultGroups(), "Q14b 文件本来就在时直接成功");
        check(changed == 0, "Q14b 而且一声不响（没写盘就不该发 groupsChanged）",
              QString::number(changed));
        check(readBytes() == before, "Q14b 文件逐字节没被动过");
    }
    {
        // 用户把五个默认组**全删光**之后的样子：文件在、组清单是空的。
        // 这一条是"默认可以删除"最容易被写漏的那一格 —— 判据从"有没有那五个组"
        // 退化成"文件在不在"的话，用户删完一重启它们就全回来了。
        writeRaw(QByteArrayLiteral("{\"version\":1,\"groups\":[],\"members\":{}}"));
        const QByteArray before = readBytes();
        InstanceGroupStore store;
        check(store.seedDefaultGroups(), "Q14c 组清单是空的时候也直接成功");
        check(store.groups().isEmpty(), "Q14c 删光的组没被种回来");
        check(readBytes() == before, "Q14c 一个字节都没写盘");
    }

    // ---------- Q14d 那个保留值在各条写路径上留得住 ----------
    // 写路径的清洗规则是"指向不存在的组的归属清掉"，而保留值**不在组清单里** ——
    // 漏掉那一条例外的话，用户一按「确定」它就被清掉（="我明明选过未分组"）。
    {
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}],"
            "\"members\":{\"Q14-明确未分组\":\"ungrouped\",\"Q14-在甲组\":\"g1\","
            "\"Q14-悬空\":\"早就没了的组\"}}"));

        InstanceGroupStore store;
        check(store.isAssigned(QStringLiteral("Q14-明确未分组")),
              "Q14d 写了保留值 = 表过态（不然会被自动归类拉走）");
        check(store.isAssigned(QStringLiteral("Q14-悬空")),
              "Q14d 指向已删组的残留也算表过态（理由见 isAssigned() 那段：删组后落未分组的"
              "那一半靠它，用不着写清理代码）");
        check(!store.isAssigned(QStringLiteral("Q14-没提过")), "Q14d 文件里没有这一条才是没表过态");
        check(!store.isAssigned(QString()), "Q14d 例外：空键一律当没表过态");
        check(store.groupIdOf(QStringLiteral("Q14-明确未分组")).isEmpty() &&
                  store.groupIdOf(QStringLiteral("Q14-悬空")).isEmpty(),
              "Q14d 两者读回来都是未分组（界面上看不出区别，文件里分得开）");

        // 别的写路径（全量替换）不能把它顺手清掉。
        QList<InstanceGroupStore::Group> groups;
        groups.append({QStringLiteral("g1"), QStringLiteral("甲")});
        QString error;
        check(store.replaceGroups(groups, &error), "Q14d 全量替换写回去成功", error);
        const QJsonObject members = readRaw().value(QStringLiteral("members")).toObject();
        check(members.value(QStringLiteral("Q14-明确未分组")).toString() ==
                  InstanceGroupStore::ungroupedMarker(),
              "Q14d ★ 保留值在写路径上留得住",
              members.value(QStringLiteral("Q14-明确未分组")).toString());
        check(members.value(QStringLiteral("Q14-在甲组")).toString() == QStringLiteral("g1"),
              "Q14d 正常那条也留着");
        check(!members.contains(QStringLiteral("Q14-悬空")),
              "Q14d 而悬空那条被清掉了（清理规则本身没被这条例外带歪）");
        check(InstanceGroupStore().isAssigned(QStringLiteral("Q14-明确未分组")),
              "Q14d 换个对象读回来还是表过态的");

        // 再走一遍 assign()：取消分组两次，结果一样是那个保留值。
        check(store.assign(QStringLiteral("Q14-明确未分组"), QString(), &error),
              "Q14d 再取消一次也成功", error);
        check(readRaw().value(QStringLiteral("members")).toObject()
                  .value(QStringLiteral("Q14-明确未分组")).toString() ==
              InstanceGroupStore::ungroupedMarker(),
              "Q14d assign(空) 写的还是保留值");
    }

    // ---------- Q15 「置顶前的归属」：置顶记下原来在哪一组，取消置顶走回去 ----------
    //
    // 要求：「加一个记录置顶前这玩意属于哪一组的，用户直接塞的取消置顶
    // 才归进未分类」。四种来路四种落点（完整规矩见 InstanceGroupStore 文件头那一段），
    // 外加"记录什么时候作废"（写侧那道自清理不变量）和几条空操作。
    //
    // NOTE: 这一段凡是"盘上究竟写了什么"的断言一律读**文件**（readRaw()），不问任何一个
    // store 对象 —— 拿内存自证等于没验（见本节开头那两段）。
    {
        const auto membersInFile = [&readRaw]() {
            return readRaw().value(QStringLiteral("members")).toObject();
        };
        const auto originsInFile = [&readRaw]() {
            return readRaw().value(QStringLiteral("pinOrigin")).toObject();
        };
        // 每一条子段的起点：两个自定义组（g1「甲」/ g2「乙」= 置顶组）+ 想要的那几条归属。
        // origins 传空 = **连 pinOrigin 这一格都不写**（老文件/手改的文件就是那个样子）。
        const auto seed = [&writeRaw](const QByteArray &members, const QByteArray &origins) {
            QByteArray json = QByteArrayLiteral(
                "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"},"
                "{\"id\":\"g2\",\"name\":\"乙\",\"pinned\":true}],\"members\":");
            json += members;
            if (!origins.isEmpty()) {
                json += QByteArrayLiteral(",\"pinOrigin\":") + origins;
            }
            json += '}';
            writeRaw(json);
        };

        QString error;

        // (1) 从一个**普通的组**置顶 ⇒ 取消置顶回到那个组。
        {
            seed(QByteArrayLiteral("{\"Q15-甲组\":\"g1\"}"), QByteArray());
            InstanceGroupStore store;
            check(store.pinInstance(QStringLiteral("Q15-甲组"), &error), "Q15 置顶成功", error);
            check(membersInFile().value(QStringLiteral("Q15-甲组")).toString() ==
                      QStringLiteral("g2"),
                  "Q15 置顶 = 这一条落盘进了置顶组那一格");
            check(originsInFile().value(QStringLiteral("Q15-甲组")).toString() ==
                          QStringLiteral("g1") &&
                      !originsInFile().value(QStringLiteral("Q15-甲组")).isNull(),
                  "Q15 ★ 顺手记下了「置顶之前它在哪一组」",
                  originsInFile().value(QStringLiteral("Q15-甲组")).toString());
            check(store.unpinInstance(QStringLiteral("Q15-甲组"), &error), "Q15 取消置顶成功",
                  error);
            check(membersInFile().value(QStringLiteral("Q15-甲组")).toString() ==
                      QStringLiteral("g1"),
                  "Q15 ★ 取消置顶**回到置顶之前的那一组**",
                  membersInFile().value(QStringLiteral("Q15-甲组")).toString());
            check(originsInFile().isEmpty(),
                  "Q15 记录用完就删（留着的话下一次置顶会把旧账当成「置顶前」再读一遍）",
                  QString::number(originsInFile().size()));
            check(readRaw().contains(QStringLiteral("pinOrigin")),
                  "Q15 这一格还在（写侧一律写出来，空的就是 {} —— 和 expanded / pinned 同规矩）");
            check(InstanceGroupStore().groupIdOf(QStringLiteral("Q15-甲组")) ==
                      QStringLiteral("g1"),
                  "Q15 换个对象读回来也是那个组");
        }

        // (2) 从**没表过态**置顶（显示层按版本类型把它归在某组里）⇒ 取消置顶把成员条目
        // **删掉**，让它重新按类型归类 —— 落回它置顶前**显示**的那一组。
        {
            seed(QByteArrayLiteral("{}"), QByteArray());
            InstanceGroupStore store;
            check(store.pinInstance(QStringLiteral("Q15-没提过"), &error), "Q15 置顶成功", error);
            check(originsInFile().contains(QStringLiteral("Q15-没提过")) &&
                      originsInFile().value(QStringLiteral("Q15-没提过")).isNull(),
                  "Q15 ★ 置顶前压根没有归属那一种记成 **null**（不是空串、也不是保留值）",
                  originsInFile().value(QStringLiteral("Q15-没提过")).toVariant().toString());
            check(store.unpinInstance(QStringLiteral("Q15-没提过"), &error), "Q15 取消置顶成功",
                  error);
            check(!membersInFile().contains(QStringLiteral("Q15-没提过")),
                  "Q15 ★ 取消置顶把这一条**删掉**了（不是写成「明确选了未分组」）—— 于是显示层"
                  "按版本类型再算一遍，落回它置顶前显示的那一组；写成保留值等于替用户说了一句"
                  "他没说过的话，那个实例从此再也不会被自动归类");
            check(!InstanceGroupStore().isAssigned(QStringLiteral("Q15-没提过")),
                  "Q15 读回来就是「没表过态」");
            check(originsInFile().isEmpty(), "Q15 记录也跟着没了",
                  QString::number(originsInFile().size()));
        }

        // (3) 从「未分组」（= 用户**明确**选过的那一档）置顶 ⇒ 取消置顶回到那条保留值。
        // 它和 (2) 在界面上是同一格，在文件里必须差一格 —— 这正是"别把 (2) 写成保留值"
        // 那条的反面：这里写回去的**就是**保留值，因为它本来就是保留值。
        {
            seed(QByteArrayLiteral("{\"Q15-明确未分组\":\"ungrouped\"}"), QByteArray());
            InstanceGroupStore store;
            check(store.isAssigned(QStringLiteral("Q15-明确未分组")) &&
                      store.groupIdOf(QStringLiteral("Q15-明确未分组")).isEmpty(),
                  "Q15 前提：它明确选了未分组（表过态，但读回来是空串）");
            check(store.pinInstance(QStringLiteral("Q15-明确未分组"), &error), "Q15 置顶成功",
                  error);
            check(originsInFile().value(QStringLiteral("Q15-明确未分组")).toString() ==
                      InstanceGroupStore::ungroupedMarker(),
                  "Q15 ★ 置顶前是「明确未分组」的，记的就是那个保留值本身",
                  originsInFile().value(QStringLiteral("Q15-明确未分组")).toString());
            check(store.unpinInstance(QStringLiteral("Q15-明确未分组"), &error),
                  "Q15 取消置顶成功", error);
            check(membersInFile().value(QStringLiteral("Q15-明确未分组")).toString() ==
                      InstanceGroupStore::ungroupedMarker(),
                  "Q15 ★ 取消置顶回到「明确选了未分组」那条保留值（这一条留得住，不是被删掉）",
                  membersInFile().value(QStringLiteral("Q15-明确未分组")).toString());
        }

        // (4) **用户自己塞进置顶组的** ⇒ 取消置顶落未分组（用户那句"用户直接塞的才归进未分类"）。
        // 两条来路各来一次：走 assign()（=「移入分组」那个动作），和"文件里本来就在
        // 置顶组里、连 pinOrigin 这一格都没有"（= 这一版之前的老数据）。
        {
            seed(QByteArrayLiteral("{}"), QByteArray());
            InstanceGroupStore store;
            check(store.assign(QStringLiteral("Q15-直接塞的"), QStringLiteral("g2"), &error),
                  "Q15 前提：「移入分组」把它直接塞进了置顶组", error);
            check(!originsInFile().contains(QStringLiteral("Q15-直接塞的")),
                  "Q15 ★ 塞进置顶组**不记**「置顶前的归属」—— 记的是「置顶前」那一格，而这个"
                  "动作压根不是置顶（用户那句把两条路分得很清楚）");
            check(store.unpinInstance(QStringLiteral("Q15-直接塞的"), &error), "Q15 取消置顶成功",
                  error);
            check(membersInFile().value(QStringLiteral("Q15-直接塞的")).toString() ==
                      InstanceGroupStore::ungroupedMarker(),
                  "Q15 ★ 没有记录 ⇒ 取消置顶落**未分组**（写那条保留值，和「移出分组」逐字"
                  "同一个落点）");
        }
        {
            seed(QByteArrayLiteral("{\"Q15-老数据\":\"g2\"}"), QByteArray());
            InstanceGroupStore store;
            check(!originsInFile().contains(QStringLiteral("Q15-老数据")),
                  "Q15 前提：老文件里连 pinOrigin 这一格都没有");
            check(store.unpinInstance(QStringLiteral("Q15-老数据"), &error), "Q15 取消置顶成功",
                  error);
            check(membersInFile().value(QStringLiteral("Q15-老数据")).toString() ==
                      InstanceGroupStore::ungroupedMarker(),
                  "Q15 ★ 老数据也一样落未分组（这一格没有 = 一条记录都没有，读侧不做任何猜测）");
        }

        // (5) 改名的同时把记录一起搬到新键上。
        {
            seed(QByteArrayLiteral("{\"Q15-老键\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-老键\":\"g1\"}"));
            InstanceGroupStore store;
            check(store.rekey(QStringLiteral("Q15-老键"), QStringLiteral("Q15-新键"), &error),
                  "Q15 改名搬运成功", error);
            check(membersInFile().value(QStringLiteral("Q15-新键")).toString() ==
                          QStringLiteral("g2") &&
                      originsInFile().value(QStringLiteral("Q15-新键")).toString() ==
                          QStringLiteral("g1"),
                  "Q15 ★ 归属和「置顶前的归属」一起搬到新键上（不搬的话，改完名再取消置顶"
                  "就落未分组了，而用户什么都没做错）");
            check(!membersInFile().contains(QStringLiteral("Q15-老键")) &&
                      !originsInFile().contains(QStringLiteral("Q15-老键")),
                  "Q15 老键上两格都清干净了");
        }

        // (6) 别的动作把记录清掉 —— 写侧那道自清理不变量（只留"此刻确实待在置顶组里"的）。
        // 每一条都是"先摆成'在置顶组里、记着 g1'，再走那个动作，然后记录必须没了"。
        {
            // (6a) 移入别组。
            seed(QByteArrayLiteral("{\"Q15-搬家\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-搬家\":\"g1\"}"));
            InstanceGroupStore store;
            check(store.assign(QStringLiteral("Q15-搬家"), QStringLiteral("g1"), &error),
                  "Q15 移入甲组成功", error);
            check(!originsInFile().contains(QStringLiteral("Q15-搬家")),
                  "Q15 ★ 挪进别组之后记录作废（它已经不在置顶组里了，那个动作只剩"
                  "「取消置顶」一个读者）");
        }
        {
            // (6b) 移出分组（= 明确选未分组）。
            seed(QByteArrayLiteral("{\"Q15-移出\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-移出\":\"g1\"}"));
            InstanceGroupStore store;
            check(store.assign(QStringLiteral("Q15-移出"), QString(), &error), "Q15 移出分组成功",
                  error);
            check(!originsInFile().contains(QStringLiteral("Q15-移出")) &&
                      membersInFile().value(QStringLiteral("Q15-移出")).toString() ==
                          InstanceGroupStore::ungroupedMarker(),
                  "Q15 ★ 「移出分组」也把记录清掉");
        }
        {
            // (6c) 换一个置顶组：原来那一组不再是置顶组了，它的成员自然不再是"被置顶的"。
            seed(QByteArrayLiteral("{\"Q15-换置顶\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-换置顶\":\"g1\"}"));
            InstanceGroupStore store;
            check(store.setGroupPinned(QStringLiteral("g1"), true, &error), "Q15 换成甲组当置顶组",
                  error);
            check(!originsInFile().contains(QStringLiteral("Q15-换置顶")) &&
                      membersInFile().value(QStringLiteral("Q15-换置顶")).toString() ==
                          QStringLiteral("g2"),
                  "Q15 ★ 换了置顶组 ⇒ 老记录的读者（下箭头）已经不在那一行了，记录跟着作废"
                  "（成员一个字节都没动）");
        }
        {
            // (6d) 删掉那个组（全量替换里没有 g2）。
            seed(QByteArrayLiteral("{\"Q15-删组\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-删组\":\"g1\"}"));
            InstanceGroupStore store;
            QList<InstanceGroupStore::Group> kept;
            kept.append({QStringLiteral("g1"), QStringLiteral("甲")});
            check(store.replaceGroups(kept, &error), "Q15 换成只剩甲组的清单", error);
            check(!originsInFile().contains(QStringLiteral("Q15-删组")) &&
                      !membersInFile().contains(QStringLiteral("Q15-删组")),
                  "Q15 ★ 置顶组被删掉：指向它的归属和记录一起清（实例本身一个字节都不动）");
        }
        {
            // (6e) 恢复默认：整张清单换成五个默认组（g1/g2 都是自定义的，一起没）。
            seed(QByteArrayLiteral("{\"Q15-恢复默认\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-恢复默认\":\"g1\"}"));
            InstanceGroupStore store;
            check(store.restoreDefaultGroups(&error), "Q15 恢复默认分组设置", error);
            check(!originsInFile().contains(QStringLiteral("Q15-恢复默认")) &&
                      !membersInFile().contains(QStringLiteral("Q15-恢复默认")),
                  "Q15 ★ 「恢复默认」也一样（记录不会跨过一整张新清单活下来）");
        }

        // (7) 几条空操作：**一个字节都不写盘**。
        {
            // 已经在置顶组里的那一条：不算失败，但也不该盖掉已有的那条记录 —— 那记的是
            // 上一次置顶之前它在哪，比此刻这一格有用得多。
            seed(QByteArrayLiteral("{\"Q15-已经在了\":\"g2\"}"),
                 QByteArrayLiteral("{\"Q15-已经在了\":\"g1\"}"));
            const QByteArray before = readBytes();
            InstanceGroupStore store;
            check(store.pinInstance(QStringLiteral("Q15-已经在了"), &error),
                  "Q15 已经在置顶组里时置顶也算成功（不是失败）", error);
            check(readBytes() == before, "Q15 ★ 一个字节都不写盘");
            check(originsInFile().value(QStringLiteral("Q15-已经在了")).toString() ==
                      QStringLiteral("g1"),
                  "Q15 ★ 也不许盖掉已有的那条记录",
                  originsInFile().value(QStringLiteral("Q15-已经在了")).toString());
        }
        {
            // 这一份 .minecraft 里没有置顶组（那颗上箭头本来就没画）。store 这一层不写盘、
            // 也不报错：这件事此刻没有去处，不是失败。
            writeRaw(QByteArrayLiteral(
                "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"}],\"members\":{}}"));
            const QByteArray before = readBytes();
            InstanceGroupStore store;
            check(store.pinInstance(QStringLiteral("Q15-没去处"), &error),
                  "Q15 没有置顶组时置顶也返回成功（这一下本来就没有去处）", error);
            check(readBytes() == before, "Q15 ★ 没有置顶组 ⇒ 一个字节都不写盘");
        }
    }

    // ---------- Q15b pinnedGroupIdOf()：写侧那道自清理和读侧那颗箭头用同一份判据 ----------
    // 抽成静态函数就是为了这两处看到的**必须是同一个答案** —— 各写一遍的话，某天有人只
    // 改了一处（比如又加一条退回的路），表现是"记录清掉的那一档和箭头送去的那一档不是
    // 同一个组"，于是取消置顶把实例送进一个它从没待过的组（中途每一步都"看起来对"）。
    {
        const auto group = [](const char *id, bool pinned) {
            InstanceGroupStore::Group g;
            g.id = QString::fromLatin1(id);
            g.name = g.id;
            g.pinned = pinned;
            return g;
        };
        check(InstanceGroupStore::pinnedGroupIdOf(QList<InstanceGroupStore::Group>()).isEmpty(),
              "Q15b 空清单 = 空串（那颗上箭头没有去处）");
        // 手改出来的"两个都是 true"：取第一个 —— 这和 groups() 那套归一化（留第一个）
        // 看到的是同一个。
        QList<InstanceGroupStore::Group> twoPinned;
        twoPinned.append(group("g1", false));
        twoPinned.append(group("g2", true));
        twoPinned.append(group("g3", true));
        check(InstanceGroupStore::pinnedGroupIdOf(twoPinned) == QStringLiteral("g2"),
              "Q15b 多过一个置顶组时取**第一个**（和读侧归一化同一条规矩）",
              InstanceGroupStore::pinnedGroupIdOf(twoPinned));
        // 老文件那条退回路：一个都没勾，但那个叫「置顶」的默认组还在清单里。
        QList<InstanceGroupStore::Group> legacy;
        legacy.append(group("g1", false));
        InstanceGroupStore::Group fallback = group("x", false);
        fallback.id = InstanceGroupStore::defaultGroupId(InstanceGroupStore::DefaultKind::Pinned);
        legacy.append(fallback);
        check(InstanceGroupStore::pinnedGroupIdOf(legacy) ==
                  InstanceGroupStore::defaultGroupId(InstanceGroupStore::DefaultKind::Pinned),
              "Q15b 一个都没勾 ⇒ 退回「置顶」那个默认组（老用户的迁移路）");
        check(InstanceGroupStore::pinnedGroupIdOf(QList<InstanceGroupStore::Group>() <<
                                                  group("g1", false))
                  .isEmpty(),
              "Q15b 连它都不在 ⇒ 空串（第三条出口）");
        // 读侧那一头就是它：一份文件读出来的组清单，两条路必须一样。
        writeRaw(QByteArrayLiteral(
            "{\"version\":1,\"groups\":[{\"id\":\"g1\",\"name\":\"甲\"},"
            "{\"id\":\"g2\",\"name\":\"乙\",\"pinned\":true}],\"members\":{}}"));
        InstanceGroupStore store;
        check(InstanceGroupStore::pinnedGroupIdOf(store.groups()) == store.pinnedGroupId(),
              "Q15b ★ pinnedGroupId() 就是 pinnedGroupIdOf(groups()) —— 两条路一个答案");
    }

    // ---------- Q16 超大分组表：当读不懂，不读进内存也不覆盖 ----------
    {
        const QByteArray huge(9 * 1024 * 1024, 'x');
        writeRaw(huge);
        InstanceGroupStore store;
        const QList<InstanceGroupStore::Group> groups = store.groups();
        check(groups.isEmpty(), "Q16 ★ 超过 8 MiB 的分组表按读不懂处理（不先 readAll 再说）");
        check(store.groupIdOf(QStringLiteral("随便什么实例")).isEmpty(),
              "Q16 ★ 读不懂时归属也是空的");
        QString writeError;
        check(!store.replaceGroups(QList<InstanceGroupStore::Group>{}, &writeError) &&
                  !writeError.isEmpty(),
              "Q16 ★ 读不懂时改动被拒绝，并且说得出原因", writeError);
        check(readBytes().size() == huge.size(), "Q16 ★ 盘上那一份原样还在、没被覆盖",
              QString::number(readBytes().size()));
        QDir().remove(groupFile);
    }

    // ---------- Q12 沙盒收拾干净 ----------
    QDir().remove(groupFile);
    check(!QFile::exists(groupFile), "Q12 Q 段造出来的 wtfml.folders 删掉了");
    const QStringList left = QDir(versionsRoot).entryList(
        QStringList() << QStringLiteral("Q*"), QDir::Dirs | QDir::NoDotAndDotDot);
    check(left.isEmpty(), "Q12 Q 段造出来的目录一个都没剩下", left.join(QLatin1Char('|')));

    return 0;
}

