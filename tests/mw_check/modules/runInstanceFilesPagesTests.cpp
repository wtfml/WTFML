#include "Support.hpp"
#include "LoadingState.hpp"
#include <QElapsedTimer>
#include <QProcess>
#include <QScrollBar>
#include <QThread>
#ifdef Q_OS_WIN
#include <qt_windows.h>  // T20.13：占住一份假截图（允许读写共享、不允许删除共享）
#endif
#include "controllers/InstanceFileController.hpp"
#include "controllers/InstancePagesController.hpp"
#include "GameLogWindow.hpp"
void runInstanceFilesPagesTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T20.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    // 数据根目录**两头都改**（理由见 AH0 / T19：MainWindow 构造里会拿盘上那个「当前
    // 游戏文件夹」再把 AppPaths 改一次）。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const QVariant sourceBefore = sandbox.value(QStringLiteral("Download/Source"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.setValue(QStringLiteral("Download/Source"),
                     LauncherSettings::downloadSourceKey(DownloadSource::Official));
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));

    // ---------- 夹具：九个实例 ----------
    // idV 什么都不装、东西最全；idO 只在 wtfml.meta 里写着 loaderName=OptiFine；
    // idN 是个"名字里夹着 iris 的 jar"的靶子（mods/ 里**有**两个 jar）；idB 才是光板。
    // 又加了三个，摆的是"装了东西、但写法不是 `iris-…`/`litematica-…`"的
    // 三种真写法（它们原来一条都不认 ⇒ 那两页在装了东西的实例上照样说「用不上」）：
    // idR = jar 前面挂中文名（中文整合包 / PCL 的常态）、idX = 只有 Oculus（Forge 上那个
    // Iris 移植）、idF = OptiFine 当**普通模组**装进 mods/（1.12.2 及以前就是这么装的）。
    const QString idV = QStringLiteral("1.98.1");
    const QString idO = QStringLiteral("1.98.2");
    const QString idN = QStringLiteral("1.98.3");
    const QString idR = QStringLiteral("1.98.4");
    const QString idX = QStringLiteral("1.98.5");
    const QString idF = QStringLiteral("1.98.6");
    const QString idB = QStringLiteral("1.98.7");
    makeInstalledVersion(versionsDir, idV);
    makeInstalledVersion(versionsDir, idO);
    makeInstalledVersion(versionsDir, idN);
    makeInstalledVersion(versionsDir, idR);
    makeInstalledVersion(versionsDir, idX);
    makeInstalledVersion(versionsDir, idF);
    makeInstalledVersion(versionsDir, idB);
    makeVersionMeta(versionsDir, idO,
                    QByteArrayLiteral("{\"loaderName\":\"OptiFine\",\"loaderVersion\":\"H1\"}"));

    const QString dirV = AppPaths::versionDir(idV);
    const QString dirO = AppPaths::versionDir(idO);
    const QString dirN = AppPaths::versionDir(idN);
    const QString suffix = InstanceFilesPage::disabledSuffix();

    // idV 里那些东西：每一样都摆一份，好逐页量"该看见什么"。
    const QString irisJar = dirV + QStringLiteral("/mods/iris-1.6.5.jar");
    const QString litJar = dirV + QStringLiteral("/mods/litematica-fabric-1.20.1.jar");
    const QString sodiumOn = dirV + QStringLiteral("/mods/sodium.jar");
    const QString sodiumOff = sodiumOn + suffix;
    const QString subDir = dirV + QStringLiteral("/mods/mcef-libraries");
    writeFileAt(irisJar, QByteArrayLiteral("iris"));
    writeFileAt(litJar, QByteArrayLiteral("litematica"));
    // sodium **只给禁用着的那一份**：这一条钉的是"被禁用的不算装过"。
    writeFileAt(sodiumOff, QByteArrayLiteral("sodium"));
    // mods/ 底下还放一个**目录**（真机上就有：mcef-libraries）：它既不是 mod，
    // 也不该因为名字不长成 .jar 就被当成"没装过任何东西"。
    QDir().mkpath(subDir);
    writeFileAt(dirV + QStringLiteral("/resourcepacks/材质.zip"), QByteArrayLiteral("pack"));
    writeFileAt(dirV + QStringLiteral("/datapacks/数据.zip"), QByteArrayLiteral("dp"));
    writeFileAt(dirV + QStringLiteral("/shaderpacks/光影.zip"), QByteArrayLiteral("shader"));
    writeFileAt(dirV + QStringLiteral("/schematics/房子.litematic"), QByteArrayLiteral("lite"));
    writeFileAt(dirV + QStringLiteral("/saves/我的存档/level.dat"), QByteArrayLiteral("level"));
    writeFileAt(dirV + QStringLiteral("/screenshots/2026-09-26_12.00.00.png"),
                QByteArrayLiteral("png"));
    writeFileAt(dirV + QStringLiteral("/logs/latest.log"), QByteArrayLiteral("log"));

    writeFileAt(AppPaths::versionDir(idR) +
                    QStringLiteral("/mods/[虹膜] iris-fabric-1.11.4+mc26.2.jar"),
                QByteArrayLiteral("iris"));
    writeFileAt(AppPaths::versionDir(idR) +
                    QStringLiteral("/mods/[投影] litematica-fabric-26.2-0.28.7.jar"),
                QByteArrayLiteral("litematica"));
    writeFileAt(AppPaths::versionDir(idX) + QStringLiteral("/mods/oculus-mc1.20.1-1.8.0.jar"),
                QByteArrayLiteral("oculus"));
    writeFileAt(AppPaths::versionDir(idF) + QStringLiteral("/mods/OptiFine_1.12.2_HD_U_G5.jar"),
                QByteArrayLiteral("optifine"));
    // 光板那个实例里放两个"名字里夹着 iris 这几个字母、但它们自己不是光影加载器"的真文件
    // —— 判据要是退化成"整个文件名里找子串"，这一条会当场红。
    writeFileAt(dirN + QStringLiteral("/mods/GeckoLibIrisCompat-Fabric-1.0.0.jar"),
                QByteArrayLiteral("compat"));
    writeFileAt(dirN + QStringLiteral("/mods/[钠] sodium-fabric-0.9.2+mc26.2.jar"),
                QByteArrayLiteral("sodium"));

    // idP / idQ 摆的是**用户盘上真正报错的那一类** —— PCL 那种"整份 json
    // 合进实例目录"的实例：目录名是用户起的（中文），json 里没有 inheritsFrom，
    // `wtfml.meta` 里**只有** instanceId（我们自己的安装器才会写 loaderName）。
    // 两份都**没有 mods/ 目录**：这样"加载器认出来了"这件事只可能来自 json —— mods/
    // 那条退路（instanceModsUsable 的第二半）在这两个实例上一步都走不出去。
    // idP = 用户 点名的那一个（Fabric + 一堆模组，模组页却灰着）；
    // idQ = 只是装了 OptiFine 的那一类（json 里是 optifine:OptiFine，也没有 mods/）。
    const QString idP = QStringLiteral("海波篮子");
    const QString idQ = QStringLiteral("1.20.1-OptiFine_I6");
    writeFileAt(AppPaths::versionDir(idP) + QLatin1Char('/') + idP + QStringLiteral(".json"),
                QByteArrayLiteral(
                    "{\"id\":\"海波篮子\",\"type\":\"release\",\"mainClass\":"
                    "\"net.fabricmc.loader.impl.launch.knot.KnotClient\","
                    "\"clientVersion\":\"1.21.11\","
                    "\"libraries\":[{\"name\":\"net.fabricmc:fabric-loader:0.19.5\"},"
                    "{\"name\":\"net.fabricmc:intermediary:1.21.11\"},"
                    "{\"name\":\"net.minecraft:client:1.21.11\"}]}"));
    writeFileAt(AppPaths::versionDir(idQ) + QLatin1Char('/') + idQ + QStringLiteral(".json"),
                QByteArrayLiteral(
                    "{\"id\":\"1.20.1-OptiFine_I6\",\"type\":\"release\",\"clientVersion\":"
                    "\"1.20.1\",\"libraries\":[{\"name\":"
                    "\"optifine:OptiFine:1.20.1_HD_U_I6\"}]}"));
    makeInstalledVersion(versionsDir, idP);
    makeInstalledVersion(versionsDir, idQ);

    check(QFile::exists(irisJar) && QFile::exists(sodiumOff) && QDir(subDir).exists(),
          "T20.0 夹具摆好了（含一份**只有 .disabled** 的模组和一个 mods/ 底下的目录）");

    header("T20.10 文件页：宿主说正在运行 / 被占用时，一个字节都不动");
    {
        QTemporaryDir guard;
        const QString guarded = guard.filePath(QStringLiteral("guarded.jar"));
        writeFileAt(guarded, QByteArrayLiteral("KEEP"));
        const QStringList allPaths{guarded};
        bool refused = false;
        InstanceFileController controller(
            nullptr, InstanceFileController::Host{
                [](const QString &) {},
                [&refused, &allPaths](const QString &path) -> QString {
                    if (!allPaths.contains(path)) return QString();
                    refused = true;
                    return QStringLiteral("这个实例正在运行，先关掉游戏再动文件。");
                }});
        QString problem;
        check(!controller.toggle(guarded, &problem) && refused
                  && problem.contains(QStringLiteral("正在运行")),
              "★★ T20.10 正在运行的实例拒绝启用 / 禁用文件，并说清原因", problem);
        check(QFileInfo::exists(guarded) && !QFileInfo::exists(guarded + QStringLiteral(".disabled")),
              "★★ T20.10 拒绝之后文件名一个字节都没改");
        refused = false;
        problem.clear();
        check(!controller.removeUnasked(guarded, &problem) && refused
                  && problem.contains(QStringLiteral("正在运行")),
              "★★ T20.10 正在运行的实例拒绝删除文件，并说清原因", problem);
        check(QFileInfo::exists(guarded), "★★★ T20.10 拒绝之后文件仍在原处");
    }

    header("T20.10 生命周期文件批量操作只改临时夹具");
    {
        const QString source = root.filePath(QStringLiteral("lifecycle-batch/source"));
        const QString target = root.filePath(QStringLiteral("lifecycle-batch/target"));
        const QString first = QDir(source).filePath(QStringLiteral("first.png"));
        const QString second = QDir(source).filePath(QStringLiteral("second.png"));
        const QString firstTarget = QDir(target).filePath(QStringLiteral("first.png"));
        const QString secondTarget = QDir(target).filePath(QStringLiteral("second.png"));
        const QString outsider = root.filePath(QStringLiteral("lifecycle-batch/outside.png"));
        writeFileAt(first, QByteArrayLiteral("FIRST"));
        writeFileAt(second, QByteArrayLiteral("SECOND"));
        writeFileAt(outsider, QByteArrayLiteral("OUTSIDE"));
        check(QDir().mkpath(target), "create isolated lifecycle batch target");
        const auto contents = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };

        bool refuseTarget = false;
        bool insertConflict = false;
        QSet<QString> busy;
        const QString canonicalTarget = InstanceFiles::canonicalPath(target);
        const auto under = [](const QString &directory, const QString &path) {
            return QFileInfo(path).absolutePath().compare(directory, Qt::CaseInsensitive) == 0;
        };
        InstanceFileController controller(nullptr, InstanceFileController::Host{
            {},
            [&](const QString &path) {
                return refuseTarget && under(canonicalTarget, path) ? QStringLiteral("目标正被占用") : QString();
            },
            [&](const QString &path) {
                return under(source, path) ? QStringLiteral("source") : QStringLiteral("target");
            },
            [&](const QString &id, bool active) {
                if (active) busy.insert(id);
                else busy.remove(id);
                if (active && insertConflict && id == QStringLiteral("target"))
                    writeFileAt(secondTarget, QByteArrayLiteral("CONCURRENT"));
            }});
        using Action = InstanceFileController::LifecycleAction;
        auto result = controller.applyLifecycleBatch(Action::Remove, source, {first, outsider});
        check(result.done == 0 && !result.problem.isEmpty() && contents(first) == "FIRST"
                  && contents(outsider) == "OUTSIDE",
              "delete rejects out-of-directory paths before touching any member of the batch");
        result = controller.applyLifecycleBatch(Action::Move, source, {first, first}, target);
        check(result.done == 0 && !result.problem.isEmpty() && !QFileInfo::exists(firstTarget),
              "move rejects duplicate source paths");
        result = controller.applyLifecycleBatch(Action::Copy, source, {first}, source);
        check(result.done == 0 && !result.problem.isEmpty() && contents(first) == "FIRST",
              "copy rejects the source directory as a destination");
        const QString folder = QDir(source).filePath(QStringLiteral("nested.png"));
        check(QDir().mkpath(folder), "create directory disguised as a screenshot");
        result = controller.applyLifecycleBatch(Action::Remove, source, {folder});
        check(result.done == 0 && !result.problem.isEmpty() && QDir(folder).exists(),
              "batch deletion never recurses into a directory");
        writeFileAt(secondTarget, QByteArrayLiteral("KEEP"));
        result = controller.applyLifecycleBatch(Action::Move, source, {first, second}, target);
        check(result.done == 0 && !result.problem.isEmpty() && contents(first) == "FIRST"
                  && !QFileInfo::exists(firstTarget) && contents(secondTarget) == "KEEP",
              "a known target collision stops the whole batch without overwriting");
        check(QFile::remove(secondTarget), "remove the isolated collision fixture");
        refuseTarget = true;
        result = controller.applyLifecycleBatch(Action::Copy, source, {first}, target);
        check(result.done == 0 && result.problem == QStringLiteral("目标正被占用") && busy.isEmpty(),
              "the destination instance guard runs before writes");
        refuseTarget = false;
        result = controller.applyLifecycleBatch(Action::Copy, source, {first}, target);
        check(result.done == 1 && result.problem.isEmpty() && contents(firstTarget) == "FIRST"
                  && contents(first) == "FIRST" && busy.isEmpty(),
              "copy preserves the original and releases busy state");
        check(QFile::remove(firstTarget), "remove the isolated copy before the move test");
        {
            // 结构页：来源在各存档 generated 的子目录里，复制落到目标目录的一层。
            const QString saves = root.filePath(QStringLiteral("lifecycle-batch/saves"));
            const QString structures = QStringLiteral("/generated/minecraft/structures/");
            const QString nestedA = saves + QStringLiteral("/W1") + structures + QStringLiteral("a.nbt");
            const QString nestedB = saves + QStringLiteral("/W2") + structures + QStringLiteral("b.nbt");
            const QString nestedSame = saves + QStringLiteral("/W2") + structures + QStringLiteral("a.nbt");
            const QString elsewhere = saves + QStringLiteral("/W1/other/a.nbt");
            for (const QString &path : {nestedA, nestedB, nestedSame, elsewhere}) {
                check(QDir().mkpath(QFileInfo(path).absolutePath()), "create nested structure fixture folder");
                writeFileAt(path, QByteArrayLiteral("NESTED"));
            }
            result = controller.applyLifecycleBatch(Action::Copy, saves, {nestedA, nestedB}, target);
            check(result.done == 0 && !result.problem.isEmpty(),
                  "files below the source root are rejected unless nested sources are enabled");
            result = controller.applyLifecycleBatch(Action::Copy, saves, {nestedA, nestedSame}, target, true);
            check(result.done == 0 && !result.problem.isEmpty() && !QFileInfo::exists(target + QStringLiteral("/a.nbt")),
                  "nested copy rejects same-named files from different worlds before writing");
            result = controller.applyLifecycleBatch(Action::Copy, saves, {elsewhere}, target, true);
            check(result.done == 0 && !result.problem.isEmpty(),
                  "nested sources must be inside a generated folder");
            result = controller.applyLifecycleBatch(Action::Copy, saves, {nestedA, nestedB}, target, true);
            check(result.done == 2 && result.problem.isEmpty() && contents(nestedA) == "NESTED"
                      && contents(target + QStringLiteral("/a.nbt")) == "NESTED"
                      && contents(target + QStringLiteral("/b.nbt")) == "NESTED",
                  "nested copy lands every file in the destination folder and keeps the originals");
            check(QFile::remove(target + QStringLiteral("/a.nbt")) && QFile::remove(target + QStringLiteral("/b.nbt")),
                  "remove the isolated nested copies");
        }
#ifdef Q_OS_WIN
        if (!qEnvironmentVariableIsSet("WTFML_SKIP_JUNCTION")) {
            const QString alias = root.filePath(QStringLiteral("lifecycle-batch/target-link"));
            const bool madeAlias = QProcess::execute(QStringLiteral("cmd"),
                {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                 QDir::toNativeSeparators(alias), QDir::toNativeSeparators(target)}) == 0;
            if (madeAlias) {
                result = controller.applyLifecycleBatch(Action::Move, source, {first}, alias);
                check(result.done == 0 && !result.problem.isEmpty() && contents(first) == "FIRST"
                          && !QFileInfo::exists(firstTarget),
                      "move refuses a junction destination before writing to its target");
                check(QDir().rmdir(alias), "remove only the isolated junction");
            } else {
                printf("SKIP: could not create isolated lifecycle batch junction\n");
            }
        }
        insertConflict = true;
        result = controller.applyLifecycleBatch(Action::Move, source, {first, second}, target);
        check(result.done == 1 && !result.problem.isEmpty() && !QFileInfo::exists(first)
                  && contents(firstTarget) == "FIRST" && contents(second) == "SECOND"
                  && contents(secondTarget) == "CONCURRENT" && busy.isEmpty(),
              "move stops on a late conflict, keeps unprocessed sources and reports completed files");
#endif
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T20User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T20.0 建一个离线账号并设成当前身份", offlineError);
    const QString lastVersionBefore = LauncherSettings().lastLaunchedVersion();
    rememberLastVersion(idV);

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        // ---------- ① instanceFileDirs：六页各看哪个目录 ----------
        {
            // Q_RETURN_ARG 是个宏，类型里的逗号会被当成参数分隔符 ⇒ 必须先起个别名。
            using DirMap = QHash<QString, QString>;
            DirMap dirs;
            const bool got = QMetaObject::invokeMethod(
                &w, "instanceFileDirs", Qt::DirectConnection,
                Q_RETURN_ARG(DirMap, dirs), Q_ARG(QString, idV));
            check(got && dirs.size() == 9,
                  "★ T20.1 instanceFileDirs() 给全了九条路径（少一条 = 对应的那一页/那颗按钮"
                  " 安静地看着一个空目录）",
                  QStringLiteral("got=%1, %2 个键").arg(got).arg(dirs.size()));

            // 四个能下载的类**同一个出处**：ResourceCatalog::folderForKind()。这里刻意
            // 重新调一遍那个函数当期望值 —— 两处写死的话，改目录名那天两边一起错。
            const QList<QPair<QString, ResourceCatalog::Kind>> fromCatalog{
                {QStringLiteral("mod"), ResourceCatalog::Kind::Mod},
                {QStringLiteral("resourcepack"), ResourceCatalog::Kind::ResourcePack},
                {QStringLiteral("datapack"), ResourceCatalog::Kind::DataPack},
                {QStringLiteral("shader"), ResourceCatalog::Kind::Shader},
            };
            bool catalogOk = true;
            QString mismatch;
            for (const auto &one : fromCatalog) {
                const QString want =
                    dirV + QLatin1Char('/') + ResourceCatalog::folderForKind(one.second);
                if (dirs.value(one.first) != want) {
                    catalogOk = false;
                    mismatch = QStringLiteral("%1: 拿到 %2，该是 %3")
                                   .arg(one.first, dirs.value(one.first), want);
                    break;
                }
            }
            check(catalogOk,
                  "★ T20.1 模组/资源包/数据包/光影这四个目录走 folderForKind() ——"
                  " **不是**在这儿又抄一份目录名",
                  catalogOk ? dirs.value(QStringLiteral("datapack")) : mismatch);
            check(dirs.value(QStringLiteral("schematic")) == dirV + QStringLiteral("/schematics") &&
                      dirs.value(QStringLiteral("save")) == dirV + QStringLiteral("/saves"),
                  "T20.1 原理图 / 存档这两个目录名（它们不在下载页那几类里，所以写在这一层）",
                  QStringLiteral("%1 | %2")
                      .arg(dirs.value(QStringLiteral("schematic")),
                           dirs.value(QStringLiteral("save"))));
            // 「配置文件」那一页（点开一个配置文件就能改，见
            // ConfigFileEditorPage）。**它必须是 `<实例>/config`** —— 那是模组自己建的
            // 那一个，不是别处任何一个同名目录；指错了的话用户改的是别人的配置。
            check(dirs.value(QStringLiteral("config")) == dirV + QStringLiteral("/config"),
                  "★★ T20.1 「配置文件」那一页看的是 `<实例>/config`（模组自己建的那一个）",
                  dirs.value(QStringLiteral("config")));
            bool underOk = !dirs.isEmpty();
            for (auto it = dirs.constBegin(); it != dirs.constEnd(); ++it) {
                underOk = underOk && it.value().startsWith(dirV + QLatin1Char('/'));
            }
            check(underOk,
                  "★ T20.1 九条路径**全在这个实例自己的目录底下**（--gameDir 就是它）——"
                  " 有一条跑到别处去，用户就会在别的实例里删东西",
                  dirV);
        }

        // ---------- ② instanceHasMod / instanceShaderUsable ----------
        {
            const auto hasMod = [&w](const QString &id, const QString &prefix) {
                bool result = false;
                QMetaObject::invokeMethod(&w, "instanceHasMod", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, result), Q_ARG(QString, id),
                                          Q_ARG(QString, prefix));
                return result;
            };
            check(hasMod(idV, QStringLiteral("iris")) &&
                      hasMod(idV, QStringLiteral("litematica")),
                  "T20.2 装了两个模组的实例：都认得出来（iris-… / litematica-…）");
            check(!hasMod(idV, QStringLiteral("sodium")),
                  "★ T20.2 只有 `sodium.jar.disabled` 时**不算装过**（那正是"
                  "「禁用」的意思）—— 把它算成装了，用户点「禁用」之后光影页还是说能用");
            check(hasMod(idV, QStringLiteral("IRIS")),
                  "★ T20.2 不区分大小写（Windows 上找个文件不区分，判据也不该区分）");
            check(!hasMod(idN, QStringLiteral("iris")) && !hasMod(idV, QStringLiteral("optifine")),
                  "T20.2 光板实例 / 找不着的模组都回假");
            check(hasMod(idR, QStringLiteral("iris")) &&
                      hasMod(idR, QStringLiteral("litematica")),
                  "★★ T20.2 jar 前面挂着中文名（`[虹膜] iris-….jar` / `[投影] litematica-….jar`）"
                  " 照样认得出来 —— 那是中文整合包和 PCL 的常态写法。按前缀认的话，这两个"
                  " 实例上那两页会谎报「用不上」（用户报的"
                  "「原理图和光影包没正常显示」）");
            check(!hasMod(idN, QStringLiteral("iris")),
                  "★★ T20.2 判据是**整段词**，不是「文件名里找子串」："
                  "`GeckoLibIrisCompat-….jar` 里夹着 iris，而它自己不是光影加载器 ——"
                  " 这一条钉的是「别为了认中文名就退化成 contains()」");

            const auto shaderUsable = [&w](const QString &id) {
                bool result = false;
                QMetaObject::invokeMethod(&w, "instanceShaderUsable", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, result), Q_ARG(QString, id));
                return result;
            };
            check(shaderUsable(idV),
                  "T20.3 装了 Iris（一个普通模组）的实例能用光影包");
            check(shaderUsable(idO),
                  "★ T20.3 装的是 OptiFine（它是**加载器**，自己就认 shaderpacks/）也算"
                  " —— 这一条判的是 wtfml.meta 里的 loaderName，不是 mods/ 里有什么",
                  dirO);
            check(!shaderUsable(idN),
                  "★ T20.3 没有光影加载器的实例该说「用不上」—— 而这个实例 mods/ 里塞着"
                  "`GeckoLibIrisCompat-….jar` 和 `[钠] sodium-….jar`：名字里夹着 iris 那"
                  "几个字母**不算**装过（判据是整段词，见 T20.2 那条）");
            check(shaderUsable(idR),
                  "★★ T20.3 挂着中文名装的 Iris 也算能用光影包（同 T20.2 那一条）");
            check(shaderUsable(idX),
                  "★★ T20.3 装的是 **Oculus** 也算 —— Forge 上那个 Iris 移植，用户盘上"
                  "「落幕曲」「香草纪元」「魔改版ATT2」三个包用的都是它，而它原来不认");
            check(shaderUsable(idF),
                  "★★ T20.3 OptiFine **当普通模组**装进 mods/ 的老写法也算（1.12.2 及以前"
                  "OptiFine 就是这么装的，`OptiFine_1.12.2_HD_U_G5.jar`）—— 那以前只有"
                  "「加载器名 == OptiFine」一条路，这个写法的实例全被判成用不上");

            // ---------- ②b 加载器从**版本 json 自己身上**认（ 修的那个误判）----------
            //
            // 要求：「模组存在误判的情况，像海波篮子连 litematica 都有了怎么会没有模组
            // 加载器呢」。根子是判据原来只看 `wtfml.meta` 里的 loaderName，而那一格**只有
            // 我们自己的安装器会写** —— PCL 那类实例（真机上 70 个里 67 个）读出来是空的。
            // 这一节钉的就是那第二条路：**合并过的 json** 里的 `libraries`（外加 OptiFine
            // 那种「认得出来、但导不了」的档）。
            {
                VersionManager manager;
                const auto loader = [&manager](const QString &id, QString *versionOut) {
                    QString name;
                    return manager.instanceModLoader(id, &name, versionOut) ? name : QString();
                };
                QString version;
                const QString nameP = loader(idP, &version);
                const QString versionP = version;
                const QString nameB = loader(idB, nullptr);
                const QString nameQ = loader(idQ, &version);
                const QString versionQ = version;
                const QString nameO = loader(idO, nullptr);
                check(nameP == QStringLiteral("Fabric") && versionP == QStringLiteral("0.19.5"),
                      "★★ T20.3b 目录名是用户起的中文名、`wtfml.meta` 里只有 instanceId —— "
                      "加载器照样从 json 的 `net.fabricmc:fabric-loader:<版本>` 上认出来，"
                      "版本号也一块儿带回来（海波篮子那一个实例，用户点名报的）",
                      QStringLiteral("%1 %2").arg(nameP, versionP));
                check(nameB.isEmpty(),
                      "★★ T20.3b 真的什么都没有的实例（没 meta、没 json、没 mods/）上"
                      "**认不出加载器** —— 反过来的那一半：上一行要是靠「叫不出来也回真」"
                      "蒙过去，这一行会红");
                check(nameQ == QStringLiteral("OptiFine") &&
                          versionQ == QStringLiteral("1.20.1_HD_U_I6"),
                      "★★ T20.3b 装 OptiFine 的实例（json 里是 `optifine:OptiFine:<版本>`，"
                      "**没有 mods/ 目录**）也算有加载器、版本号也认得出 —— 那一档在导出那条路"
                      "上是「认得出来但导不了」，而这里问的是「基于哪个加载器」，两件事",
                      QStringLiteral("%1 %2").arg(nameQ, versionQ));
                check(nameO == QStringLiteral("OptiFine"),
                      "T20.3b 第一条路（wtfml.meta 里的 loaderName）一个字没动：我们自己的安装器"
                      "装的实例照旧从 meta 上认");

                const auto modsUsable = [&w](const QString &id) {
                    bool result = false;
                    QMetaObject::invokeMethod(&w, "instanceModsUsable", Qt::DirectConnection,
                                              Q_RETURN_ARG(bool, result), Q_ARG(QString, id));
                    return result;
                };
                check(modsUsable(idP),
                      "★★★ T20.3b 「模组」那一页在**没有 mods/ 目录**的 PCL 实例上也是亮的 —— "
                      "只可能来自 json 那条路（第二半判据模组目录里有没有 jar，这里一个都没有）。"
                      "用户报的就是这一条：那个实例连 litematica 都有了，模组页却灰着");
                check(modsUsable(idN),
                      "★★ T20.3b 反过来那半：问不出加载器、可 mods/ 里躺着 jar ⇒ 也算用得上"
                      "（用户盘上有两个实例连唯一一份 json 都解析不出来，却装着 27 / 136 个模组）");
                check(!modsUsable(idB),
                      "★★ T20.3b 两条都不中的实例上「模组」是灰的（前面那两条要是一起退化成"
                      "恒真，这一行当场红）");
                check(shaderUsable(idQ),
                      "★★ T20.3b 同一条路也把「光影包」救回来了：OptiFine 实例没有 mods/ 目录，"
                      "原来两条判据同时落空，那一页在**真的装了 OptiFine** 的实例上说「用不上」"
                      "（用户盘上 `1.20.1-OptiFine_I6` / `1.20.2-OptiFine_I7_pre1` 两个就是这样）");

                // ---- 同一件事的另外两个出口：自动归类（defaultGroupIdFor）与行首那个标志
                // （defaultInstanceMark）都走同一个 instanceLoaderName()。归类这一个在
                // 实例树上看得见，所以在这儿钉一下：认出来了 ⇒ 进「可安装mod」，
                // 而不是被扔进「原版」。
                QTreeWidget *instanceTree =
                    byObjectName<QTreeWidget>(&w, MainWindowUI::instanceListName());
                const auto groupHeaderFor = [](QTreeWidget *tree, const QString &id) {
                    if (tree == nullptr) {
                        return QString();
                    }
                    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                        QTreeWidgetItem *header = tree->topLevelItem(i);
                        if (header == nullptr) {
                            continue;
                        }
                        for (int j = 0; j < header->childCount(); ++j) {
                            if (header->child(j)->data(0, Qt::UserRole).toString() == id) {
                                return header->text(0);
                            }
                        }
                    }
                    return QString();
                };
                const QString groupP = groupHeaderFor(instanceTree, idP);
                const QString groupB = groupHeaderFor(instanceTree, idB);
                check(groupP.startsWith(QStringLiteral("可安装mod")),
                      "★★ T20.3b 自动归类也跟着修好了：加载器是从 json 上认出来的 PCL 实例进"
                      "「可安装mod」，不再被扔进「原版」—— 判据是同一个 instanceLoaderName()，"
                      "用户盘上 67 个实例原本全落在「原版」里（五个默认组里那一个永远是空的）",
                      groupP);
                check(!groupB.startsWith(QStringLiteral("可安装mod")),
                      "★★ T20.3b 而真的没有加载器、mods/ 也是空的那个**不**进「可安装mod」"
                      "（归类那条判据要是被放宽成「看 mods/ 里有没有东西」，这一行会红）",
                      groupB.isEmpty() ? QStringLiteral("(没找到那一行)") : groupB);
            }
        }

        // ---------- ③ toggleInstanceFile：改名，撞车就不许动 ----------
        {
            const auto toggle = [&w](const QString &path) {
                bool result = false;
                QMetaObject::invokeMethod(&w, "toggleInstanceFile", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, result), Q_ARG(QString, path));
                return result;
            };
            check(toggle(sodiumOff) && QFile::exists(sodiumOn) && !QFile::exists(sodiumOff),
                  "★ T20.4 「启用」把 `.jar.disabled` 改回 `.jar`（盘上只有一份，没有复制）");
            check(toggle(sodiumOn) && QFile::exists(sodiumOff) && !QFile::exists(sodiumOn),
                  "★ T20.4 「禁用」反过来 —— 而且**文件名一个字没改**，只是尾巴上多了一截"
                  "（游戏扫不到它，东西还在）");
            {
                // 拿旧路径（.jar）再点一次：那个名字这会儿已经不在盘上了 ⇒ 走那条
                // "不在盘上"的岔路，它会**弹一句**（这一节里每一条会弹话的路都得有
                // 一个 PromptAnswerer 在跑 —— 没有的话那是个嵌套事件循环，测试就挂在那儿了）。
                PromptAnswerer answerer({0});
                answerer.start();
                const bool again = toggle(sodiumOn);
                const bool warned =
                    pumpUntil([&answerer] { return !answerer.answered.isEmpty(); }, 5000);
                answerer.stop();
                check(!again && warned,
                      "★ T20.4 界面上那一行报的是**它现在那个名字**：拿禁用前那个路径再点一次"
                      " 应该当场回假并说一声（真去猜一个近似名字就把别人的文件改了）",
                      QStringLiteral("again=%1 warned=%2").arg(again).arg(warned));
                check(QFile::exists(sodiumOff) && !QFile::exists(sodiumOn),
                      "T20.4 而且那份文件还是禁用着躺在原处（那一下什么都没动）");
            }

            // 撞车：`x.jar` 和 `x.jar.disabled` 同时存在。这一下**一份都不许动**。
            const QString bothA = dirV + QStringLiteral("/mods/both.jar");
            const QString bothB = bothA + suffix;
            writeFileAt(bothA, QByteArrayLiteral("AAA"));
            writeFileAt(bothB, QByteArrayLiteral("BBB"));
            {
                PromptAnswerer answerer({0});
                answerer.start();
                const bool moved = toggle(bothA);
                const bool warned =
                    pumpUntil([&answerer] { return !answerer.answered.isEmpty(); }, 5000);
                answerer.stop();
                check(!moved && warned && !answerer.answered.isEmpty() &&
                          answerer.answered.at(0).title == QStringLiteral("改动失败"),
                      "★ T20.4 目标名已经有一份时不改：弹一句「改动失败」",
                      answerer.answered.isEmpty() ? QStringLiteral("没弹")
                                                  : answerer.answered.at(0).title);
                check(readBytesAt(bothA) == QByteArrayLiteral("AAA") &&
                          readBytesAt(bothB) == QByteArrayLiteral("BBB"),
                      "★ T20.4 而且**两份都原样躺着** —— 照改就等于用户自己那一份没了"
                      "（这是这一节唯一不可接受的失败）");
            }
            QFile::remove(bothA);
            QFile::remove(bothB);

            const QString dirPath = dirV + QStringLiteral("/mods/mcef-libraries");
            const QString insideDir = dirPath + QStringLiteral("/在里面的.jar");
            writeFileAt(insideDir, QByteArrayLiteral("T20-DIR"));
            {
                PromptAnswerer answerer({0});
                answerer.start();
                const bool moved = toggle(dirPath);
                const bool warned =
                    pumpUntil([&answerer] { return !answerer.answered.isEmpty(); }, 5000);
                answerer.stop();
                check(!moved && QDir(dirPath).exists() && !QDir(dirPath + suffix).exists() &&
                          QFile::exists(insideDir),
                      "★★ T20.4 拿一个**目录**点「启用 / 禁用」：**一个字节都不动盘**"
                      "（用户原话「游戏模组文件夹里套的文件夹也会被列出来，而禁用对其的作用"
                      "只是改个名」—— 对一个文件夹改名，它里面的东西会一起从游戏眼里消失）",
                      QStringLiteral("没改名=%1 目标名没出现=%2 里面的文件还在=%3")
                          .arg(QDir(dirPath).exists() ? QStringLiteral("是")
                                                      : QStringLiteral("**不是**"),
                               !QDir(dirPath + suffix).exists() ? QStringLiteral("是")
                                                                : QStringLiteral("**不是**"),
                               QFile::exists(insideDir) ? QStringLiteral("是")
                                                        : QStringLiteral("**不是**")));
                check(warned && !answerer.answered.isEmpty() &&
                          answerer.answered.at(0).text.contains(QStringLiteral("文件夹")),
                      "★★ T20.4 而且当场说一句人话说明为什么（不是静默什么都不做）",
                      answerer.answered.isEmpty()
                          ? QStringLiteral("没弹")
                          : answerer.answered.at(0).text.left(80));
            }
            QFile::remove(insideDir);
        }

        // ---------- ④ deleteInstanceFile：先问一句 ----------
        {
            const QString victim = dirV + QStringLiteral("/datapacks/数据.zip");
            const QString saveDir = dirV + QStringLiteral("/saves/我的存档");
            const auto remove = [&w](const QString &path) {
                bool result = false;
                QMetaObject::invokeMethod(&w, "deleteInstanceFile", Qt::DirectConnection,
                                          Q_RETURN_ARG(bool, result), Q_ARG(QString, path));
                return result;
            };

            // 「取消」：一个字节都不许少。按钮次序是 question(…, 是=「删除」, 否=「取消」)
            // ⇒ 下标 1 是取消。
            PromptAnswerer answerer({1});
            answerer.start();
            const bool removed = remove(victim);
            const bool asked = pumpUntil([&answerer] { return !answerer.answered.isEmpty(); }, 5000);
            answerer.stop();
            check(!removed && asked && !answerer.answered.isEmpty() &&
                      answerer.answered.at(0).title == QStringLiteral("删除文件") &&
                      answerer.answered.at(0).buttons ==
                          QStringList({QStringLiteral("删除"), QStringLiteral("取消")}),
                  "★ T20.5 点「删除」**先问一句**，问的就是「删除 / 取消」这两颗",
                  answerer.answered.isEmpty()
                      ? QStringLiteral("没弹")
                      : answerer.answered.at(0).buttons.join(QStringLiteral("/")));
            check(readBytesAt(victim) == QByteArrayLiteral("dp"),
                  "★ T20.5 按「取消」时那个文件**原样躺着**（问一句的意义就在这儿）");

            // 「删除」：这一次真的没了。
            PromptAnswerer answerer2({0});
            answerer2.start();
            const bool deleted = remove(victim);
            pumpUntil([&answerer2] { return !answerer2.answered.isEmpty(); }, 5000);
            answerer2.stop();
            check(deleted && !QFile::exists(victim), "★ T20.5 按「删除」时它真的没了");

            // 文件夹**连同里面的东西一起**（saves 底下的 level.dat 不能留下来）。
            check(QFile::exists(saveDir + QStringLiteral("/level.dat")),
                  "T20.5 前提：那个存档文件夹里确实有一份 level.dat");
            PromptAnswerer answerer3({0});
            answerer3.start();
            const bool dirDeleted = remove(saveDir);
            pumpUntil([&answerer3] { return !answerer3.answered.isEmpty(); }, 5000);
            answerer3.stop();
            check(dirDeleted && !QDir(saveDir).exists(),
                  "★ T20.5 删文件夹是**连着里面的东西一起**（砍掉一半等于留一个空壳存档）");
        }

        // ---------- ⑤ loadServerList：三行代码，但"不在"和"读不动"是两句话 ----------
        {
            InstanceSettingsDialog dialog(QStringLiteral("T20"), idV,
                                          InstanceSettingsDialog::Override(),
                                          InstanceSettingsDialog::GlobalValues(), QStringList(),
                                          true);
            const QString datPath = dirV + QStringLiteral("/servers.dat");
            // (a) 盘上根本没有这个文件。
            QFile::remove(datPath);
            const bool invokedA = QMetaObject::invokeMethod(
                &w, "loadServerList", Qt::DirectConnection,
                Q_ARG(InstanceSettingsDialog *, &dialog), Q_ARG(QString, idV));
            // 产品的顺序：开窗那一刻就 `loadServerList()`（页还没建），用户点进「服务器」那一页
            // 才建 —— 建页时 `replayServerPage()` 要把先到的那一份摆上去。这一下同时钉住那条
            // 回放：读的时候页还是空的，切过去之后必须已经有了。
            dialog.showPage(InstanceSettingsDialog::serverPageKey());
            const QString noticeA = dialog.serverNotice() != nullptr
                                        ? dialog.serverNotice()->text()
                                        : QString();
            check(invokedA && noticeA == InstanceSettingsDialog::serverEmptyText(),
                  "★ T20.5 盘上没有 servers.dat 时说「还没加过服务器」——"
                  " **不是**「这份文件读不动」（用户会照着后一句去找一个坏掉的文件）",
                  noticeA);

            // (b) 有文件但是垃圾字节。
            writeFileAt(datPath, QByteArray(64, 'q'));
            QMetaObject::invokeMethod(&w, "loadServerList", Qt::DirectConnection,
                                      Q_ARG(InstanceSettingsDialog *, &dialog),
                                      Q_ARG(QString, idV));
            const QString noticeB = dialog.serverNotice() != nullptr
                                        ? dialog.serverNotice()->text()
                                        : QString();
            check(!noticeB.isEmpty() && noticeB != InstanceSettingsDialog::serverEmptyText() &&
                      dialog.serverTree() != nullptr && dialog.serverTree()->isHidden(),
                  "★ T20.5 有一份**读不动**的 servers.dat 时说的是为什么，而表格收起来"
                  "（两句不同的话 —— 混成一句就等于把用户的文件说成了「从没加过」）",
                  noticeB);
            QFile::remove(datPath);
        }

        // ---------- ⑥ pingServers：真的连上去问一遍----------
        //
        // 这一条是**整条路**上唯一一处端到端：servers.dat（真 NBT 字节）→ NbtUtil →
        // MainWindow::serverListOf() → ServerPing（真 TCP）→ ServerRowStatus → 那一页
        // 的表格。两头各自的那一半都别处验过了（拆包在 build/verify 的 ZH 节、画在
        // ui_check 的 U10L），缺的正是"中间这一节把结果装对没装对"——比如把 motd 装进
        // 地址那一格、把 -1 个玩家写成 0/0、或者图标解都没解就塞进去。
        //
        // 三台服务器各走一档：一台正常答（图标 / MOTD / 人数都在）、一台**连上但不答**
        // （超时）、一台**压根没人听**（连不上）。后两台的实时长由那个超时旋钮压着 ——
        // 不压的话这一节光等就要 8 秒起。
        {
            class FakeMcServer : public QThread {
            public:
                QByteArray json;
                bool silent = false;
                quint16 port = 0;
                int connections = 0;
                QSemaphore ready;

                void run() override {
                    QTcpServer server;
                    if (!server.listen(QHostAddress::LocalHost, 0)) {
                        ready.release();
                        return;
                    }
                    port = server.serverPort();
                    ready.release();
                    {
                        QEventLoop loop;
                        QObject::connect(&server, &QTcpServer::newConnection, &loop,
                                         &QEventLoop::quit);
                        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
                        if (!server.hasPendingConnections()) {
                            loop.exec();
                        }
                    }
                    QTcpSocket *socket = server.nextPendingConnection();
                    if (socket == nullptr) {
                        return;
                    }
                    ++connections;
                    // 收到第一条数据就往下走（内容不看：那个握手逐字段核对在 ZH6）。
                    bool got = false;
                    QElapsedTimer clock;
                    clock.start();
                    while (!got && clock.elapsed() < 2000) {
                        if (socket->waitForReadyRead(200)) {
                            socket->readAll();
                            got = true;
                        }
                    }
                    if (silent) {
                        socket->waitForDisconnected(3000);
                        delete socket;
                        return;
                    }
                    const QByteArray body =
                        ServerPing::varIntBytes(0x00) + ServerPing::varIntBytes(json.size()) + json;
                    socket->write(ServerPing::varIntBytes(body.size()) + body);
                    socket->waitForBytesWritten(1000);
                    socket->flush();
                    socket->waitForDisconnected(1500);
                    delete socket;
                }
            };

            // 一份"像真服务器答的" JSON（和 ZH6 那份同一个形状）。
            QJsonObject motd;
            motd[QStringLiteral("text")] = QStringLiteral("§a欢迎来到 ");
            QJsonObject colored;
            colored[QStringLiteral("text")] = QStringLiteral("本地服");
            motd[QStringLiteral("extra")] = QJsonArray{colored};
            QJsonObject players;
            players[QStringLiteral("online")] = 3;
            players[QStringLiteral("max")] = 8;
            QJsonObject version;
            version[QStringLiteral("name")] = QStringLiteral("原版 1.20.1");
            QJsonObject root;
            root[QStringLiteral("description")] = motd;
            root[QStringLiteral("players")] = players;
            root[QStringLiteral("version")] = version;
            // 图标：现造一张 16x16（不落盘），交给服务器当 favicon 发出来。
            QImage iconSource(16, 16, QImage::Format_ARGB32);
            iconSource.fill(QColor(0x22, 0x44, 0x88));
            QByteArray iconPng;
            {
                QBuffer buffer(&iconPng);
                buffer.open(QIODevice::WriteOnly);
                iconSource.save(&buffer, "PNG");
            }
            root[QStringLiteral("favicon")] =
                QStringLiteral("data:image/png;base64,") + QString::fromLatin1(iconPng.toBase64());

            FakeMcServer live;
            live.json = QJsonDocument(root).toJson(QJsonDocument::Compact);
            FakeMcServer quiet;
            quiet.silent = true;
            live.start();
            quiet.start();
            live.ready.acquire();
            quiet.ready.acquire();

            quint16 freePort = 0;
            {
                QTcpServer probe;
                if (probe.listen(QHostAddress::LocalHost, 0)) {
                    freePort = probe.serverPort();
                    probe.close();
                }
            }

            const QString datPath = dirV + QStringLiteral("/servers.dat");
            writeFileAt(datPath,
                        buildServersDat({qMakePair(QStringLiteral("本地服"),
                                                   QStringLiteral("127.0.0.1:%1").arg(live.port)),
                                         qMakePair(QStringLiteral("不答话的服"),
                                                   QStringLiteral("127.0.0.1:%1").arg(quiet.port)),
                                         qMakePair(QStringLiteral("没人听的服"),
                                                   QStringLiteral("127.0.0.1:%1").arg(freePort))}));

            InstanceSettingsDialog dialog2(QStringLiteral("T20"), idV,
                                           InstanceSettingsDialog::Override(),
                                           InstanceSettingsDialog::GlobalValues(), QStringList(),
                                           true);
            // 先走一遍真的"读列表"那一发 —— 表格里的行是它摆出来的（pingServers 只写
            // 每一行的状态，行的存在是这一步的事）。
            QMetaObject::invokeMethod(&w, "loadServerList", Qt::DirectConnection,
                                      Q_ARG(InstanceSettingsDialog *, &dialog2),
                                      Q_ARG(QString, idV));
            // 同上：列表先到、页后建（见 InstanceSettingsDialog::ensurePage()），这里照用户的
            // 走法进那一页 —— 三行是建页时 `replayServerPage()` 摆上去的。
            dialog2.showPage(InstanceSettingsDialog::serverPageKey());
            check(dialog2.serverTree() != nullptr &&
                      dialog2.serverTree()->topLevelItemCount() == 3,
                  "★ T20.6 那三台服务器从 servers.dat 里读出来了（自己拼的 NBT 字节过得了"
                  "产品的解析）",
                  QStringLiteral("%1 行")
                      .arg(dialog2.serverTree() != nullptr
                               ? dialog2.serverTree()->topLevelItemCount()
                               : -1));

            // 超时旋钮压到 300 毫秒 —— 这一节有两台是"要等到超时"的。
            const int savedTimeout = ServerPing::timeoutMs();
            ServerPing::setTimeoutMs(300);

            QElapsedTimer wall;
            wall.start();
            const bool invoked = QMetaObject::invokeMethod(
                &w, "pingServers", Qt::DirectConnection,
                Q_ARG(InstanceSettingsDialog *, &dialog2), Q_ARG(QString, idV));
            // 起 pingServers() **不再自己等**（整趟交给后台线程上的
            // `ServerPing::Pinger`，要求「现在如果没测完会锁住其他按钮，不能这样」）
            // —— 所以这一发回来时那一趟**还在跑**，「刷新中…」这一格现在就能直接读
            // 「测速」并进「刷新」之后，这一格挂的是**「刷新」那颗**。
            const QString buttonWhilePinging =
                dialog2.serverRefreshButton() != nullptr ? dialog2.serverRefreshButton()->text()
                                                      : QStringLiteral("<没这颗按钮>");
            QMetaObject::invokeMethod(&w, "pingServers", Qt::DirectConnection,
                                      Q_ARG(InstanceSettingsDialog *, &dialog2),
                                      Q_ARG(QString, idV));
            check(dialog2.serverRefreshButton() != nullptr &&
                      dialog2.serverRefreshButton()->text() ==
                          InstanceSettingsDialog::serverPingingText(),
                  "★ T20.6 跑到一半时又喊了一声（「刷新」那条路）：那颗按钮**还是**「刷新中…」"
                  "（这一声被那道闸接住了，没有重开一趟）",
                  dialog2.serverRefreshButton() != nullptr
                      ? dialog2.serverRefreshButton()->text()
                      : QStringLiteral("<没这颗按钮>"));
            // 等这一趟跑完：一台一条信号 + 收尾那一条都是从工作线程**排队**过来的，
            // 不转事件循环它们一条也到不了主线程（"没等到"才是红的那种，见下面那几条）。
            const bool pinged = pumpUntil(
                [&dialog2]() {
                    return dialog2.serverRefreshButton() != nullptr &&
                           dialog2.serverRefreshButton()->text() ==
                               InstanceSettingsDialog::serverRefreshText();
                },
                10000);
            const qint64 wallMs = wall.elapsed();
            ServerPing::setTimeoutMs(savedTimeout);

            check(invoked, "★ T20.6 点「刷新」那一条路（重新读一遍 + 重测）真的跑起来了");
            check(pinged, "★ T20.6 这一趟跑完了（三台的答复都排回来、收尾那一条也到了）");
            check(live.connections == 1 && quiet.connections == 1,
                  "★★ T20.6 两台假服务器各收到**一个**连接（真的连出去了，不是照本地猜的；"
                  "而且上面那一句重复的「刷新」没有让它重开一趟 —— 重开的话这里会是 2）",
                  QStringLiteral("答话的 %1 个、不答的 %2 个")
                      .arg(live.connections)
                      .arg(quiet.connections));
            check(buttonWhilePinging == InstanceSettingsDialog::serverPingingText(),
                  "★ T20.6 重测期间那颗按钮是「刷新中…」（那一页是活的，不是冻住的）",
                  buttonWhilePinging);

            QTreeWidget *tree2 = dialog2.serverTree();
            QTreeWidgetItem *liveItem = tree2 != nullptr ? tree2->topLevelItem(0) : nullptr;
            QTreeWidgetItem *quietItem = tree2 != nullptr ? tree2->topLevelItem(1) : nullptr;
            QTreeWidgetItem *deadItem = tree2 != nullptr ? tree2->topLevelItem(2) : nullptr;

            check(liveItem != nullptr &&
                      liveItem->text(2).endsWith(QStringLiteral(" ms\n3/8 人")) &&
                      !liveItem->text(2).contains(QStringLiteral("在线")),
                  "★ T20.6 通的那一台：「多少毫秒」+ 换行 + 「3/8 人」（没有「在线」两个字）",
                  liveItem != nullptr ? liveItem->text(2) : QStringLiteral("<没有这一行>"));
            check(liveItem != nullptr &&
                      liveItem->data(0, MainWindowUI::rowSubTextRole()).toString() ==
                          QStringLiteral("欢迎来到 本地服"),
                  "★ T20.6 它的 MOTD 装在第二行小字那一格（没装到地址那一格去），"
                  "而且颜色码是洗过的",
                  liveItem != nullptr
                      ? liveItem->data(0, MainWindowUI::rowSubTextRole()).toString()
                      : QStringLiteral("<没有这一行>"));
            check(liveItem != nullptr && !liveItem->icon(0).isNull(),
                  "★ T20.6 它的 favicon 解成图之后挂在图标那一格上（没解出来就是空的）");
            if (liveItem != nullptr && !liveItem->icon(0).isNull()) {
                const QImage shown = liveItem->icon(0).pixmap(16, 16).toImage();
                check(shown.size() == QSize(16, 16) && shown.pixelColor(8, 8) == QColor(0x22, 0x44, 0x88),
                      "★ T20.6 而且就是服务器发来的**那一张**（不是一张占位图）",
                      QStringLiteral("%1x%2 中心是 %3")
                          .arg(shown.width())
                          .arg(shown.height())
                          .arg(shown.pixelColor(8, 8).name()));
            }

            // 这一条是「超时的只写超时」的**端到端**那一半：假服务器真的收了连接、真的
            // 没答话，屏幕上那一格就只剩两个字。理由那半句（「连上了，但服务器没有回答」）
            // 仍在 ServerPing::Status::error 里 —— 它由 verify 那边的 ZH8 钉着。
            check(quietItem != nullptr &&
                      quietItem->text(2) == InstanceSettingsDialog::serverTimeoutText(),
                  "★★ T20.6 连上但不答话的那一台：那一格**只写「超时」**"
                  "（「超时的只写超时」；判据是 Status::timedOut，"
                  "不是拿 error 那句字符串去认前缀）",
                  quietItem != nullptr ? quietItem->text(2) : QStringLiteral("<没有这一行>"));
            check(deadItem != nullptr && deadItem->text(2) != InstanceSettingsDialog::serverUntestedText() &&
                      !deadItem->text(2).isEmpty(),
                  "★ T20.6 压根没人听的那一台也有话说（不是停留在「还没测」）",
                  deadItem != nullptr ? deadItem->text(2) : QStringLiteral("<没有这一行>"));
            check(quietItem != nullptr && deadItem != nullptr &&
                      quietItem->icon(0).isNull() && deadItem->icon(0).isNull() &&
                      quietItem->data(0, MainWindowUI::rowSubTextRole()).toString().isEmpty(),
                  "★ T20.6 没通的那两台：图标和 MOTD 都是空的（不留别人那一份）");
            check(dialog2.serverRefreshButton() != nullptr &&
                      dialog2.serverRefreshButton()->isEnabled() &&
                      dialog2.serverRefreshButton()->text() ==
                          InstanceSettingsDialog::serverRefreshText(),
                  "★ T20.6 测完按钮回到「刷新」、也点得动了（不允许永久灰着）",
                  dialog2.serverRefreshButton() != nullptr
                      ? dialog2.serverRefreshButton()->text()
                      : QStringLiteral("<没这颗按钮>"));
            check(wallMs < 5000, "T20.6 三台问完没有拖太久（超时那个旋钮真的管着它）",
                  QStringLiteral("用了 %1 ms").arg(wallMs));

            // ---------- ★「超时的只写超时」的另一半：**不是**超时的照旧写全话 ----------
            //
            // 上面那几条钉的是"真连出去"的那一半。反过来那一半没法拿"没人听的那一台"来钉：
            // **这台机器上连一个没人听的端口回来的也是「超时」**（本机那个过滤器把 SYN 吃掉
            // 了、没回 RST —— 同一条实测写在 verify 的 ZH7 里，那边也是因此才不敢断言"一定
            // 是「没有回应」"）。所以这一半拿**摆出来的一行**钉：真条目、真代理、真文本，
            // 只有 timedOut 这一位由我们说了算。
            //
            // NOTE: 这一下会把上面那三行一起盖掉（setServerStatuses 是整份盖）—— 它们的断言
            // **全都跑在上面了**，所以这一段摆在这一节的最后。第 2 行那两条才是这一段
            // 要看的：**同一句话**，只翻 timedOut 这一位，那一格从全话变成两个字 ——
            // 这就是"界面照的是那一位、不是那句话本身"的直接证据。
            if (deadItem != nullptr) {
                const QString fullError = QStringLiteral("没有回应（对面没开，或者端口不对）");
                QList<InstanceSettingsDialog::ServerRowStatus> crafted;
                crafted.append(InstanceSettingsDialog::ServerRowStatus());
                crafted.append(InstanceSettingsDialog::ServerRowStatus());
                InstanceSettingsDialog::ServerRowStatus refusedRow;
                refusedRow.tested = true;
                refusedRow.online = false;
                refusedRow.timedOut = false;  // 没通，但**不是**超时
                refusedRow.error = fullError;
                crafted.append(refusedRow);
                dialog2.setServerStatuses(crafted);
                check(deadItem->text(2) == fullError,
                      "★★ T20.6 没通但**不是超时**的那一行照旧写那句话的全话"
                      "（「超时」两个字只属于 timedOut 那一档 —— 别顺手把整档没通都缩了）",
                      deadItem->text(2));
                refusedRow.timedOut = true;
                crafted[2] = refusedRow;
                dialog2.setServerStatuses(crafted);
                check(deadItem->text(2) == InstanceSettingsDialog::serverTimeoutText(),
                      "★★ T20.6 同一句话、只把 timedOut 翻成真 ⇒ 那一格当场缩成「超时」"
                      "（判据是那一位，界面上不许拿 error 的字符串去认前缀）",
                      deadItem->text(2));
            }

            live.wait(8000);
            quiet.wait(8000);
        }

        // ---------- ⑥b 「编辑」/「加入服务器」那一对----------
        //
        // 要求：「在服务器列表那里做一个编辑地址和名称的功能，然后世界和
        // 服务器都做一个临时占位（加入选中的世界（仅限有这个参数的版本）/服务器）先试试看
        // 能不能生效」。
        //
        // 「编辑」那一发**只发信号**（那一页够不着 InlineMessage / OverlayHost，见
        // `InstanceSettingsDialog` 的文件头；问两句和落盘都在 MainWindow 里）—— 所以这里
        // 钉的是**发出去的那一线带的是什么**：
        // - 名字取的是 `serverNameRole()` 那一格（原样），**不是**屏幕上那行字。没名字的
        // 那一条屏幕上写着「（无名）」，而发出去的必须是**空串** —— 发成那三个字的话，
        // 用户点一下「编辑」再点确定，`servers.dat` 里就真多出一台叫「（无名）」的服务器；
        // - 地址取的是 `serverAddressRole()` 那一格（**完整的那一串**），不是第 1 栏里
        // 隐藏服务器地址打开时的遮过那串 `m-.h-.c-`；
        // - 没选中任何一行时那两颗是**灰的**，而且点了什么都不发（两道闸）。
        {
            const QString datPath = dirV + QStringLiteral("/servers.dat");
            // 第二条的 name 是**空串**（盘上有那一格、值是空的）。真机上 1.12.2 那代更彻底
            // —— 那一格根本没有；两种在界面和这一条路上走的是同一个分支（都读成空串）。
            writeFileAt(datPath,
                        buildServersDat({qMakePair(QStringLiteral("叫得出名字的"),
                                                   QStringLiteral("one.example.com")),
                                         qMakePair(QString(), QStringLiteral("two.example.com"))}));

            InstanceSettingsDialog dialog3(QStringLiteral("T20"), idV,
                                           InstanceSettingsDialog::Override(),
                                           InstanceSettingsDialog::GlobalValues(), QStringList(),
                                           true);
            QMetaObject::invokeMethod(&w, "loadServerList", Qt::DirectConnection,
                                      Q_ARG(InstanceSettingsDialog *, &dialog3),
                                      Q_ARG(QString, idV));
            // 同上：进那一页，那两颗按钮和两行才在。
            dialog3.showPage(InstanceSettingsDialog::serverPageKey());

            check(dialog3.serverEditButton() != nullptr &&
                      dialog3.serverEditButton()->objectName() ==
                          InstanceSettingsDialog::serverEditButtonName() &&
                      dialog3.serverEditButton()->text() ==
                          InstanceSettingsDialog::serverEditText(),
                  "T20.8 「编辑」那颗在，而且认得出它（按 objectName，不认文字）");
            check(dialog3.serverJoinButton() != nullptr &&
                      dialog3.serverJoinButton()->objectName() ==
                          InstanceSettingsDialog::serverJoinButtonName(),
                  "T20.8 「加入服务器」那颗也在");
            // 初值：`setServerJoinText()` 还没被喊过（这里是直接构造的，没走 MainWindow 那一趟）
            // ⇒ 它藏着 —— 不支持的版本上 MainWindow 递进来的就是**空串**。
            check(dialog3.serverJoinButton() != nullptr && dialog3.serverJoinButton()->isHidden(),
                  "T20.8 没接过那句文字的时候「加入服务器」藏着");

            QTreeWidget *tree3 = dialog3.serverTree();
            check(tree3 != nullptr && tree3->topLevelItemCount() == 2, "T20.8 两行都摆出来了",
                  QStringLiteral("%1 行")
                      .arg(tree3 != nullptr ? tree3->topLevelItemCount() : -1));

            QString sentEditName;
            QString sentEditAddress;
            int editEmitted = 0;
            QString sentJoinAddress;
            int joinEmitted = 0;
            QObject::connect(&dialog3, &InstanceSettingsDialog::editServerRequested, &dialog3,
                             [&](const QString &name, const QString &address) {
                                 sentEditName = name;
                                 sentEditAddress = address;
                                 ++editEmitted;
                             });
            QObject::connect(&dialog3, &InstanceSettingsDialog::joinServerRequested, &dialog3,
                             [&](const QString &, const QString &address) {
                                 sentJoinAddress = address;
                                 ++joinEmitted;
                             });

            // 没选中任何一行 ⇒ 两颗都灰着，而且点下去什么都不发。
            check(dialog3.serverEditButton() != nullptr &&
                      !dialog3.serverEditButton()->isEnabled() &&
                      dialog3.serverJoinButton() != nullptr &&
                      !dialog3.serverJoinButton()->isEnabled(),
                  "T20.8 没选中任何一行时那两颗都是灰的");
            check(dialog3.serverEditButton() != nullptr &&
                      dialog3.serverEditButton()->toolTip() ==
                          InstanceSettingsDialog::serverSelectFirstHint(),
                  "T20.8 灰着的时候悬停提示说的是「先选中一行」，不是它本来那句",
                  dialog3.serverEditButton() != nullptr
                      ? dialog3.serverEditButton()->toolTip()
                      : QStringLiteral("<没这颗按钮>"));
            dialog3.serverEditButton()->click();
            dialog3.serverJoinButton()->click();
            check(editEmitted == 0 && joinEmitted == 0,
                  "T20.8 灰着的时候点它们什么都不发（按钮那一道闸之外，信号里还有第二道）",
                  QStringLiteral("编辑发了 %1 次、加入发了 %2 次").arg(editEmitted).arg(joinEmitted));

            check(tree3 != nullptr && tree3->topLevelItemCount() == 2, "T20.8 两行还在");
            if (tree3 != nullptr && tree3->topLevelItemCount() == 2) {
                // ---- 第一行：有名字的那一条 ----
                tree3->setCurrentItem(tree3->topLevelItem(0));
                check(dialog3.serverEditButton()->isEnabled(),
                      "T20.8 选中一行之后「编辑」点得动了");
                check(dialog3.serverEditButton()->toolTip() ==
                          InstanceSettingsDialog::serverEditTip(),
                      "T20.8 有选中时悬停提示换回本来那句");
                dialog3.serverEditButton()->click();
                check(sentEditName == QStringLiteral("叫得出名字的") &&
                          sentEditAddress == QStringLiteral("one.example.com"),
                      "T20.8 「编辑」发出去的是这一行的**原名**和**完整地址**"
                      "（地址不是第 1 栏里遮过的那一串）",
                      QStringLiteral("%1 / %2").arg(sentEditName, sentEditAddress));

                // ---- 第二行：没名字的那一条（★ 这一条是这一段最值钱的）----
                tree3->setCurrentItem(tree3->topLevelItem(1));
                sentEditName = QStringLiteral("<没发>");
                sentEditAddress = QStringLiteral("<没发>");
                dialog3.serverEditButton()->click();
                check(sentEditName.isEmpty() && sentEditAddress == QStringLiteral("two.example.com"),
                      "★★ T20.7 没名字的那一条：屏幕上写着「（无名）」，而「编辑」发出去的是"
                      "**空串**（发成屏幕上那三个字的话，用户点一下确定就真多出一台叫"
                      "「（无名）」的服务器）",
                      QStringLiteral("屏幕上写「%1」，发出去的是「%2」")
                          .arg(tree3->topLevelItem(1)->text(0), sentEditName));

                // ---- 「加入服务器」：递了文字才露，点一下带的是**地址** ----
                dialog3.setServerJoinText(InstanceSettingsDialog::serverJoinText(),
                                          InstanceSettingsDialog::serverJoinTip());
                check(!dialog3.serverJoinButton()->isHidden() &&
                          dialog3.serverJoinButton()->isEnabled(),
                      "T20.8 递了那句文字之后「加入服务器」露出来、也点得动了"
                      "（当前选中的还是第二行）");
                dialog3.serverJoinButton()->click();
                check(joinEmitted == 1 &&
                          sentJoinAddress == QStringLiteral("two.example.com"),
                      "T20.8 点「加入服务器」带的是这一行的**地址**（也就是 servers.dat 里那一串）",
                      QStringLiteral("发了 %1 次，地址「%2」")
                          .arg(joinEmitted)
                          .arg(sentJoinAddress));

                // 空串 = 这个版本不认 `--quickPlayMultiplayer` ⇒ 收起来（不值得留一颗
                // 永远发不出去的按钮，那才是「按了没反应」）。
                dialog3.setServerJoinText(QString(), QString());
                check(dialog3.serverJoinButton()->isHidden(),
                      "T20.8 递空串进来 ⇒ 「加入服务器」当场收起来");

                // ---------- ⑥c 「添加」/「删除」那一对（ 稍后）----------
                //
                // 要求：「编辑要能改名字改地址以及删除，服务器那个页面里放一个添加
                // 服务器的按钮，允许添加」。和「编辑」一样，这两颗**只发信号**（这一层
                // 够不着 InlineMessage 和 src/，见 `InstanceSettingsDialog` 的文件头）。
                //
                // 这一段钉的是**两颗的可点性不一样**：
                // - 「添加」不跟着选中走 —— 要加的那一台还没出现在表里，没有"先选中"
                // 这一步，所以没选中时它**还是亮的**（连一条都没有的时候尤其要能按）；
                // - 「删除」动的是选中那一行，所以没选中时**灰着**，而且灰着时点下去
                // 什么都不发（两道闸，和「编辑」同）。
                // 还有一条：上面「测速」并进「刷新」之后，那一颗上的字得是「刷新」——
                // 屏幕上再找得着「测速」两个字就是合并没做干净。
                check(dialog3.serverRefreshButton() != nullptr &&
                          dialog3.serverRefreshButton()->text() ==
                              InstanceSettingsDialog::serverRefreshText(),
                      "T20.8 「刷新」那颗上的字就是「刷新」（没在测的时候）",
                      dialog3.serverRefreshButton() != nullptr
                          ? dialog3.serverRefreshButton()->text()
                          : QStringLiteral("<没这颗按钮>"));
                check(dialog3.serverAddButton() != nullptr &&
                          dialog3.serverAddButton()->objectName() ==
                              InstanceSettingsDialog::serverAddButtonName() &&
                          dialog3.serverAddButton()->text() ==
                              InstanceSettingsDialog::serverAddText(),
                      "T20.8 「添加」那颗在，而且认得出它（按 objectName，不认文字）");
                check(dialog3.serverDeleteButton() != nullptr &&
                          dialog3.serverDeleteButton()->objectName() ==
                              InstanceSettingsDialog::serverDeleteButtonName() &&
                          dialog3.serverDeleteButton()->text() ==
                              InstanceSettingsDialog::serverDeleteText(),
                      "T20.8 「删除」那颗也在");

                int addEmitted = 0;
                QString sentDeleteName;
                QString sentDeleteAddress;
                int deleteEmitted = 0;
                QObject::connect(&dialog3, &InstanceSettingsDialog::addServerRequested, &dialog3,
                                 [&]() { ++addEmitted; });
                QObject::connect(&dialog3, &InstanceSettingsDialog::deleteServerRequested, &dialog3,
                                 [&](const QString &name, const QString &address) {
                                     sentDeleteName = name;
                                     sentDeleteAddress = address;
                                     ++deleteEmitted;
                                 });

                // 把选中抹掉：**重列一遍表**（`loadServerList` 那条路 —— 「刷新」干的就是
                // 它）—— 整张表是重建的，选中自然没有了。不走 `clearSelection()`：那一下
                // 清的是 selection，而这一页"选中了没有"的判据是 `currentItem()`
                // （见 `selectedServerItem()`），清完 selection 之后它还指着原来那一行。
                QMetaObject::invokeMethod(&w, "loadServerList", Qt::DirectConnection,
                                          Q_ARG(InstanceSettingsDialog *, &dialog3),
                                          Q_ARG(QString, idV));
                check(tree3 != nullptr && tree3->currentItem() == nullptr,
                      "T20.8 重列之后没有任何一行是选中状态（这一节的起点）");
                check(dialog3.serverDeleteButton() != nullptr &&
                          !dialog3.serverDeleteButton()->isEnabled(),
                      "T20.8 没选中任何一行时「删除」是灰的");
                check(dialog3.serverAddButton() != nullptr &&
                          dialog3.serverAddButton()->isEnabled(),
                      "★ T20.8 但「添加」还是能按（要加的那一台还没出现在表里，"
                      "它没有「先选中」这一步）");
                check(dialog3.serverDeleteButton()->toolTip() ==
                          InstanceSettingsDialog::serverSelectFirstHint(),
                      "T20.8 灰着的时候「删除」的悬停提示也换成「先选中一行」",
                      dialog3.serverDeleteButton()->toolTip());
                dialog3.serverDeleteButton()->click();
                check(deleteEmitted == 0,
                      "T20.8 灰着的时候点「删除」什么都不发（两道闸）");
                dialog3.serverAddButton()->click();
                check(addEmitted == 1,
                      "T20.8 点「添加」发出去一发（**不带参数** —— 要加的那一台还没有名字"
                      "和地址可给 MainWindow）",
                      QStringLiteral("发了 %1 次").arg(addEmitted));

                // 选中没名字的那一条 ⇒ 「删除」发出去的是空串名字 + 完整地址（和「编辑」
                // 同一套取值：屏幕上写着「（无名）」，发出去的必须是**空串**）。
                tree3->setCurrentItem(tree3->topLevelItem(1));
                check(dialog3.serverDeleteButton()->isEnabled() &&
                          dialog3.serverDeleteButton()->toolTip() ==
                              InstanceSettingsDialog::serverDeleteTip(),
                      "T20.8 选中一行之后「删除」点得动、悬停提示换回本来那句");
                sentDeleteName = QStringLiteral("<没发>");
                sentDeleteAddress = QStringLiteral("<没发>");
                dialog3.serverDeleteButton()->click();
                check(deleteEmitted == 1 && sentDeleteName.isEmpty() &&
                          sentDeleteAddress == QStringLiteral("two.example.com"),
                      "T20.8 「删除」发出去的是这一行的**原名**（没名字的那条是空串）"
                      "和**完整地址**",
                      QStringLiteral("%1 / %2").arg(sentDeleteName, sentDeleteAddress));
            }
        }

        // ---------- ⑦ 「配置文件」那一页：点开一个文件 → 改 → 保存（真落盘）----------
        //
        // 这一节是整条路上**唯一一处让那些字节真落到盘上**的地方：ui_check 的 U11 量的是
        // "哪一格画了什么、发了哪个信号"，它**一个字节都不写盘**（那一层连 AppPaths 都够不着）。
        // 这里钉两道接缝：
        // ① 编辑器里那份正文经 MainWindow 的手落到盘上之后，**行尾还是原来那样** ——
        // 这一条错了的后果是用户改一个数字、整份配置被换成另一种行尾：模组照读，可
        // diff 从此全是假的，而界面上一个字都看不出来；
        // ② 「改过的东西不许悄悄没掉」那两个岔口（退回列表 / 收整页）**真的**先问了一句。
        //
        // 驱动走**产品那条路**（和上面 S2 / T18 同一形状）：开实例设置 → 文件管理 →
        // 配置文件 → 双击那一行 → 编辑器那一格。`openInstanceSettings` 会卡在嵌套事件循环
        // 里，所以"点哪儿"全写在下面那一发 `QTimer::singleShot(0, …)` 里 —— 它就在那个
        // 嵌套循环里跑，`pumpUntil()` 也 pump 得动（提示嵌在主窗口里，不是独立模态窗）。
        {
            const QString configDir = dirV + QStringLiteral("/config");
            QDir().mkpath(configDir);
            const QString jsonPath = configDir + QStringLiteral("/sodium-options.json");
            // 盘上这一份是 **CRLF + 结尾带换行**（Windows 上多数模组配置就长这样）。
            writeFileAt(jsonPath, QByteArrayLiteral("{\r\n  \"quality\": 1\r\n}\r\n"));

            bool sawEditor = false;      // 双击那一行之后编辑器那一格真的露出来了
            bool editable = false;
            bool cleanOnOpen = false;    // 刚摆进去是干净的（不干净的话一进来点返回就被问）
            int indexCount = -1;         // 认出来几条
            bool indexVisible = false;   // 那一格露着没有
            QString indexPicked;         // 点一条之后编辑框里选中了什么
            QString indexTip;            // 那一条的 tooltip（那份"唯一说明书"的落点）
            QString indexTextIntact;     // 跳完之后编辑框里那串正文（跳只选不改）
            QByteArray afterSave;        // 保存之后盘上那一份
            bool savedNotDirty = false;  // 存完 not dirty（不然退出去还会被问"改过没保存"）
            bool keptOnKeep = false;     // 答「留在这儿」时那一格还在、还是脏的
            bool stillDirtyOnKeep = false;
            QByteArray afterKeep;        // 答「留在这儿」时盘上没动
            bool backToListOnDiscard = false;  // 答「丢掉改动」时切回列表
            QByteArray afterDiscard;     // 答「丢掉改动」时盘上还是存下去的那一份
            bool pageStayedOpen = false; // 改过没保存时**整个实例设置页没关**
            int promptCount = 0;
            QString promptTitle;
            QStringList promptButtons;

            // 合成一发双击，落在视口上那一行的行首（行是代理画的，鼠标底下就是视口本身）。
            const auto dbl = [](QListWidget *list, QListWidgetItem *item) {
                QWidget *target = list->viewport();
                const QRect rect = list->visualItemRect(item);
                const QPoint pos(rect.x() + 10, rect.center().y());
                const auto one = [&target, &pos](QEvent::Type type, Qt::MouseButtons buttons) {
                    QMouseEvent event(type, QPointF(pos), QPointF(target->mapToGlobal(pos)),
                                      Qt::LeftButton, buttons, Qt::NoModifier);
                    QApplication::sendEvent(target, &event);
                };
                one(QEvent::MouseButtonPress, Qt::LeftButton);
                one(QEvent::MouseButtonRelease, Qt::NoButton);
                one(QEvent::MouseButtonDblClick, Qt::LeftButton);
                one(QEvent::MouseButtonRelease, Qt::NoButton);
                QApplication::processEvents();
            };

            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;  // 这一页没开起来：下面那几条会红，这里别崩
                }
                // 文件管理 → 配置文件（按**键**跳页：三页收起来之后，"第几行"和键表的下标
                // 已经不是一回事了，见 InstanceSettingsDialog::shownFilePageKeys()）。
                page->showPage(QStringLiteral("config"));
                QApplication::processEvents();

                ConfigFileEditorPage *editor = page->configFilePage();
                if (editor == nullptr) {
                    return;
                }
                const auto openByDoubleClick = [&] {
                    InstanceFilesPage *files = page->filesPage(QStringLiteral("config"));
                    if (files == nullptr || files->list() == nullptr) {
                        return;
                    }
                    files->list()->doItemsLayout();
                    for (int row = 0; row < files->list()->count(); ++row) {
                        QListWidgetItem *item = files->list()->item(row);
                        if (item == nullptr) {
                            continue;
                        }
                        const QString one =
                            QFileInfo(item->data(InstanceFilesPage::pathRole()).toString()).fileName();
                        if (one != QStringLiteral("sodium-options.json")) {
                            continue;
                        }
                        dbl(files->list(), item);
                        return;
                    }
                };

                openByDoubleClick();
                sawEditor = editor->isVisible();
                editable = editor->isEditable();
                cleanOnOpen = !editor->isDirty();

                indexCount = editor->indexEntryCount();
                indexVisible = editor->indexVisible();
                if (QTreeWidget *tree = editor->indexTree()) {
                    QTreeWidgetItem *leaf = nullptr;
                    std::function<void(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *item) {
                        if (item->childCount() == 0 &&
                            item->data(0, ConfigFileEditorPage::entryPathRole()).toString() ==
                                QStringLiteral("quality")) {
                            leaf = item;
                            return;
                        }
                        for (int i = 0; i < item->childCount(); ++i) {
                            find(item->child(i));
                        }
                    };
                    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                        find(tree->topLevelItem(i));
                    }
                    if (leaf != nullptr) {
                        indexTip = leaf->toolTip(0);
                        editor->jumpToEntry(leaf);
                    }
                }
                indexPicked = editor->editor()->textCursor().selectedText();
                indexTextIntact = editor->text();

                // 改一个值（框里一律是 `\n`），点「保存」—— 那一下走的是真信号线：
                // 编辑器的 saveRequested → MainWindow::saveConfigFile → QSaveFile。
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 5\n}\n"));
                editor->saveButton()->click();
                QApplication::processEvents();
                afterSave = readBytesAt(jsonPath);
                savedNotDirty = !editor->isDirty();

                // 再改一次（让它变脏），点「返回」—— 这一发必须**先问一句**。
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 9\n}\n"));
                {
                    PromptAnswerer keep({1});  // 1 = 「留在这儿」（那两颗是 0=丢掉、1=留下）
                    keep.start();
                    editor->backButton()->click();
                    pumpUntil([&keep] { return !keep.answered.isEmpty(); }, 5000);
                    keep.stop();
                    promptCount = keep.answered.size();
                    if (!keep.answered.isEmpty()) {
                        promptTitle = keep.answered.first().title;
                        promptButtons = keep.answered.first().buttons;
                    }
                }
                keptOnKeep = editor->isVisible();
                stillDirtyOnKeep = editor->isDirty();
                afterKeep = readBytesAt(jsonPath);

                // 再点一次「返回」—— 这回答「丢掉改动」。改过的东西**由用户看着它没掉**：
                // 那一格退回上一次存下去的那一份（不是退回"读进来那一份"），再切回列表。
                {
                    PromptAnswerer discard({0});
                    discard.start();
                    editor->backButton()->click();
                    pumpUntil([&discard] { return !discard.answered.isEmpty(); }, 5000);
                    discard.stop();
                }
                backToListOnDiscard = !editor->isVisible();
                afterDiscard = readBytesAt(jsonPath);

                // 再开一次、再改脏，然后点**顶上那颗返回箭头**（收整个实例设置页）：
                // 那是另一个岔口，同样必须先问一句 —— 不问的话用户按一下上面那颗箭头，
                // 刚改的东西就没了（这正是 .hpp 里"改过的东西不许悄悄没掉"那一条）。
                openByDoubleClick();
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality = 7\"\n}\n"));
                QPushButton *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName());
                if (back != nullptr) {
                    {
                        PromptAnswerer keep({1});
                        keep.start();
                        back->click();
                        pumpUntil([&keep] { return !keep.answered.isEmpty(); }, 5000);
                        keep.stop();
                    }
                    pageStayedOpen = page->isVisible();
                    // 再来一次，这回答「丢掉改动」—— 页面这才真的收掉。
                    {
                        PromptAnswerer discard({0});
                        discard.start();
                        back->click();
                        pumpUntil([&discard] { return !discard.answered.isEmpty(); }, 5000);
                        discard.stop();
                    }
                } else if (host->pageOpen()) {
                    // 兜底（那颗箭头没找着时）：至少别让测试卡在这一层的嵌套循环里。
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(sawEditor && editable && cleanOnOpen,
                  "★★ T20.7 双击 config/ 底下一行 → 那一格露出来、可编辑、而且**刚摆进去是"
                  "干净的**（不干净的话用户一进来点「返回」就被问「改过要不要保存」）",
                  QStringLiteral("露出=%1 可编辑=%2 干净=%3")
                      .arg(int(sawEditor))
                      .arg(int(editable))
                      .arg(int(cleanOnOpen)));
            check(afterSave == QByteArrayLiteral("{\r\n  \"quality\": 5\r\n}\r\n"),
                  "★★ T20.7 点「保存」之后**盘上那一份真的变了**，而且行尾还是 CRLF、"
                  "结尾那个换行也还在 —— 写回去的是「整份字节」，不是编辑器里那串 `\\n`",
                  QString::fromUtf8(afterSave));
            check(savedNotDirty,
                  "★ T20.7 存完那一格不脏了（不脏的话用户接着点「返回」又被问一次"
                  "「改过要不要丢掉」——它明明刚存过）");
            check(promptCount == 1 && promptTitle == QStringLiteral("世界设置有未保存的更改"),
                  "★★ T20.7 改过之后点「返回」**先问一句**（那一问的字是 MainWindow 给的，"
                  "两处共用同一份）",
                  QStringLiteral("%1 条 / %2").arg(promptCount).arg(promptTitle));
            check(keptOnKeep && stillDirtyOnKeep && afterKeep == afterSave,
                  "★★ T20.7 答「留在这儿」时那一格还在、改动还在、盘上也没动 —— 手一滑按到"
                  "「Esc」或者点到卡片外面**不该把用户刚改的东西丢掉**");
            check(backToListOnDiscard && afterDiscard == afterSave,
                  "★ T20.7 答「丢掉改动」时切回文件列表，而且盘上还是**上一次存下去的那一份**"
                  "（「丢掉」丢的是框里那些字，不是盘上那份）");
            check(pageStayedOpen,
                  "★★ T20.7 编辑器里改脏了之后再点**顶上那颗返回箭头**：整个实例设置页"
                  "**不关** —— 用户还没回答丢不丢，这一页就不许消失（不然改的东西悄悄没了）");
            // 那一页收掉之后，对话框本身是 `deleteLater()` 收的 —— 测试里
            // `processEvents()` 够不着那条 DeferredDelete（嵌套层数对不上），得显式送一发。
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(!w.findChild<InstanceSettingsDialog *>(),
                  "★★ T20.7 答了「丢掉改动」之后那一页才真的收掉");
            check(indexCount == 1 && indexVisible,
                  "★★ T20.8 **盘上那份真配置**读进来之后，左边那一格认出了那一条"
                  "（`quality`）并且露着 —— 认的要是拿手搓的正文，就验不到「真文件读出来"
                  "认不认得出」这一趟",
                  QStringLiteral("认出 %1 条 / 露出=%2").arg(indexCount).arg(int(indexVisible)));
            check(indexPicked == QStringLiteral("1"),
                  "★★ T20.8 点一条 ⇒ 编辑框里**正好那一格值被选中**（这份文件里是 `1`）——"
                  "选歪一个字符的话，用户照着索引去改，改掉的是旁边那个逗号或者引号",
                  QStringLiteral("选中 <%1>").arg(indexPicked));
            check(indexTextIntact == QStringLiteral("{\n  \"quality\": 1\n}\n"),
                  "★★ T20.8 跳过去**只选不改**：框里那串正文一个字符都没动（索引是只读的，"
                  "它身上一个字节的写风险都不该有）",
                  indexTextIntact);
            check(indexTip.contains(QStringLiteral("quality")) &&
                      indexTip.contains(QStringLiteral("数字")),
                  "★ T20.8 那一条的 tooltip 里有键路径和**类型**（「数字」）—— 这份文件里没有"
                  "注释，所以那份说明书就只有类型这一行",
                  indexTip);
        }

        // ---------- ⑦.5 概览页那颗「检查更新」----------
        //
        // 要求：「实例设置那里做一个更新检测」。这一颗要钉的是**接线**：
        // ui_check 的 U0h3 只验到"点一下发了信号"，而"那个信号有没有人接、接了之后有没有
        // 出结果"只有把真的 MainWindow 立起来才验得到 —— 漏了 `MainWindow.cpp` 里那条
        // `connect(… checkUpdatesRequested …)` 的症状是"**按了没反应**"，屏幕上一点异常
        // 都没有（这正是这一整套里最难查的一类）。
        //
        // 走的是**一个空的实例**（连 mods/ 都不存在）：四个"能下载的资源"目录一个都没有
        // ⇒ 那一趟连一个网络请求都不发，直接出一句「这个实例里没有可以查更新的文件。」
        // —— 这一条是**完全离线**的。有文件的实例要走反查 + 每个项目一发版本列表，验证
        // 工程里不许真联网（那条路的内核在纯函数那一层，见主验证工程 Z6e 那一段）。
        //
        // 判据读的是**那一条提示说了什么**：一个框都没出现 = 按钮没接上；弹出的是别的
        // 东西 = 答话器照样把它记下来，断言看到的是"多弹了一个"（不是恒真）。
        {
            const QString idE = QStringLiteral("1.98.8");
            makeInstalledVersion(versionsDir, idE);
            // 一个资源目录都不建（这一条的全部意义就是"没有可以查更新的文件"）。

            PromptAnswerer answerer({0});
            answerer.start();
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;  // 这一页没开起来：下面那几条会红，这里别崩
                }
                page->showPage(QStringLiteral("mod"));
                QApplication::processEvents();

                if (auto *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();  // 返回箭头 = reject
                } else {
                    host->closeOverlay();  // 兜底：至少别让测试卡在这一层上
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idE));
            QApplication::processEvents();
            answerer.stop();

            check(answerer.answered.isEmpty(), "empty instance update check remains silent");

        }

        // ---------- ⑦.6 原理图那一页底下那条「这一份是什么」----------
        //
        // 这一条**只有把真页立起来才验得到**：那一条连接在 `InstanceDialogController` 里
        // （选中一行 → 150 ms 合并 → `SchematicInfo::read()` → 摆回那一格），ui_check 够
        // 不着控制器。盘上放一份**真形状**的 Sponge 原理图（`Palette` + `BlockData`），
        // 选它，看那一格先写「正在读取…」、过一会儿换成元数据与方块统计。
        {
            const QString schematicPath = dirV + QStringLiteral("/schematics/真原理图.schematic");
            {
                NbtTree::Node sponge;
                sponge.type = NbtTree::Type::Compound;
                const auto addNode = [&sponge](const QString &name, const NbtTree::Node &node) {
                    sponge.fields.append(qMakePair(name, node));
                };
                const auto intNode = [](qint64 value) {
                    NbtTree::Node node;
                    node.type = NbtTree::Type::Int;
                    node.number = value;
                    return node;
                };
                addNode(QStringLiteral("Width"), intNode(3));
                addNode(QStringLiteral("Height"), intNode(1));
                addNode(QStringLiteral("Length"), intNode(2));
                addNode(QStringLiteral("DataVersion"), intNode(3465));
                NbtTree::Node metadata;
                metadata.type = NbtTree::Type::Compound;
                NbtTree::Node nameNode;
                nameNode.type = NbtTree::Type::String;
                nameNode.text = QStringLiteral("真原理图");
                metadata.fields.append(qMakePair(QStringLiteral("Name"), nameNode));
                addNode(QStringLiteral("Metadata"), metadata);
                NbtTree::Node palette;
                palette.type = NbtTree::Type::Compound;
                const auto addState = [&palette](const QString &name, qint64 index) {
                    NbtTree::Node node;
                    node.type = NbtTree::Type::Int;
                    node.number = index;
                    palette.fields.append(qMakePair(name, node));
                };
                addState(QStringLiteral("minecraft:air"), 0);
                addState(QStringLiteral("minecraft:stone"), 1);
                addState(QStringLiteral("minecraft:oak_planks"), 2);
                addNode(QStringLiteral("Palette"), palette);
                NbtTree::Node blockData;
                blockData.type = NbtTree::Type::ByteArray;
                blockData.bytes = QByteArray::fromHex("00010201");  // air / stone / oak / stone
                addNode(QStringLiteral("BlockData"), blockData);
                NbtTree::Document document;
                document.root = sponge;
                writeFileAt(schematicPath, NbtTree::serialize(document));
            }

            bool labelFound = false;
            bool hiddenAtFirst = false;
            bool pendingSeen = false;
            bool otherPanelOff = false;
            QString infoText;
            QString infoTip;
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;  // 这一页没开起来：下面那几条会红，这里别崩
                }
                page->showPage(QStringLiteral("schematic"));
                QApplication::processEvents();
                InstanceFilesPage *files = page->filesPage(QStringLiteral("schematic"));
                if (files == nullptr) {
                    return;
                }
                QLabel *info = files->findChild<QLabel *>(InstanceFilesPage::infoLabelName());
                labelFound = info != nullptr;
                hiddenAtFirst = info != nullptr && info->isHidden();
                QListWidget *list = files->list();
                QListWidgetItem *targetItem = nullptr;
                for (int i = 0; list != nullptr && i < list->count(); ++i) {
                    QListWidgetItem *item = list->item(i);
                    if (item != nullptr &&
                        item->data(InstanceFilesPage::pathRole()).toString() == schematicPath) {
                        targetItem = item;
                        break;
                    }
                }
                if (targetItem == nullptr || info == nullptr) {
                    return;
                }
                list->setCurrentItem(targetItem);
                QApplication::processEvents();
                // 步骤 126：等的时候字清掉、上面挂一行加载态（LoadingState，过 150ms 才露「正在读取…」）。
                pendingSeen = info->isVisible() && info->text().isEmpty() && LoadingState::overlay(info) != nullptr;
                // 那一发合并定时器是 150 ms，读盘还要一会儿 —— 最多等 1.5 秒。
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 1500 && LoadingState::overlay(info) != nullptr) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(5);
                }
                infoText = info->text();
                infoTip = info->toolTip();
                // 这一条要在**收页之前**量：收掉之后这一页就没了。
                InstanceFilesPage *modsPage = page->filesPage(QStringLiteral("mod"));
                otherPanelOff = modsPage != nullptr && !modsPage->infoPanelEnabled();
                if (modsPage != nullptr) {
                    if (QLabel *modsLabel =
                            modsPage->findChild<QLabel *>(InstanceFilesPage::infoLabelName())) {
                        otherPanelOff = otherPanelOff && modsLabel->isHidden();
                    }
                }
                if (auto *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(labelFound && hiddenAtFirst,
                  "★ T20.11 原理图那一页有「这一份是什么」那一格，而且**什么都没选的时候藏着**"
                  "（别的页上它压根不开，一个像素都不变）");
            check(pendingSeen,
                  "★ T20.11 选中一份投影 ⇒ 立即挂上加载态、上一份的字清掉");
            check(infoText.contains(QStringLiteral("真原理图")) &&
                      infoText.contains(QStringLiteral("3 × 1 × 2")) &&
                      infoText.contains(QStringLiteral("3 个方块")) &&
                      infoText.contains(QStringLiteral("Sponge 原理图")),
                  "★★ T20.11 过一会儿那一格换成真读出来的元数据与方块统计"
                  "（名字 / 尺寸 / 方块数 / 格式）",
                  infoText);
            check(!infoText.isEmpty(),
                  "★ T20.11 那一格真的被接上了（停在「正在读取…」= 控制器那条连接漏了）");
            check(infoTip.contains(QStringLiteral("minecraft:stone：2")),
                  "★ T20.11 悬停提示里是方块排行（最多的那种真数出来了）", infoTip);
            check(otherPanelOff,
                  "★★ T20.11 别的页（模组）上那一格是关着的 —— 只有原理图那一页开");
        }

        // ---------- ⑦.7 Replay 那一页底下那条「这一份是什么」----------
        {
            const QString replayDir = AppPaths::replayRecordingsDir();
            QDir().mkpath(replayDir);
            const QString replayPath = replayDir + QStringLiteral("/测试录像.mcpr");
            const QString metaSource = dirV + QStringLiteral("/replay-meta.json");
            writeFileAt(metaSource, QByteArray(R"({
                "singleplayer": false,
                "serverName": "mc.example.com",
                "duration": 125000,
                "date": 1690000000000,
                "mcversion": "1.20.1",
                "generator": "ReplayMod v2.6.11",
                "protocol": 763,
                "players": ["a", "b"]
            })"));
            const QString recordingSource = dirV + QStringLiteral("/replay-recording.bin");
            writeFileAt(recordingSource, QByteArray(8, '\0'));
            QString replayWhy;
            const bool replayBuilt = ZipUtil::createZip(
                replayPath,
                {{QStringLiteral("metaData.json"), metaSource},
                 {QStringLiteral("recording.tmcpr"), recordingSource}},
                &replayWhy);
            check(replayBuilt, "★ T20.12 造一份真 `.mcpr`（zip + metaData.json）", replayWhy);

            bool replayLabelFound = false;
            bool replayPendingSeen = false;
            QString replayInfoText;
            QString replayInfoTip;
            QString replayRowText;
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;
                }
                page->showPage(QStringLiteral("replay"));
                QApplication::processEvents();
                auto *list = page->findChild<QListWidget *>(
                    QStringLiteral("instanceLifecycleFiles_replay"));
                QLabel *info = page->findChild<QLabel *>(
                    InstanceSettingsDialog::lifecycleInfoLabelName(QStringLiteral("replay")));
                replayLabelFound = list != nullptr && info != nullptr;
                if (list == nullptr || info == nullptr) {
                    return;
                }
                QListWidgetItem *targetItem = nullptr;
                for (int i = 0; i < list->count(); ++i) {
                    QListWidgetItem *item = list->item(i);
                    if (item != nullptr && item->data(Qt::UserRole).toString() == replayPath) {
                        targetItem = item;
                        break;
                    }
                }
                if (targetItem == nullptr) {
                    return;
                }
                list->setCurrentItem(targetItem);
                QApplication::processEvents();
                // 这一页是普通 QListWidgetItem（没有行控件），第二行就是「时间 / 大小」。
                replayRowText = targetItem->text().section(QLatin1Char('\n'), 1);
                replayPendingSeen =
                    info->isVisible() && info->text() == InstanceSettingsDialog::lifecycleInfoPendingText();
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 3000
                       && info->text() == InstanceSettingsDialog::lifecycleInfoPendingText()) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(5);
                }
                replayInfoText = info->text();
                replayInfoTip = info->toolTip();
                if (auto *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(replayLabelFound,
                  "★ T20.12 Replay 那一页有「这一份是什么」那一格");
            check(replayPendingSeen,
                  "★ T20.12 选中一份录像 ⇒ 立即写「正在读取…」");
            check(replayInfoText.contains(QStringLiteral("2 分 5 秒"))
                      && replayInfoText.contains(QStringLiteral("1.20.1"))
                      && replayInfoText.contains(QStringLiteral("多人（mc.example.com）"))
                      && replayInfoText.contains(QStringLiteral("2 位玩家"))
                      && replayInfoText.contains(QStringLiteral("ReplayMod v2.6.11")),
                  "★★ T20.12 过一会儿那一格换成真读出来的元数据"
                  "（时长 / 版本 / 多人 + 服务器 / 玩家数 / 录制器）",
                  replayInfoText);
            check(replayInfoText != InstanceSettingsDialog::lifecycleInfoPendingText()
                      && !replayInfoText.isEmpty(),
                  "★ T20.12 那一格真的被接上了（停在「正在读取…」= 控制器那条连接漏了）");
            check(replayInfoTip.contains(QStringLiteral("录制时间："))
                      && replayInfoTip.contains(QStringLiteral("文件大小：")),
                  "★ T20.12 悬停提示里补上了录制时间与文件大小", replayInfoTip);
            check(replayRowText.contains(QStringLiteral(" / ")),
                  "★ T20.12 列表行第二行是「时间 / 大小」那一条细节（不只是时间戳）",
                  replayRowText);
        }

        // ---------- ⑦.8 日志页：单击出信息（行数在后台数）、双击在内置查看器里打开 ----------
        {
            const QString logsDir = InstancePagesController::fileDirs(idV).value(QStringLiteral("logs"));
            QDir().mkpath(logsDir);
            QByteArray latest;
            for (int i = 0; i < 6000; ++i)
                latest += "[12:00:00] [main/INFO]: 第 " + QByteArray::number(i) + " 行日志"
                          + QByteArray(i % 50 == 0 ? " [main/WARN]" : "") + QByteArray(1, 10);
            writeFileAt(logsDir + QStringLiteral("/latest.log"), latest);
            QByteArray oldBody;
            for (int i = 0; i < 300; ++i)
                oldBody += "[11:00:00] [main/INFO]: 旧日志第 " + QByteArray::number(i) + " 行" + QByteArray(1, 10);
            const QString oldPlain = dirV + QStringLiteral("/old-log-source.txt");
            writeFileAt(oldPlain, oldBody);
            const QString gzPath = logsDir + QStringLiteral("/2026-11-18-1.log.gz");
            QString gzWhy;
            const bool gzBuilt = ZipUtil::gzipFrom(oldPlain, gzPath, &gzWhy);
            check(gzBuilt, "★ T20.13b 造一份 .log.gz", gzWhy);

            QString infoText, infoTip;
            bool pendingSeen = false;
            bool viewerFound = false;
            QString viewerTitle, viewerLast;
            int viewerLines = -1;
            QString latestViewerTitle;
            int latestLimit = -1, latestLines = -1;
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) return;
                page->showPage(QStringLiteral("logs"));
                QApplication::processEvents();
                auto *list = page->findChild<QListWidget *>(QStringLiteral("instanceLifecycleFiles_logs"));
                QLabel *info = page->findChild<QLabel *>(
                    InstanceSettingsDialog::lifecycleInfoLabelName(QStringLiteral("logs")));
                if (list == nullptr || info == nullptr) return;
                const auto itemFor = [list](const QString &name) -> QListWidgetItem * {
                    for (int i = 0; i < list->count(); ++i)
                        if (list->item(i)->data(Qt::UserRole).toString().endsWith(name)) return list->item(i);
                    return nullptr;
                };
                const auto waitUntil = [](const std::function<bool()> &done, int ms) {
                    QElapsedTimer timer;
                    timer.start();
                    while (timer.elapsed() < ms && !done()) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        QThread::msleep(5);
                    }
                };
                // 单击 gz：先是「正在读取…」，后台解压数完后补成「300 行」
                QListWidgetItem *gzItem = itemFor(QStringLiteral("2026-11-18-1.log.gz"));
                QListWidgetItem *latestItem = itemFor(QStringLiteral("latest.log"));
                if (gzItem == nullptr || latestItem == nullptr) return;
                list->setCurrentItem(gzItem);
                gzItem->setSelected(true);
                QApplication::processEvents();
                pendingSeen = info->isVisible() && info->text() == InstanceSettingsDialog::lifecycleInfoPendingText();
                waitUntil([&] { return info->text().contains(QStringLiteral("300 行")); }, 15000);
                infoText = info->text();
                infoTip = info->toolTip();
                // 双击 gz：内置查看器
                emit list->itemActivated(gzItem);
                GameLogWindow *viewer = nullptr;
                const auto findViewer = [&](const QString &name) {
                    for (QWidget *top : QApplication::topLevelWidgets())
                        if (auto *candidate = qobject_cast<GameLogWindow *>(top))
                            if (candidate->isFileViewer() && candidate->filePath().endsWith(name)) return candidate;
                    return static_cast<GameLogWindow *>(nullptr);
                };
                waitUntil([&] {
                    viewer = findViewer(QStringLiteral("2026-11-18-1.log.gz"));
                    return viewer != nullptr && viewer->lineCount() > 0;
                }, 20000);
                viewerFound = viewer != nullptr;
                if (viewer != nullptr) {
                    viewerTitle = viewer->windowTitle();
                    viewerLines = viewer->lineCount();
                    viewerLast = viewer->allLogText().trimmed().section(QLatin1Char(10), -1);
                    viewer->close();
                }
                // 双击 latest.log：6000 行 > 默认末尾 5000
                list->clearSelection();
                emit list->itemActivated(latestItem);
                GameLogWindow *latestViewer = nullptr;
                waitUntil([&] {
                    latestViewer = findViewer(QStringLiteral("latest.log"));
                    return latestViewer != nullptr && latestViewer->lineCount() > 0;
                }, 20000);
                if (latestViewer != nullptr) {
                    latestViewerTitle = latestViewer->windowTitle();
                    latestLimit = latestViewer->displayLineLimit();
                    latestLines = latestViewer->lineCount();
                    latestViewer->close();
                }
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                if (auto *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection, Q_ARG(QString, idV));
            QApplication::processEvents();

            check(pendingSeen, "★ T20.13b 单击一份日志 ⇒ 立即写「正在读取…」");
            check(infoText.contains(QStringLiteral("2026-11-18-1.log.gz")) && infoText.contains(QStringLiteral("300 行")),
                  "★★ T20.13b 后台解压并数完行数后，信息栏补上文件名与行数", infoText);
            check(infoTip.contains(QStringLiteral("2026-11-18-1.log.gz")), "★ T20.13b 悬停提示是完整路径", infoTip);
            check(viewerFound && viewerTitle.contains(QStringLiteral("2026-11-18-1.log.gz")),
                  "★★ T20.13b 双击 .log.gz 在内置查看器里打开，标题是文件名", viewerTitle);
            check(viewerLines == 300 && viewerLast == QStringLiteral("[11:00:00] [main/INFO]: 旧日志第 299 行"),
                  "★★ T20.13b .log.gz 解压后整份显示（300 行，末行是最后一行）",
                  QString::number(viewerLines) + QStringLiteral(" / ") + viewerLast);
            check(latestViewerTitle.contains(QStringLiteral("latest.log")) && latestLines == 6000
                      && latestLimit == GameLogWindow::fileTailLines(),
                  "★★ T20.13b 6000 行的 latest.log 全部读进模型，默认只显示末尾",
                  QString::number(latestLines) + QStringLiteral(" / ") + QString::number(latestLimit));
        }

        // `instanceDiagnosisReport` 里那份字是 `InstanceDialogController` 把实例 / Java /
        // 内存 / 模组 / 日志 / 崩溃报告拼起来交给 `StartupDiagnosis::analyze()` 算的——
        // ui_check 只量得到纯函数，**"这条线接没接上"在这里量**。另加一条：「重新检查」
        // 按下去真的再算一遍（先塞一句哨兵，按完哨兵必须没了）。
        {
            bool reportFound = false;
            bool autoFilled = false;
            bool rerunReplaced = false;
            QString reportText;
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;
                }
                page->showPage(QStringLiteral("diagnosis"));
                QApplication::processEvents();
                auto *report = page->findChild<QPlainTextEdit *>(
                    InstanceSettingsDialog::diagnosisReportName());
                auto *refresh = page->findChild<QPushButton *>(
                    InstanceSettingsDialog::diagnosisRefreshButtonName());
                reportFound = report != nullptr && refresh != nullptr;
                if (report == nullptr || refresh == nullptr) {
                    return;
                }
                QElapsedTimer timer;
                timer.start();
                while (timer.elapsed() < 3000
                       && (report->toPlainText() == InstanceSettingsDialog::diagnosisPendingText()
                           || report->toPlainText().isEmpty())) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(5);
                }
                reportText = report->toPlainText();
                autoFilled = reportText.contains(QStringLiteral("启动诊断："))
                             && reportText != InstanceSettingsDialog::diagnosisPendingText();

                page->setDiagnosisText(QStringLiteral("哨兵：重新检查之前的旧报告"));
                refresh->click();
                timer.restart();
                while (timer.elapsed() < 3000
                       && report->toPlainText() == QStringLiteral("哨兵：重新检查之前的旧报告")) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(5);
                }
                rerunReplaced = report->toPlainText().contains(QStringLiteral("启动诊断："))
                                && report->toPlainText() != QStringLiteral("哨兵：重新检查之前的旧报告");

                if (auto *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(reportFound,
                  "★ T20.13 「诊断」那一页有报告那一格和「重新检查」那一颗");
            check(autoFilled,
                  "★★ T20.13 一进这一页，报告那一格就换成了算出来的内容"
                  "（停在「正在检查…」= 控制器那条连接漏了）",
                  reportText.left(200));
            check(reportText.contains(QStringLiteral("实例：")),
                  "★ T20.13 报告第二行说这是哪个实例", reportText.left(200));
            check(rerunReplaced,
                  "★ T20.13 按「重新检查」⇒ 真再算一遍（哨兵被换掉了）");
        }

        // ---------- ⑦.9 「迁移」/「升级」那两页----------
        //
        // 判据层（`TransferPlanner` / `TransferApply`）由 verify.exe 的 TR 节逐条钉住；
        // 这里只量**这条线接没接上**：
        // ① 一进「迁移」页，目标下拉框里就摆着"这个游戏文件夹里别的版本"（且不含自己）；
        // ② 计划正文真的算出来了（`setTransferPlan` / `setTransferItems` 两个 setter 都被喂了）；
        // ③ 默认勾选真的摆到了框里（存档勾着、日志不勾 —— 记录类永不默认在 TR1 里钉过，
        // 这里钉的是"那一条结论有没有落进界面"）；
        // ④ 动手那颗按钮听的是勾选：全不勾 ⇒ 灰、再勾一项 ⇒ 又亮；
        // ⑤ 「升级」页用的是同一个构建器，顶上是「升级副本计划：」而不是迁移。
        {
            bool migrationFound = false;
            bool targetPicked = false;
            bool planFilled = false;
            bool checkboxDefaults = false;
            bool applyGate = false;
            bool upgradeFilled = false;
            QString migrationPlanText;
            QString upgradePlanText;
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;
                }
                page->showPage(QStringLiteral("migration"));
                QApplication::processEvents();
                const QString migrationKey = QStringLiteral("migration");
                auto *combo = page->findChild<QComboBox *>(
                    InstanceSettingsDialog::transferTargetComboName(migrationKey));
                auto *plan = page->findChild<QPlainTextEdit *>(
                    InstanceSettingsDialog::transferPlanTextName(migrationKey));
                auto *list = page->findChild<QListWidget *>(
                    InstanceSettingsDialog::transferItemsListName(migrationKey));
                auto *apply = page->findChild<QPushButton *>(
                    InstanceSettingsDialog::transferApplyButtonName(migrationKey));
                migrationFound = combo != nullptr && plan != nullptr && list != nullptr
                                && apply != nullptr;
                if (migrationFound) {
                    targetPicked = combo->count() > 0
                                   && !combo->currentData().toString().isEmpty()
                                   && combo->currentData().toString() != idV;
                    // 计划在后台读取目录；保留结果断言，但不要求打开页面同步扫完整棵目录。
                    QElapsedTimer previewWait;
                    previewWait.start();
                    while (!plan->toPlainText().contains(QStringLiteral("迁移计划："))
                           && previewWait.elapsed() < 5000) {
                        QApplication::processEvents(QEventLoop::AllEvents, 10);
                        QThread::msleep(1);
                    }
                    migrationPlanText = plan->toPlainText();
                    planFilled = migrationPlanText.contains(QStringLiteral("迁移计划："))
                                 && !migrationPlanText.contains(QStringLiteral("找不到源实例目录"));

                    bool savesChecked = false;
                    bool logsUnchecked = false;
                    bool logsPresent = false;
                    for (int i = 0; i < list->count(); ++i) {
                        const QListWidgetItem *item = list->item(i);
                        if (item == nullptr) continue;
                        const QString key = item->data(Qt::UserRole).toString();
                        if (key == QStringLiteral("saves") && item->checkState() == Qt::Checked) {
                            savesChecked = true;
                        }
                        if (key == QStringLiteral("logs")) {
                            logsPresent = true;
                            logsUnchecked = item->checkState() == Qt::Unchecked;
                        }
                    }
                    checkboxDefaults = savesChecked && logsPresent && logsUnchecked;

                    // 按钮状态由 `updateTransferApplyState()` 算：目标 + 至少一项勾选。
                    const bool enabledWithSelection = apply->isEnabled();
                    for (int i = 0; i < list->count(); ++i) {
                        list->item(i)->setCheckState(Qt::Unchecked);
                    }
                    QApplication::processEvents();
                    const bool disabledWithoutSelection = !apply->isEnabled();
                    if (list->count() > 0) {
                        list->item(0)->setCheckState(Qt::Checked);
                    }
                    QApplication::processEvents();
                    applyGate = enabledWithSelection && disabledWithoutSelection
                                && apply->isEnabled();
                }

                page->showPage(QStringLiteral("upgrade"));
                QApplication::processEvents();
                if (auto *upgradePlan = page->findChild<QPlainTextEdit *>(
                        InstanceSettingsDialog::transferPlanTextName(
                            QStringLiteral("upgrade")))) {
                    upgradePlanText = upgradePlan->toPlainText();
                    auto *game = page->findChild<QComboBox *>(QStringLiteral("instanceUpdateGame"));
                    auto *loader = page->findChild<QComboBox *>(QStringLiteral("instanceUpdateLoader"));
                    auto *mode = page->findChild<QComboBox *>(QStringLiteral("instanceUpdateMode"));
                    auto *start = page->findChild<QPushButton *>(QStringLiteral("instanceUpdateStart"));
                    auto *wizard = page->findChild<QWizard *>(QStringLiteral("instanceUpdateWizard"));
                    // 新页面接管之后，进「版本升级」**不再自动弹旧向导**：向导对象还在（兜底用），
                    // 但默认必须是隐藏的；开始按钮仍然因为没选版本而灰着。
                    upgradeFilled = game && loader && mode && start && wizard && !wizard->isVisible()
                        && game->currentIndex() == 0 && !start->isEnabled()
                        && !mode->currentData().toBool();
                    const QString preview = qEnvironmentVariable("WTFML_UPDATE_PREVIEW");
                    if (!preview.isEmpty()) {
                        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
                        wizard->setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
                        QApplication::processEvents(); wizard->grab().save(preview);
                    }
                }

                if (auto *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(migrationFound,
                  "★ T20.14 「迁移」页有一整块：目标下拉框 + 计划正文 + 范围列表 + 动手那颗");
            check(targetPicked,
                  "★★ T20.14 目标列表里是别的版本（不含自己）"
                  "（空 = 控制器没把 installedVersions 喂给下拉框）");
            check(planFilled,
                  "★★ T20.14 后台迁移计划在截止前生成完整正文",
                  migrationPlanText.left(160));
            check(checkboxDefaults,
                  "★★ T20.14 默认勾选落进了框里：存档勾着、日志不勾");
            check(applyGate,
                  "★ T20.14 动手那颗听勾选：一项不勾就灰、勾回来又亮");
            check(upgradeFilled,
                  "★ T20.14 更新页按游戏和加载器选择目标，默认创建副本，未选版本不能开始",
                  upgradePlanText.left(160));
        }

        // ---------- ⑧ 世代备份 + 写之前那几道闸----------
        //
        // 用户那份设计评审里 S 级的三条（撤销/世代备份、游戏在跑时挡写、写之前比对盘上
        // 那一份）**只有真落盘才验得到**：ui_check 的 U11f/U11g 量的是纯函数和那一格长什么
        // 样，它一个字节都不写盘。这里钉五件事：
        // ① 存一次 ⇒ 盘上**原来**那一份躺进备份里（"改坏了能回去"这个承诺的兑现处）；
        // ② 「还原上一次」取回来的东西**只进框、不进盘**，再点保存才落回去；
        // ③ 盘上那份在中间被别人改过 ⇒ 保存**先问一句**；答"读盘上那一份"时框里换成盘上
        // 那份、盘上不动，答"盖掉它"时**被盖掉的那一份也留进了备份**（连"盖掉"都
        // 不是不可逆的 —— 这正是这一整套东西的用处）；
        // ④ 这个实例的游戏**在跑**时保存被挡住（游戏退出会整份写回，改了也白改），而且
        // 挡住之后**框里的改动还在**（不许顺手丢掉），关掉游戏再存就下去了；
        // ⑤ 备份只留最近 20 份，多的自动清掉（那是全仓唯一一处会主动删东西的地方）。
        //
        // 会话是拿 `session-holder` 真起的（和 AH 段同一套：这个 exe 的一份替身，睡着不动）
        // —— 不用真 JVM，也不用碰启动那条路（那一趟要弹的框太多，和这一节没关系）。
        {
            const QString configDirV = dirV + QStringLiteral("/config");
            const QString jsonPath2 = configDirV + QStringLiteral("/sodium-options.json");
            // 盘上这一份的初值（和 ⑦ 那份区分开：这一节量的都是"存之前是什么"）。
            const QByteArray firstContent = QByteArrayLiteral("{\r\n  \"quality\": 1\r\n}\r\n");
            writeFileAt(jsonPath2, firstContent);
            // 备份根先清干净：这一节要**数文件夹个数**（第 ⑤ 条）。
            const QString backupRoot = AppPaths::configBackupDir() + QLatin1Char('/') + idV;
            QDir(backupRoot).removeRecursively();

            const auto backupFolders = [&backupRoot] {
                return QDir(backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            };
            // 备份里那个文件：路径是 `<备份根>/<实例id>/<时间戳>/<它相对 config/ 的路径>`。
            const auto backupBytes = [&backupRoot](const QString &folder) {
                return readBytesAt(backupRoot + QLatin1Char('/') + folder +
                                   QStringLiteral("/sodium-options.json"));
            };
            const auto anyBackupHolds = [&](const QByteArray &wanted) {
                for (const QString &folder : backupFolders()) {
                    if (backupBytes(folder) == wanted) {
                        return true;
                    }
                }
                return false;
            };

            bool hiddenBeforeAnySave = false;   // 没留过备份时不画那颗按钮
            bool shownAfterSave = false;        // 存完就画出来了
            QByteArray afterFirstSave;          // 第一次存之后盘上那一份
            bool firstBackupHoldsOriginal = false;  // 备份里躺着的是**存之前**那一份
            int foldersAfterFirstSave = -1;
            QString restoredText;               // 点「还原上一次」之后框里那串
            bool restoredDirty = false;
            QString restoredNote;
            QByteArray diskAfterRestoreClick;   // 点「还原」时盘上**必须没动**
            QByteArray diskAfterRestoreSave;    // 再点保存之后 == 存之前那一份
            bool restoreBackupMade = false;     // 而"刚才存的那一份"又留成备份了
            int stampPromptCount = 0;           // 闸②问了几句
            QString stampPromptTitle;
            QStringList stampPromptButtons;
            QString stampPromptNote;
            QByteArray diskAfterTakeDisk;       // 答「读盘上那一份」：盘上不动
            QString textAfterTakeDisk;          // 而框里换成了盘上那份
            QByteArray sneakyContent;           // 中间偷偷写下去那份
            bool overwroteDisk = false;         // 答「盖掉它」：盘上换成了我们的
            bool sneakyKeptInBackup = false;    // 而"被盖掉的那一份"留了备份
            int gamePromptCount = 0;            // 闸①弹了几句
            QString gamePromptTitle;
            bool dirtyAfterRefusal = false;     // 被挡住之后框里的改动还在
            QByteArray diskAfterRefusal;        // 被挡住时盘上一个字节没动
            QByteArray diskAfterGameStopped;    // 关掉游戏再存：下去了
            int foldersAfterManySaves = -1;     // 第 ⑤ 条：留 20 份
            QByteArray newestBackupBytes;       // 最新那一份里躺着的是**最后一次存之前**那份
            QByteArray expectedNewest;          // 期望"最新那一份"里是什么
            int collisionNames = 0;             // 同一秒里撞出来的备份夹名有几个
            bool allPadded = true;              // 撞出来的那些名字，序号一律三位
            QString badName;
            int unexpectedPrompts = 0;          // 不该弹话的那几趟真的没弹
            QString unexpectedTitle;

            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;  // 这一页没开起来：下面那几条会红，这里别崩
                }
                ConfigFileEditorPage *editor = page->configFilePage();
                if (editor == nullptr || editor->restoreButton() == nullptr) {
                    return;
                }
                page->showPage(QStringLiteral("config"));
                QApplication::processEvents();

                // 双击 config/ 底下那一行（和 ⑦ 同一手法：发给鼠标底下真正是谁，让它冒上去）。
                const auto openIt = [&] {
                    InstanceFilesPage *files = page->filesPage(QStringLiteral("config"));
                    if (files == nullptr || files->list() == nullptr) {
                        return;
                    }
                    files->list()->doItemsLayout();
                    for (int row = 0; row < files->list()->count(); ++row) {
                        QListWidgetItem *item = files->list()->item(row);
                        if (item == nullptr) {
                            continue;
                        }
                        const QString one = QFileInfo(
                            item->data(InstanceFilesPage::pathRole()).toString()).fileName();
                        if (one != QStringLiteral("sodium-options.json")) {
                            continue;
                        }
                        QWidget *target = files->list()->viewport();
                        const QRect rect = files->list()->visualItemRect(item);
                        const QPoint pos(rect.x() + 10, rect.center().y());
                        const auto one_event = [&target, &pos](QEvent::Type type,
                                                               Qt::MouseButtons buttons) {
                            QMouseEvent event(type, QPointF(pos),
                                              QPointF(target->mapToGlobal(pos)), Qt::LeftButton,
                                              buttons, Qt::NoModifier);
                            QApplication::sendEvent(target, &event);
                        };
                        one_event(QEvent::MouseButtonPress, Qt::LeftButton);
                        one_event(QEvent::MouseButtonRelease, Qt::NoButton);
                        one_event(QEvent::MouseButtonDblClick, Qt::LeftButton);
                        one_event(QEvent::MouseButtonRelease, Qt::NoButton);
                        QApplication::processEvents();
                        return;
                    }
                };
                // 点「保存」。@param answer 这一趟该点第几颗按钮；@param expected 这一趟
                // **该不该**弹出一句话来。
                //
                // NOTE: `expected` 不是装饰：不该弹却弹了就是产品的问题，得让它变成红的断言。
                // 没有这一位的话，每一趟都挂一个"随便点一颗"的定时器，产品哪一次多问了
                // 一句都会被它悄悄答掉，"没弹话"那几条断言就永远绿着。
                const auto save = [&](int answer, bool expected, int *promptCount,
                                      QString *promptTitle, QStringList *promptButtons,
                                      QString *promptNote) {
                    int count = 0;
                    QString title;
                    QStringList buttons;
                    QString note;
                    {
                        PromptAnswerer answerer({answer});
                        answerer.start();
                        editor->saveButton()->click();
                        // 该弹的那几趟是在这一发 click **里面**弹的（产品那条路嵌在嵌套事件
                        // 循环里），定时器早答完了；这一句只是兜住"把弹框排到事件队列里"那种
                        // 写法的余量。不该弹的那几趟只等很短一下就够 —— 不然连着存二十几次的
                        // 那一趟会白白等上几十秒。
                        pumpUntil([&answerer] { return !answerer.answered.isEmpty(); },
                                  expected ? 3000 : 150);
                        answerer.stop();
                        count = answerer.answered.size();
                        if (!answerer.answered.isEmpty()) {
                            title = answerer.answered.first().title;
                            buttons = answerer.answered.first().buttons;
                            note = answerer.answered.first().note;
                        }
                    }
                    if (!expected && count > 0) {
                        ++unexpectedPrompts;
                        unexpectedTitle = title;
                    }
                    if (promptCount != nullptr) {
                        *promptCount = count;
                    }
                    if (promptTitle != nullptr) {
                        *promptTitle = title;
                    }
                    if (promptButtons != nullptr) {
                        *promptButtons = buttons;
                    }
                    if (promptNote != nullptr) {
                        *promptNote = note;
                    }
                };

                // ---- ① 存一次：原来那一份进备份 ----
                openIt();
                hiddenBeforeAnySave = !editor->restoreButton()->isVisible();
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 2\n}\n"));
                save(0, false, nullptr, nullptr, nullptr, nullptr);
                afterFirstSave = readBytesAt(jsonPath2);
                shownAfterSave = editor->restoreButton()->isVisible();
                foldersAfterFirstSave = backupFolders().size();
                firstBackupHoldsOriginal = anyBackupHolds(firstContent);

                // ---- ② 「还原上一次」：只进框、不进盘 ----
                editor->restoreButton()->click();
                QApplication::processEvents();
                restoredText = editor->text();
                restoredDirty = editor->isDirty();
                restoredNote = editor->noteLabel()->text();
                diskAfterRestoreClick = readBytesAt(jsonPath2);
                save(0, false, nullptr, nullptr, nullptr, nullptr);
                diskAfterRestoreSave = readBytesAt(jsonPath2);
                restoreBackupMade = anyBackupHolds(afterFirstSave);

                // ---- ③ 闸②：盘上那份被别人改过 ----
                // 中间那一份**故意换个大小**（`extra` 那一条多出来的字节）：大小和修改时间
                // 两样一起比，但大小不一样就够判出来了 —— 同一毫秒里写完两趟也不怕。
                sneakyContent = QByteArrayLiteral("{\r\n  \"quality\": 42,\r\n"
                                                  "  \"extra\": true\r\n}\r\n");
                writeFileAt(jsonPath2, sneakyContent);
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 3\n}\n"));
                save(1, true, &stampPromptCount, &stampPromptTitle, &stampPromptButtons,
                     &stampPromptNote);
                diskAfterTakeDisk = readBytesAt(jsonPath2);
                textAfterTakeDisk = editor->text();

                // 再来一次同样的现场，这回答「盖掉它」。这一趟盘上换一份**长度不一样**的
                // （那一趟读盘的 stamp 就是上面这份的长度）：大小本身就够判出"被改过"了，
                // 不用赌"两次写的修改时间会不会落在同一毫秒里"。
                const QByteArray sneaky2Content = QByteArrayLiteral(
                    "{\r\n  \"quality\": 43,\r\n  \"extra\": true,\r\n  \"note\": \"x\"\r\n}\r\n");
                writeFileAt(jsonPath2, sneaky2Content);
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 4\n}\n"));
                save(0, true, nullptr, nullptr, nullptr, nullptr);
                overwroteDisk = readBytesAt(jsonPath2) ==
                                QByteArrayLiteral("{\r\n  \"quality\": 4\r\n}\r\n");
                sneakyKeptInBackup = anyBackupHolds(sneaky2Content);

                // ---- ④ 闸①：这个实例的游戏在跑 ----
                LauncherManager *manager = w.findChild<LauncherManager *>();
                int holder = 0;
                if (manager != nullptr) {
                    // 这个 exe 的一份替身（`session-holder` 那一支睡够四分钟，见 main() 开头）
                    // —— 和 AH1 同一套办法，只是这儿要的是"**这个实例**正在跑"。
                    EffectiveJavaSettings holderSettings;
                    holderSettings.javaPath = QCoreApplication::applicationFilePath();
                    holder = manager->startGame(QStringList({QStringLiteral("session-holder")}),
                                                holderSettings, idV, QStringLiteral("1.98.1"));
                    pumpUntil([manager] { return !manager->runningGames().isEmpty(); }, 5000);
                }
                editor->editor()->setPlainText(QStringLiteral("{\n  \"quality\": 5\n}\n"));
                save(0, true, &gamePromptCount, &gamePromptTitle, nullptr, nullptr);
                dirtyAfterRefusal = editor->isDirty();
                diskAfterRefusal = readBytesAt(jsonPath2);
                if (manager != nullptr && holder > 0) {
                    manager->stopGame(holder);
                    pumpUntil([manager] { return manager->runningGames().isEmpty(); }, 8000);
                }
                // 关掉游戏再存：这一下该下去了。
                save(0, false, nullptr, nullptr, nullptr, nullptr);
                diskAfterGameStopped = readBytesAt(jsonPath2);

                // ---- ⑤ 备份只留最近 20 份 ----
                // 连着存 24 次（每一次都留一份 ⇒ 一定会超过上限）。**同一秒里全是它**也是
                // 有意的：序号撞车那一档（补零）只有在这种跑法下才验得到。
                for (int i = 0; i < 24; ++i) {
                    editor->editor()->setPlainText(
                        QStringLiteral("{\n  \"quality\": %1\n}\n").arg(100 + i));
                    save(0, false, nullptr, nullptr, nullptr, nullptr);
                }
                expectedNewest = QByteArrayLiteral("{\r\n  \"quality\": 122\r\n}\r\n");
                foldersAfterManySaves = backupFolders().size();
                const QStringList left = backupFolders();
                QString newest = left.isEmpty() ? QString() : left.first();
                for (const QString &name : left) {
                    if (name > newest) {
                        newest = name;
                    }
                }
                newestBackupBytes = newest.isEmpty() ? QByteArray() : backupBytes(newest);
                for (const QString &name : left) {
                    if (name.size() == 15) {
                        continue;  // 没撞车：就是那个时间戳本身
                    }
                    ++collisionNames;
                    const QString tail = name.mid(16);
                    bool digits = tail.size() == 3;
                    for (const QChar one : tail) {
                        digits = digits && one.isDigit();
                    }
                    if (!digits || name.size() != 19 || name.at(15) != QLatin1Char('-')) {
                        allPadded = false;
                        badName = name;
                    }
                }

                // 历史版本：必须列出当前文件各代备份，而非只有最新一份。
                const QString configDir = QFileInfo(jsonPath2).absolutePath();
                const QStringList history = InstanceMaintenanceController::configBackups(idV, configDir, jsonPath2);
                check(history.size() == 20, "T20.10 历史版本只列当前文件的 20 代备份");
                check(InstanceMaintenanceController::configBackups(QStringLiteral("../escape"), configDir, jsonPath2).isEmpty(),
                      "T20.10 配置历史拒绝越界实例标识");
                if (history.size() > 1 && editor->historyButton() != nullptr) {
                    const QByteArray diskBeforeHistory = readBytesAt(jsonPath2);
                    bool listed = false;
                    QTimer::singleShot(0, [&listed, host, &history] {
                        auto *box = host->findChild<InlineMessageBox *>();
                        if (box != nullptr && box->listWidget() != nullptr) {
                            auto *list = box->listWidget();
                            listed = list->count() == history.size()
                                && list->item(1)->text().contains(QDir::toNativeSeparators(history.at(1)))
                                && list->item(1)->text().contains(QStringLiteral("字节"));
                            list->setCurrentRow(1);
                        }
                    });
                    {
                        PromptAnswerer answerer({0});
                        answerer.start();
                        editor->historyButton()->click();
                        answerer.stop();
                    }
                    check(listed && editor->text() == ConfigFileText::decode(readBytesAt(history.at(1))).text,
                          "T20.10 历史选择可载入非最新备份，列表含路径和大小");
                    check(readBytesAt(jsonPath2) == diskBeforeHistory && editor->isDirty(),
                          "T20.10 载入历史只改变编辑器，不覆盖磁盘");
                    const QString draft = QStringLiteral("unsaved draft");
                    editor->editor()->setPlainText(draft);
                    {
                        PromptAnswerer answerer({1});
                        answerer.start();
                        editor->historyButton()->click();
                        answerer.stop();
                    }
                    check(editor->text() == draft && readBytesAt(jsonPath2) == diskBeforeHistory,
                          "T20.10 取消历史选择保留草稿和磁盘");
                    {
                        PromptAnswerer answerer({0, 1});
                        answerer.start();
                        editor->historyButton()->click();
                        answerer.stop();
                    }
                    check(editor->text() == draft && readBytesAt(jsonPath2) == diskBeforeHistory,
                          "T20.10 拒绝丢弃草稿时不载入历史");
                    editor->discardChanges();
                }
                // 资源包：真实设置页切换模式，启停与优先级落到 options.txt，不再改名。
                const QString gameRoot = QFileInfo(configDir).absolutePath();
                const QString packRoot = gameRoot + QStringLiteral("/resourcepacks");
                const QString optionsPath = gameRoot + QStringLiteral("/options.txt");
                writeFileAt(packRoot + QStringLiteral("/a.zip"), "a");
                writeFileAt(packRoot + QStringLiteral("/b.zip"), "b");
                const QByteArray options = "unknown:keep\r\nresourcePacks:[\"vanilla\",\"file/a.zip\"]\r\n";
                writeFileAt(optionsPath, options);
                page->showPage(QStringLiteral("resourcepack"));
                auto *mode = page->findChild<QPushButton *>(QStringLiteral("resourcePackMode"));
                auto *packs = page->resourcePackPage();
                check(mode != nullptr && packs != nullptr, "T20.10 资源包启用顺序入口存在");
                if (mode != nullptr && packs != nullptr) {
                    mode->click();
                    auto *available = packs->findChild<QListWidget *>(QStringLiteral("resourcePackAvailable"));
                    auto *selected = packs->findChild<QListWidget *>(QStringLiteral("resourcePackSelected"));
                    auto *enable = packs->findChild<QPushButton *>(QStringLiteral("resourcePackEnable"));
                    auto *disable = packs->findChild<QPushButton *>(QStringLiteral("resourcePackDisable"));
                    auto *down = packs->findChild<QPushButton *>(QStringLiteral("resourcePackDown"));
                    int betaRow = -1;
                    for (int row = 0; row < available->count(); ++row)
                        if (available->item(row)->data(Qt::UserRole).toString() == QStringLiteral("file/b.zip")) betaRow = row;
                    check(packs->isVisible() && betaRow >= 0 && selected->count() == 2,
                          "T20.10 真实资源包页面分出可用和已启用列表");
                    available->setCurrentRow(betaRow);
                    enable->click();
                    check(ResourcePackOptions::parse(readBytesAt(optionsPath)).selected
                              == QStringList{"vanilla", "file/a.zip", "file/b.zip"},
                          "T20.10 真实启用按钮将资源包加入 options.txt");
                    down->click();
                    check(ResourcePackOptions::parse(readBytesAt(optionsPath)).selected
                              == QStringList{"vanilla", "file/b.zip", "file/a.zip"},
                          "T20.10 优先级按钮真正保存 options.txt 顺序");
                    check(readBytesAt(optionsPath).startsWith("unknown:keep\r\n")
                              && QFileInfo::exists(packRoot + QStringLiteral("/a.zip"))
                              && QFileInfo::exists(packRoot + QStringLiteral("/b.zip")),
                          "T20.10 资源包调整保留未知设置、原文件与文件名");
                    const QByteArray changed = readBytesAt(optionsPath) + "external:change\r\n";
                    writeFileAt(optionsPath, changed);
                    selected->setCurrentRow(0);
                    {
                        PromptAnswerer answerer({0});
                        answerer.start();
                        disable->click();
                        answerer.stop();
                    }
                    check(readBytesAt(optionsPath) == changed && packs->selectedIds().size() == 3,
                          "T20.10 外部修改触发冲突提示，原文件不被覆盖且界面回滚");
                    writeFileAt(packRoot + QStringLiteral("/new-download.zip"), "new");
                    page->refreshFilePages();
                    bool newFileVisible = false;
                    for (int row = 0; row < available->count(); ++row)
                        newFileVisible |= available->item(row)->data(Qt::UserRole).toString() == QStringLiteral("file/new-download.zip");
                    check(newFileVisible, "T20.10 文件变更刷新同步更新资源包双区，不必退出重开");
                }
                // 收尾：所有改动均已保存，顶上返回箭头直接把整页收掉。
                if (QPushButton *back =
                        host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                    QApplication::processEvents();
                }
                if (host->pageOpen()) {
                    host->closeOverlay();
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();

            check(hiddenBeforeAnySave,
                  "★ T20.10 还没留过备份的文件上**不画**「还原上一次」（点了也没东西可取）",
                  QStringLiteral("画没画=%1").arg(int(!hiddenBeforeAnySave)));
            check(afterFirstSave == QByteArrayLiteral("{\r\n  \"quality\": 2\r\n}\r\n"),
                  "T20.10 前提：第一次保存真的落到盘上了", QString::fromUtf8(afterFirstSave));
            check(shownAfterSave && firstBackupHoldsOriginal && foldersAfterFirstSave == 1,
                  "★★ T20.10 存一次 ⇒ 盘上**原来那一份**（`quality = 1`）躺进了备份里，而且"
                  "那颗按钮跟着画出来 —— 这是「改坏了能回去」唯一的兑现处",
                  QStringLiteral("画没画=%1 备份里有原来那份=%2 备份夹 %3 个")
                      .arg(int(shownAfterSave))
                      .arg(int(firstBackupHoldsOriginal))
                      .arg(foldersAfterFirstSave));
            check(restoredText == QStringLiteral("{\n  \"quality\": 1\n}\n"),
                  "★★ T20.10 「还原上一次」取回来的是**存之前**那一份（不是「打开时」那一份 ——"
                  "用户要回退的是他刚才那一次保存）",
                  restoredText);
            check(restoredDirty &&
                      restoredNote.startsWith(ConfigFileEditorPage::restoredPrefix()) &&
                      restoredNote.contains(QStringLiteral("保存")),
                  "★★ T20.10 取回来**只是摆进框里**：那一份是脏的、底下那行字说清它哪来的，"
                  "而且要用户按「保存」才写回盘上 —— 「还原」不许是一次悄悄的写入（不然用户"
                  "按一下就叫还原，而盘上那份已经被盖掉了，想回到「刚改的那一份」没有路）",
                  restoredNote);
            check(diskAfterRestoreClick == afterFirstSave,
                  "★★ T20.10 点「还原」那一下**盘上一个字节都没动**",
                  QString::fromUtf8(diskAfterRestoreClick));
            check(diskAfterRestoreSave == firstContent && restoreBackupMade,
                  "★★ T20.10 再点「保存」⇒ 盘上回到存之前那一份；而**刚才存的那一份**又留成了"
                  "备份 —— 于是「回去」和「再回来」两边都能走（只留一份的话，回退就成了一次"
                  "不可逆的跳崖）",
                  QStringLiteral("盘上=%1 那份留了备份=%2")
                      .arg(QString::fromUtf8(diskAfterRestoreSave))
                      .arg(int(restoreBackupMade)));
            check(stampPromptCount == 1 && stampPromptTitle == QStringLiteral("源文件已被修改") &&
                      stampPromptButtons.size() == 2,
                  "★★ T20.10 盘上那份在中间被别的东西改过 ⇒ 保存**先问一句**（不是静静盖掉）"
                  "—— 那一趟多半是游戏退出时写回，用户改了半天、被盖掉的时候屏幕上什么都不会"
                  "说，他只会以为「我改了没用」",
                  QStringLiteral("%1 句 / %2 / %3 颗按钮")
                      .arg(stampPromptCount)
                      .arg(stampPromptTitle)
                      .arg(stampPromptButtons.size()));
            check(stampPromptNote.contains(QStringLiteral("放弃当前修改")),
                  "★ T20.10 那一问的小字把「读盘上那一份会丢什么」说在前头（两颗按钮里那颗是"
                  "不可逆的，点之前得知道）",
                  stampPromptNote);
            check(diskAfterTakeDisk == sneakyContent &&
                      textAfterTakeDisk == QStringLiteral("{\n  \"quality\": 42,\n"
                                                          "  \"extra\": true\n}\n"),
                  "★★ T20.10 答「读盘上那一份」⇒ 框里换成盘上现在的内容，而**盘上不动**"
                  "（用户要的是「盘上那份」，不是「把我这份写下去」）",
                  textAfterTakeDisk);
            check(overwroteDisk && sneakyKeptInBackup,
                  "★★ T20.10 答「盖掉它」⇒ 我们的那份写下去了，而**被盖掉的那一份留在备份里**"
                  "—— 所以「盖掉」也不是不可逆的，这正是这一整套东西的用处",
                  QStringLiteral("盘上换了=%1 那份留了备份=%2")
                      .arg(int(overwroteDisk))
                      .arg(int(sneakyKeptInBackup)));
            check(gamePromptCount == 1 && gamePromptTitle == QStringLiteral("这个实例正开着游戏"),
                  "★★ T20.10 这个实例的游戏**在跑**时保存被挡住，而且说清了为什么（游戏手里"
                  "有一份内存副本，退出时会整份写回来）—— 判据和「游戏还在跑、窗口不许关」那一趟"
                  "是同一份（runningVersionIds()）",
                  QStringLiteral("%1 句 / %2").arg(gamePromptCount).arg(gamePromptTitle));
            check(dirtyAfterRefusal && diskAfterRefusal == QByteArrayLiteral(
                      "{\r\n  \"quality\": 4\r\n}\r\n"),
                  "★★ T20.10 被挡住之后**框里的改动还在、盘上也没动**（挡的是「这一次存」，"
                  "不是把用户刚敲的东西顺手丢掉）",
                  QStringLiteral("还脏=%1").arg(int(dirtyAfterRefusal)));
            check(diskAfterGameStopped == QByteArrayLiteral("{\r\n  \"quality\": 5\r\n}\r\n"),
                  "★★ T20.10 关掉游戏之后再点一次保存就下去了（不是「这个文件从此不给改」）",
                  QString::fromUtf8(diskAfterGameStopped));
            check(foldersAfterManySaves == 20,
                  "★★ T20.10 备份只留最近 20 份，多的自动清掉 —— 这是全仓**唯一一处会主动删"
                  "东西**的缓存目录（清的是我们自己生成的文件夹名，见 ConfigFileText 那一节）",
                  QStringLiteral("留了 %1 个").arg(foldersAfterManySaves));
            check(newestBackupBytes == expectedNewest,
                  "★★ T20.10 「最新那一份」真的是**最后那一次存之前**的内容（名字排序 == 时间"
                  "排序这条约定在**同一秒里连着存 24 次**时也得成立：序号不补零的话，`…-10` "
                  "会排到 `…-2` 前面，清理删掉的就是最新那份、留下的反而是最老的）",
                  QStringLiteral("最新那份里是 %1").arg(QString::fromUtf8(newestBackupBytes)));
            check(collisionNames > 0 && allPadded,
                  "★ T20.10 前提：这一节里**真的撞过名字**（同一秒里存了不止一次），而且撞出来的"
                  "序号一律补零到三位 —— 不补零的话「按名字排序 == 按时间排序」这条约定在两位"
                  "数上就断了（下面那条「最新那一份」量的正是这件事，这一条是它的前提）",
                  QStringLiteral("%1 个带序号，坏的是「%2」").arg(collisionNames).arg(badName));
            check(unexpectedPrompts == 0,
                  "★ T20.10 这一节里那几趟**不该弹话**的保存真的没弹一句（盘上那份一直是我们"
                  "自己写下去的那一份、那个实例也没在跑）—— 多弹一句就说明闸②的判据比它该有的"
                  "样子严",
                  QStringLiteral("%1 句，头一句是「%2」")
                      .arg(unexpectedPrompts)
                      .arg(unexpectedTitle));
        }

        // ---------- ⑪ 「显示哪些页」那份选择真落到 wtfml.meta 上----------
        //
        // 勾一下 → settingsEdited → MainWindow 把那一份选择写进**这个实例自己的**
        // wtfml.meta → 重开还在 → 再勾上 ⇒ 那个键**消失**（不是记一个 true）。
        //
        // 中间任何一段断了，界面上都看不出来：勾一下照样「当场生效」（U12 那一半还是绿的），
        // 只是关掉窗再进来又回来了 —— 而这种「我明明记得改过」的错觉，用户的下一个动作
        // 就是再勾一次，于是永远以为自己没点准。
        {
            const QString metaPath = AppPaths::versionMetaPath(idV);
            const auto readMeta = [&metaPath]() {
                QFile file(metaPath);
                if (!file.open(QIODevice::ReadOnly)) {
                    return QJsonObject();
                }
                return QJsonDocument::fromJson(file.readAll()).object();
            };
            // 开一次实例设置（`id`），在那个嵌套事件循环里跑 `act`，然后收页。
            //
            // NOTE: `openInstanceSettings()` 会**卡在嵌套事件循环里**，所以 `act` 必须写在
            // `QTimer::singleShot(0, …)` 里 —— 它就在那个循环里跑（和 T20.7 同一手法）。
            // 收页用 `closeOverlay()`：产品那颗返回箭头会多问一句「改过没保存」，那一趟
            // 在 T20.7 / T20.10 里。
            const auto withPage = [&w](const QString &id, auto &&act) {
                QTimer::singleShot(0, [&] {
                    OverlayHost *host = currentPageOverlay();
                    auto *page = host != nullptr
                                     ? host->findChild<InstanceSettingsDialog *>()
                                     : nullptr;
                    if (page != nullptr) {
                        act(page);
                    }
                    // 收页**走顶上那颗返回箭头**（它发 reject）—— 和其它"点返回"的用例
                    // 保持同一个形状。`closeOverlay()` 现在也收得干净（所有收页路都汇到
                    // 它、`pageClosed()` 从那一处发），但这一条要量的就是"用户按返回"
                    // 那一发。
                    if (host != nullptr) {
                        auto *back = host->findChild<QPushButton *>(
                            OverlayHost::overlayBackButtonName());
                        if (back != nullptr) {
                            back->click();
                            QApplication::processEvents();
                        } else {
                            host->dismiss();
                        }
                    }
                });
                QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                          Q_ARG(QString, id));
                QApplication::processEvents();
            };

            const QJsonObject metaBefore = readMeta();
            check(!metaBefore.contains(VersionManager::settingsPagesField()),
                  "T20.11 前提：这个实例的 wtfml.meta 上还没有那个键（默认 = 七条全显示 ⇒ "
                  "盘上什么都不写）",
                  QString::fromUtf8(QJsonDocument(metaBefore).toJson(QJsonDocument::Compact)));

            QStringList shownBefore;
            bool clicked = false;
            withPage(idV, [&](InstanceSettingsDialog *page) {
                shownBefore = page->shownEntryKeys();
                auto *box = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("save")));
                if (box != nullptr) {
                    clicked = true;
                    box->click();
                    QApplication::processEvents();
                }
            });
            check(clicked && shownBefore.contains(QStringLiteral("save")),
                  "T20.11 前提：进得去、那一列里本来有「存档」、也点得到那颗勾选框",
                  shownBefore.join(QLatin1Char(' ')));
            const QJsonObject metaAfter = readMeta();
            const QJsonObject pagesAfter =
                metaAfter.value(VersionManager::settingsPagesField()).toObject();
            check(pagesAfter.size() == 1 && pagesAfter.value(QStringLiteral("save")) == false,
                  "★★ T20.11 勾一下**当场落到盘上**：这个实例自己的 wtfml.meta 里出现 "
                  "settingsPages 那一格，并且里面记着 save=false —— 存下来的**只可能是 "
                  "false**（不可用的那几条勾不上），所以这一格是「藏起来的页」那个集合，"
                  "不是一份选择表",
                  QString::fromUtf8(QJsonDocument(metaAfter).toJson(QJsonDocument::Compact)));

            bool reopenUnchecked = false;
            bool reopenMissing = false;
            withPage(idV, [&](InstanceSettingsDialog *page) {
                auto *box = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("save")));
                reopenUnchecked = box != nullptr && !box->isChecked();
                reopenMissing = !page->shownEntryKeys().contains(QStringLiteral("save"));
            });
            check(reopenUnchecked && reopenMissing,
                  "★★ T20.11 重开那一页：那颗勾选框是**没勾**的、左栏里也没有「存档」—— "
                  "「个性化」这三个字真正落地就是这一趟（写盘那一段断了的话，这一条红、"
                  "而上面那条还是绿的）");

            bool reenableEnabled = false;
            bool reenableChecked = false;
            QString reenableChoices;
            withPage(idV, [&](InstanceSettingsDialog *page) {
                auto *box = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("save")));
                if (box != nullptr) {
                    // 一条真实会出事的路：那颗勾选框**灰着**的时候 `click()` 是个空操作
                    // （QAbstractButton::click() 头一句就是 `if (!isEnabled()) return;`），
                    // 于是"再勾回来"看起来什么都没发生、盘上那句 false 还在。
                    reenableEnabled = box->isEnabled();
                    box->click();
                    QApplication::processEvents();
                    reenableChecked = box->isChecked();
                }
                reenableChoices = QString::fromUtf8(
                    QJsonDocument(page->pageChoices()).toJson(QJsonDocument::Compact));
            });
            const QJsonObject metaRestored = readMeta();
            // 拿**第一次写入之后**那一份当基准，不是开跑前那一份：这个实例原本一个
            // `wtfml.meta` 都没有，第一次写盘会顺手补一个稳定 id（`ensureInstanceId()`，
            // 见 InstanceSettingsController::apply 开头），那是写盘顺带长的键、不是这一次
            // 操作丢了什么。真正要钉的是"`settingsPages` 整格消失、别的键一个不少"。
            QStringList keysWanted = metaAfter.keys();
            keysWanted.removeAll(VersionManager::settingsPagesField());
            QStringList keysAfter = metaRestored.keys();
            keysWanted.sort();
            keysAfter.sort();
            check(!metaRestored.contains(VersionManager::settingsPagesField()) &&
                      keysWanted == keysAfter,
                  "★★ T20.11 再勾回来 ⇒ 那个键**整个消失**（不是记一个 true），旁边那些键"
                  "一个不少 —— 盘上要是留一坨 true，默认永远是「显示」这件事就再也改不动"
                  "了：写 false 和不写长得一样，读侧分不出来",
                  QString::fromUtf8(
                      QJsonDocument(metaRestored).toJson(QJsonDocument::Compact))
                      + QStringLiteral("｜那颗框 enabled=%1 checked=%2 choices=%3")
                            .arg(int(reenableEnabled))
                            .arg(int(reenableChecked))
                            .arg(reenableChoices)
                      + QStringLiteral("｜第一次写后=%1")
                            .arg(QString::fromUtf8(
                                QJsonDocument(metaAfter).toJson(QJsonDocument::Compact))));

            // 第三件：**「有没有对应的加载器」这件事是按实例现算的**（两个假实例对着看）。
            //
            // 这一条是那一整张表的用处所在（要求：「如果没有对应的加载器，保持对应选项
            // 不可勾选的状态，悬停文本解释」）：同一个对话框类，光板那个实例上「模组 / 光影包
            // / 原理图」三条是灰的、各自说各自的原因；挂着 OptiFine 的那个实例上「模组」与
            // 「光影包」是亮的 —— OptiFine 自己就是模组加载器（1.13 起它本来就装在 mods/ 里），
            // 也自己认 shaderpacks/。
            QStringList vanillaOff;
            QStringList vanillaTips;
            withPage(idB, [&](InstanceSettingsDialog *page) {
                for (const QString &key : {QStringLiteral("mod"), QStringLiteral("shader"),
                                           QStringLiteral("schematic")}) {
                    auto *box = page->findChild<QCheckBox *>(
                        InstanceSettingsDialog::pageCheckName(key));
                    if (box == nullptr || box->isEnabled()) {
                        continue;
                    }
                    vanillaOff << key;
                    vanillaTips << box->toolTip();
                }
            });
            check(vanillaOff.size() == 3,
                  "★★ T20.11 光板那个实例（没有加载器、mods/ 底下一个 jar 都没有、也没装 "
                  "litematica）上「模组 / 光影包 / 原理图」三条都是灰的：不可勾选而不是藏起来，"
                  "用户得看得见「有这么一页、但这个实例上用不上」",
                  vanillaOff.join(QLatin1Char(' ')));
            check(vanillaTips.size() == 3 && vanillaTips.at(0) != vanillaTips.at(1) &&
                      vanillaTips.at(1) != vanillaTips.at(2) &&
                      vanillaTips.at(0) != vanillaTips.at(2),
                  "★★ T20.11 而且三句悬停文本**互不相同**（一句「用不上」套在三条上，症状是"
                  "「照着提示去装错东西」）",
                  vanillaTips.join(QStringLiteral(" | ")));

            // 「模组」那一条的判据是 **有加载器 _或_ mods/ 里已经有 jar**
            // （instanceModsUsable，第二半是为了那两个连 json 都解析不出唯一一份、却装着
            // 27 / 136 个模组的真实例）。所以 idN —— 它 mods/ 里躺着两个 jar —— 应该是
            // **亮的**：那一页在它上面点得开、看得到那两个 jar 并管得了（禁用/删除）。
            // 光板那条（上面 idB）钉的是"什么都没有就灰着"，这一条钉的是另一半。
            bool modOnForJars = false;
            withPage(idN, [&](InstanceSettingsDialog *page) {
                auto *box = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("mod")));
                modOnForJars = box != nullptr && box->isEnabled();
            });
            check(modOnForJars,
                  "★★ T20.11 mods/ 里已经有 jar 的实例上「模组」那一页是**亮的**（哪怕问不出"
                  "加载器）—— 判据见 instanceModsUsable 第二半：用户盘上有两个实例连唯一一份"
                  " json 都解析不出来，却分别装着 27 和 136 个模组，把那一页灰着正是用户"
                  "报的那个症状");
            // 而「光影包 / 原理图」在 idN 上照样是灰的：它那两个 jar 一个都不是光影加载器
            // （`GeckoLibIrisCompat` / `[钠] sodium`），也没有 litematica —— 判据放宽成
            // "mods/ 里有东西就算能用光影"，这两条会当场红。
            bool shaderOffForJars = true;
            bool schematicOffForJars = true;
            withPage(idN, [&](InstanceSettingsDialog *page) {
                auto *shaderBox = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("shader")));
                auto *schematicBox = page->findChild<QCheckBox *>(
                    InstanceSettingsDialog::pageCheckName(QStringLiteral("schematic")));
                shaderOffForJars = shaderBox != nullptr && !shaderBox->isEnabled();
                schematicOffForJars = schematicBox != nullptr && !schematicBox->isEnabled();
            });
            check(shaderOffForJars && schematicOffForJars,
                  "★★ T20.11 反过来：mods/ 里有 jar **不等于**能开光影包 / 原理图 —— "
                  "那两条另有判据（认得出 Iris/Oculus/OptiFine / 装没装 litematica）");

            QStringList optifineOn;
            withPage(idO, [&](InstanceSettingsDialog *page) {
                for (const QString &key : {QStringLiteral("mod"), QStringLiteral("shader")}) {
                    auto *box = page->findChild<QCheckBox *>(
                        InstanceSettingsDialog::pageCheckName(key));
                    if (box != nullptr && box->isEnabled()) {
                        optifineOn << key;
                    }
                }
            });
            check(optifineOn.size() == 2,
                  "★★ T20.11 挂着 OptiFine 的那个实例上「模组」与「光影包」都是亮的 —— "
                  "判据是 wtfml.meta 里的 loaderName，不是 mods/ 里有什么",
                  optifineOn.join(QLatin1Char(' ')));
        }

        // ---------- T20.11b 实例设置那两行「窗口模式 / 窗口大小」真落到 wtfml.meta 上 ----------
        //
        // 两头各写一遍键名、各写一遍代号表，界面那边照样全绿（U9b 是喂进去的假代号）。
        // 这一段断了的话，症状是"这一页上改完关掉，下次打开又回去了"，而屏幕上全程正常。
        {
            const QString metaPath = AppPaths::versionMetaPath(idV);
            const auto readMeta = [&metaPath]() {
                QFile file(metaPath);
                if (!file.open(QIODevice::ReadOnly)) {
                    return QJsonObject();
                }
                return QJsonDocument::fromJson(file.readAll()).object();
            };
            // 和 T20.11 那个 withPage 同一手法（`openInstanceSettings()` 是非阻塞的；
            // 动作排在 0 毫秒定时器里，收页走顶上那颗返回箭头 —— `closeOverlay()` 不经过
            // 页面自己的 reject，收页钩子那一套也不跑）。
            const auto withPage = [&w](const QString &id, auto &&act) {
                QTimer::singleShot(0, [&] {
                    OverlayHost *host = currentPageOverlay();
                    auto *page = host != nullptr
                                     ? host->findChild<InstanceSettingsDialog *>()
                                     : nullptr;
                    if (page != nullptr) {
                        act(page);
                    }
                    if (host != nullptr) {
                        auto *back = host->findChild<QPushButton *>(
                            OverlayHost::overlayBackButtonName());
                        if (back != nullptr) {
                            back->click();
                            QApplication::processEvents();
                        } else {
                            host->dismiss();
                        }
                    }
                });
                QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                          Q_ARG(QString, id));
                QApplication::processEvents();
            };
            // 那一格上的**一对单选钮换成了一个下拉框**（要求「所有单选框
            // 换成下拉的」），`customRadioName()` / `followRadioName()` 两个名字一起没了，
            // 改按 `choiceComboName()` 找、按**下标**选 —— 那两个下标是产品给的唯一出处
            // （`choiceFollowIndex()` / `choiceCustomIndex()`），别在这儿再写 0 / 1。
            //
            // NOTE: 这里走的是**用户那条路**：直接 `setCurrentIndex()`，**不挡信号**。产品里
            // `setChoiceCustom()` 灌初值时挡信号是为了"打开这一页别当成一次编辑"，那是
            // 另一回事；在这儿挡掉的话，"切档位这一下要写盘"那条线就等于没测（产品里
            // 接的是 `QComboBox::currentIndexChanged`，程序化设置**就是**用户改一次档位）。
            const auto setChoice = [](InstanceSettingsDialog *page, const QString &field,
                                      bool custom) {
                auto *combo = page != nullptr ? page->findChild<QComboBox *>(
                                                    InstanceSettingsDialog::choiceComboName(field))
                                              : nullptr;
                if (combo != nullptr) {
                    combo->setCurrentIndex(custom ? InstanceSettingsDialog::choiceCustomIndex()
                                                  : InstanceSettingsDialog::choiceFollowIndex());
                    QApplication::processEvents();
                }
                return combo != nullptr;
            };

            const QJsonObject winBefore = readMeta();
            check(!winBefore.contains(gameWindowModeMetaField()),
                  "T20.11b 前提：这个实例的 wtfml.meta 上还没有「窗口模式」那个键",
                  QString::fromUtf8(QJsonDocument(winBefore).toJson(QJsonDocument::Compact)));

            // ① 两行都切到「自定义」，档位选「全屏」、大小填 1600×900。
            bool comboHasThree = false;
            bool comboPicked = false;
            const QString modeField = InstanceSettingsDialog::windowModeField();
            const QString sizeField = InstanceSettingsDialog::windowWidthField();
            withPage(idV, [&](InstanceSettingsDialog *page) {
                // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：窗口模式 /
                // 窗口大小这两行在「启动」页上，先进这一页再动它们。
                page->showPage(InstanceSettingsDialog::launchCategoryKey());
                auto *combo = page->findChild<QComboBox *>(
                    InstanceSettingsDialog::windowModeComboName());
                comboHasThree = combo != nullptr && combo->count() == 3;
                setChoice(page, modeField, true);
                setChoice(page, sizeField, true);
                if (auto *wSpin = page->findChild<QSpinBox *>(
                        InstanceSettingsDialog::windowWidthSpinName())) {
                    wSpin->setValue(1600);
                }
                if (auto *hSpin = page->findChild<QSpinBox *>(
                        InstanceSettingsDialog::windowHeightSpinName())) {
                    hSpin->setValue(900);
                }
                QApplication::processEvents();
                // 换档位必须是这一页上**最后**一个动作：上面那几发（点单选钮、改数字框）
                // 各自都会触发一次写盘，而那一趟写的是**当时**的完整签名 —— 档位先换好的
                // 话，后面任何一发都会替它把盘写掉，于是"下拉框没接那条线"这个洞被盖住。
                // 实测过：档位放在前面时，把 connect 掐掉这套断言照样全绿。
                if (combo != nullptr) {
                    const int row =
                        combo->findData(gameWindowModeToken(GameWindowMode::Fullscreen));
                    if (row >= 0) {
                        combo->setCurrentIndex(row);
                        QApplication::processEvents();
                        comboPicked = true;
                    }
                }
            });
            check(comboHasThree && comboPicked,
                  "T20.11b 前提：那一页上找得到那个三档下拉框，也选得到「全屏」那一档");

            // 这个实例自己的 meta —— 不是全局那份设置（全局那一份在 AE 段量，两者互不影响）。
            const QJsonObject winAfter = readMeta();
            check(winAfter.value(gameWindowModeMetaField()).toString() ==
                      gameWindowModeToken(GameWindowMode::Fullscreen),
                  "★★ T20.11b 换个档位**当场落到这个实例的 wtfml.meta 上**"
                  "（下拉框不接那条线的话，盘上会是老那一档，而屏幕上写着「全屏」）",
                  QString::fromUtf8(QJsonDocument(winAfter).toJson(QJsonDocument::Compact)));
            check(winAfter.value(gameWindowWidthMetaField()).toInt() == 1600 &&
                      winAfter.value(gameWindowHeightMetaField()).toInt() == 900,
                  "★★ T20.11b 那两个数也一起写上去了",
                  QStringLiteral("%1×%2")
                      .arg(winAfter.value(gameWindowWidthMetaField()).toInt())
                      .arg(winAfter.value(gameWindowHeightMetaField()).toInt()));
            // 写的是**键名自己那一份**（不是 MainWindow 里另写一个串）—— 上面读的三个键名
            // 就是 src/settings/ 里那三个函数，读得到说明写侧用的是同一个。
            check(winAfter.contains(gameWindowModeMetaField()) &&
                      winAfter.contains(gameWindowWidthMetaField()) &&
                      winAfter.contains(gameWindowHeightMetaField()),
                  "T20.11b 三个键用的就是 gameWindow*MetaField() 给出的那三个名字");

            // ② 重开那一页：两行都停在「自定义」上、下拉框停在「全屏」、两个框是那两个数。
            bool reopenCustom = false;
            bool reopenPicked = false;
            bool reopenSize = false;
            withPage(idV, [&](InstanceSettingsDialog *page) {
                // 页是第一次切过去才建的，重开的这一扇也一样：先进「启动」那一页再读。
                page->showPage(InstanceSettingsDialog::launchCategoryKey());
                // 档位从单选框变成下拉框之后，"停在「自定义」上"还是同一件事，只是读法
                // 成了"那个下拉框停在下标 1"（`choiceCustomIndex()`）。
                auto *modeChoice = page->findChild<QComboBox *>(
                    InstanceSettingsDialog::choiceComboName(modeField));
                auto *sizeChoice = page->findChild<QComboBox *>(
                    InstanceSettingsDialog::choiceComboName(sizeField));
                reopenCustom = modeChoice != nullptr &&
                               modeChoice->currentIndex() ==
                                   InstanceSettingsDialog::choiceCustomIndex() &&
                               sizeChoice != nullptr &&
                               sizeChoice->currentIndex() ==
                                   InstanceSettingsDialog::choiceCustomIndex();
                auto *combo = page->findChild<QComboBox *>(
                    InstanceSettingsDialog::windowModeComboName());
                reopenPicked = combo != nullptr &&
                               combo->currentData().toString() ==
                                   gameWindowModeToken(GameWindowMode::Fullscreen);
                auto *wSpin = page->findChild<QSpinBox *>(
                    InstanceSettingsDialog::windowWidthSpinName());
                auto *hSpin = page->findChild<QSpinBox *>(
                    InstanceSettingsDialog::windowHeightSpinName());
                reopenSize = wSpin != nullptr && wSpin->value() == 1600 && hSpin != nullptr &&
                             hSpin->value() == 900;
            });
            check(reopenCustom,
                  "★★ T20.11b 重开那一页两条都停在「自定义」上（停在「跟随全局」的话用户会"
                  "以为设置丢了，而下一页动任何开关都会把它写没）");
            check(reopenPicked,
                  "★★ T20.11b 下拉框停在**存进去的那一档**上（代号从 meta 认回来又变成了索引）");
            check(reopenSize, "★★ T20.11b 两个数字框里是存进去的那两个数");

            // ③ 两行都切回「跟随全局」⇒ 那三个键**整个消失**（不是写一个等于全局的值进去）。
            // 留着一份"看起来跟全局一样"的副本，以后改全局这个实例就不跟着走了 ——
            // 而用户明明选的是「跟随全局」。
            withPage(idV, [&](InstanceSettingsDialog *page) {
                page->showPage(InstanceSettingsDialog::launchCategoryKey());
                setChoice(page, modeField, false);
                setChoice(page, sizeField, false);
                QApplication::processEvents();
            });
            const QJsonObject winRestored = readMeta();
            QStringList winKeysBefore = winBefore.keys();
            QStringList winKeysAfter = winRestored.keys();
            winKeysBefore.sort();
            winKeysAfter.sort();
            check(!winRestored.contains(gameWindowModeMetaField()) &&
                      !winRestored.contains(gameWindowWidthMetaField()) &&
                      !winRestored.contains(gameWindowHeightMetaField()),
                  "★★ T20.11b 切回「跟随全局」⇒ 那三个键整个消失（不是写一份等于全局的副本）",
                  QString::fromUtf8(
                      QJsonDocument(winRestored).toJson(QJsonDocument::Compact)));
            check(winKeysBefore == winKeysAfter,
                  "★★ T20.11b 而且旁边那些键一个不少（Java / 分组 / 页表都没被这一趟碰坏）",
                  winKeysBefore.join(QLatin1Char(' ')) + QStringLiteral(" | ") +
                      winKeysAfter.join(QLatin1Char(' ')));
        }

        // ---------- T20.12 顶上那颗「下载新模组」：按一下，人真的到下载页那一栏了 ----------
        //
        // 要求：「在实例设置里模组 资源包 数据包 光影包里添加下载新光影，
        // 按了以后导向下载页对应类别」。这一节走的就是那条路本身：开实例设置 → 在「模组」
        // 那一页顶上按那颗按钮 → 这一页关掉、选项卡翻到「下载」、左栏那一列站到「模组」上。
        {
            MainWindowUI *ui = w.ui();
            QTabWidget *tabs = ui != nullptr ? ui->getTabWidget() : nullptr;
            QListWidget *categories = ui != nullptr ? ui->getDownloadCategoryList() : nullptr;
            QStackedWidget *stack = ui != nullptr ? ui->getDownloadStack() : nullptr;
            const QString modKey = ResourceCatalog::kindKey(ResourceCatalog::Kind::Mod);
            int modRow = -1;
            for (int i = 0; categories != nullptr && i < categories->count(); ++i) {
                if (categories->item(i)->data(Qt::UserRole).toString() == modKey) {
                    modRow = i;
                }
            }
            check(tabs != nullptr && categories != nullptr && stack != nullptr && modRow >= 0,
                  "T20.12 前提：那个窗口上有选项卡、下载页左栏那一列、右栏那份堆叠，"
                  "而且「模组」那一栏在里面",
                  QStringLiteral("行 %1").arg(modRow));

            bool clicked = false;
            QString buttonText;
            // 和 T20.11 那个 withPage 同一手法（`openInstanceSettings()` 是非阻塞的，
            // 动作照旧排在 0 毫秒定时器里），但**收尾那一下不一样**：这一页是
            // 产品自己关的（那颗按钮的意思就是"换个地方"），所以不能照抄"再点一次返回箭头"
            // —— 那一下会落在一张已经收起来的页上。兜底只在"万一它还开着"的时候做。
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page =
                    host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;
                }
                // 跳页一律按**键**（左栏那一列的行号不等于键表下标，见 showPage() 那段）。
                page->showPage(QStringLiteral("mod"));
                InstanceFilesPage *files = page->filesPage(QStringLiteral("mod"));
                QPushButton *button = files != nullptr ? files->downloadButton() : nullptr;
                if (button == nullptr) {
                    return;
                }
                buttonText = button->text();
                clicked = true;
                button->click();
                QApplication::processEvents();
                if (host->pageOpen()) {
                    if (auto *back =
                            host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                        back->click();
                        QApplication::processEvents();
                    }
                }
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            // 切选项卡那一发是**排到下一轮事件循环**的（见 openDownloadCategory() 里那段
            // 这一句所在的调用栈没退干净之前不能翻页），所以这儿等它一下，
            // 而不是 `processEvents()` 完就立刻读。
            pumpUntil(
                [&] {
                    return tabs != nullptr &&
                           tabs->currentIndex() == MainWindowUI::downloadTabIndex();
                },
                5000);

            check(clicked &&
                      buttonText ==
                          InstanceSettingsDialog::filesDownloadText(QStringLiteral("mod")),
                  "★ T20.12 那个实例的「模组」那一页顶上有这颗按钮，也按得下去", buttonText);
            check(ui != nullptr && ui->overlay() != nullptr && !ui->overlay()->pageOpen(),
                  "★★ T20.12 按完**这一页自己关掉了** —— 用户要的是「换个地方」，"
                  "不是就地多出一个东西");
            check(tabs != nullptr && tabs->currentIndex() == MainWindowUI::downloadTabIndex(),
                  "★★ T20.12 选项卡翻到了「下载」那一页",
                  tabs != nullptr ? QString::number(tabs->currentIndex()) : QString());
            check(categories != nullptr && categories->currentRow() == modRow && stack != nullptr &&
                      stack->currentIndex() == modRow,
                  "★★ T20.12 左栏那一列站到了「模组」上、右栏跟着翻到了同一页"
                  "（认行是按条目自己的 UserRole，不是行号）",
                  QStringLiteral("行 %1 / 页 %2")
                      .arg(categories != nullptr ? categories->currentRow() : -1)
                      .arg(stack != nullptr ? stack->currentIndex() : -1));
        }

        // Favorites use launcher settings, never rewriting the game's list.
        {
            const QString address = QStringLiteral("127.0.0.1:1");
            const auto bytes = buildServersDat({qMakePair(QStringLiteral("Favorite fixture"), address),
                qMakePair(QStringLiteral("Other fixture"), QStringLiteral("127.0.0.1:2"))});
            const QString datPath = dirV + QStringLiteral("/servers.dat");
            writeFileAt(datPath, bytes);
            const auto modified = QFileInfo(datPath).lastModified();
            int visits = 0;
            for (int visit = 0; visit < 2; ++visit) {
                // 开页不阻塞了（`showPage()`）：先把页摆上来，再当场量。原来这里排一发
                // 0 毫秒定时器等实例设置那层嵌套循环来派 —— 现在没有那层循环，定时器要等
                // 外面泵事件才跑，而这个循环两趟都不泵 ⇒ `visits` 永远 0。
                QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection, Q_ARG(QString, idV));
                {
                    auto *host = w.ui()->overlay();
                    // 取最后一个：上一扇页可能还在等 deleteLater，刚开的这扇是最后加进来的子控件。
                    const auto pages = host->findChildren<InstanceSettingsDialog *>();
                    auto *page = pages.isEmpty() ? nullptr : pages.last();
                    // 页是第一次切过去才建的（见 InstanceSettingsDialog::ensurePage()）：
                    // 服务器的表格、那几颗按钮都在那一页上，先照用户的走法切过去。
                    // 列表是开窗那一刻就 `loadServerList()` 进来的（页还没建），切过去时
                    // `replayServerPage()` 把它摆上 —— 下面数的就是那两行。
                    if (page != nullptr) {
                        page->showPage(InstanceSettingsDialog::serverPageKey());
                    }
                    if (page && page->serverTree() != nullptr &&
                        page->serverTree()->topLevelItemCount() == 2 &&
                        page->findChild<QPushButton *>(QStringLiteral("serverFavorite")) &&
                        page->findChild<QCheckBox *>(QStringLiteral("serverFavoritesOnly"))) {
                        ++visits;
                        auto *tree = page->serverTree();
                        auto *favorite = page->findChild<QPushButton *>(QStringLiteral("serverFavorite"));
                        auto *only = page->findChild<QCheckBox *>(QStringLiteral("serverFavoritesOnly"));
                        auto *first = tree->topLevelItem(0);
                        auto *second = tree->topLevelItem(1);
                        tree->setCurrentItem(first, 0, QItemSelectionModel::ClearAndSelect);
                        if (visit == 0) {
                            favorite->click();
                            // 三个条件**分开钉**。合成一条时 CI 上只知道"这组挂了"，
                            // 分不清是按钮没翻、行文本没改，还是没落盘 —— 2026-11-03 那两次
                            // 失败（本地全绿）就卡在这个盲区里。落盘那一条允许排队
                            // （`pumpUntil`）：写盘本身是同步的，但事件队列里可能有前一批
                            // 信号还没跑完。
                            //
                            // 时间预算 5 秒而不是 500 ms：2026-11-04 全量串行跑时这一条在
                            // 机器同时有别的长任务的情况下失败过一次，单跑整套 `wtfml.main-window`
                            // 又全绿 —— 说明是"排队多久"的问题，不是"写没写"的问题。等 5 秒
                            // 不会把真失败变成通过（真失败永远等不到），只会让重负载下不抖。
                            check(favorite->isChecked(), "★ T20 favorite button flips after click");
                            check(first->text(0).startsWith(QStringLiteral("★ ")),
                                  "★ T20 favorite row shows the star", first->text(0));
                            const bool persisted = pumpUntil(
                                [&address] { return LauncherSettings().isServerFavorite(address); },
                                5000);
                            check(persisted,
                                  "server favorite is acknowledged and persisted by real window wiring",
                                  QStringLiteral("等了 5 秒仍是「没收藏」：%1").arg(address));
                            tree->setCurrentItem(second, 0, QItemSelectionModel::ClearAndSelect);
                            only->setChecked(true);
                            check(!first->isHidden() && second->isHidden() && tree->currentItem() == nullptr &&
                                  !page->serverJoinButton()->isEnabled(), "favorite filter clears hidden current selection and disables join");
                            QList<InstanceSettingsDialog::ServerRowStatus> statuses(2);
                            statuses[0].tested = true; statuses[0].online = true; statuses[0].latencyMs = 11;
                            statuses[1].tested = true; statuses[1].online = true; statuses[1].latencyMs = 22;
                            page->setServerStatuses(statuses);
                            check(first->text(2).contains(QStringLiteral("11 ms")) && second->text(2).contains(QStringLiteral("22 ms")) &&
                                  tree->topLevelItem(0) == first && tree->topLevelItem(1) == second,
                                  "favorite filtering preserves ping result indices and row identity");
                        } else {
                            check(favorite->isChecked(), "reopened instance page restores server favorite");
                            only->setChecked(true);
                            favorite->click();
                            check(!favorite->isChecked() && !favorite->isEnabled() && first->isHidden() &&
                                  !page->serverNotice()->isHidden() && !LauncherSettings().isServerFavorite(address),
                                  "unfavorite under active filter clears selection and shows empty-favorites notice");
                            only->setChecked(false);
                            check(!first->isHidden() && !second->isHidden(), "clearing favorite filter restores all servers");
                        }
                    }
                    if (auto *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) back->click();
                }
                // 收起来那一份是 `deleteLater()` 收的：让它落地，下一趟开页时才不会
                // `findChild` 摸到已经收起来的旧那一份。
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            check(visits == 2, "server favorites real page exercised twice");
            QFile dat(datPath);
            check(dat.open(QIODevice::ReadOnly) && dat.readAll() == bytes && QFileInfo(datPath).lastModified() == modified,
                  "favorite operations leave servers.dat bytes and timestamp untouched");
            LauncherSettings().setServerFavorite(address, false);
        }

        // ---------- 文件页：实例被标记「文件操作中」时，真实 MainWindow 接线也要拒绝 ----------
        {
            const QString guarded = AppPaths::versionDir(idV) + QStringLiteral("/mods/gate.jar");
            writeFileAt(guarded, QByteArrayLiteral("KEEP"));
            w.markFileOperationBusyForTest(idV);
            QString why;
            check(!w.toggleInstanceFileForTest(guarded, &why)
                      && why.contains(QStringLiteral("占用")),
                  "★★ T20.10b 实例在「忙」名单上时，真实主窗口拒绝启用 / 禁用文件", why);
            check(QFileInfo::exists(guarded)
                      && !QFileInfo::exists(guarded + QStringLiteral(".disabled")),
                  "★★★ T20.10b 拒绝之后文件一个字节都没动");
            w.releaseFileOperationBusyForTest(idV);
            why.clear();
            check(w.toggleInstanceFileForTest(guarded, &why) && why.isEmpty(),
                  "★ T20.10b 摘下忙碌标记后同一文件可以正常改动", why);
            check(QFileInfo::exists(guarded + QStringLiteral(".disabled")),
                  "★ T20.10b 正常路径确实改成了 .disabled");
            QFile::remove(guarded + QStringLiteral(".disabled"));
        }

        // ---------- T20.13 截图页批量删除：确认框写出文件名，删完保住滚动与选择 ----------
        //
        // 这一节走真实产品那条路（实例设置 → 截图 / 回放 → 工具条「删除」→ 确认框 →
        // 移入回收站 → 重列）：确认框里那几句话是 `InstanceDialogController` 拼的，只测
        // 对话框那一层（ui_check）碰不到它。回收站只碰临时数据根里的假截图。
        {
            const QString shotDir = AppPaths::versionDir(idB) + QStringLiteral("/screenshots");
            QDir(shotDir).removeRecursively();
            check(QDir().mkpath(shotDir), "T20.13 截图夹具目录建出来了");
            QStringList shotNames;
            for (int index = 1; index <= 25; ++index) {
                const QString name =
                    QStringLiteral("shot-%1.png").arg(index, 2, 10, QLatin1Char('0'));
                writeFileAt(shotDir + QLatin1Char('/') + name, QByteArrayLiteral("fake-png"));
                shotNames.append(name);
            }
            // 搜索过滤要挡掉的那一份：名字里不带 `shot`，搜 `shot-` 时只有它被挡住
            // （筛选是"只留匹配项"，按整条路径匹配）。
            const QString filteredName = QStringLiteral("suppressed.png");
            writeFileAt(shotDir + QLatin1Char('/') + filteredName, QByteArrayLiteral("fake-png"));
            const QString replayDir = AppPaths::replayRecordingsDir();
            QDir(replayDir).removeRecursively();
            check(QDir().mkpath(replayDir), "T20.13 回放夹具目录建出来了");
            writeFileAt(replayDir + QStringLiteral("/replay-test.mcpr"), QByteArrayLiteral("fake-mcpr"));

            QString replayText;      // 回放页那一问的正文
            QString singleText;      // 只选 1 份那一问的正文
            QStringList sevenLines;  // 选 7 份那一问的正文，按行拆开
            QString promptTitle;
            QStringList promptButtons;
            bool singleKept = false;
            bool sevenKept = false;
            bool deletedGone = false;
            bool deleteLeftNothingSelected = false;
            bool failedKept = false;
            bool scrollKept = false;
            bool filterKept = false;
            bool actionsReenabled = false;
            bool scrollable = false;
            int scrollBefore = 0;
            int barValue = -1;
            int barMax = -1;
            QString fixtureProblem;

            // 一条脚本答完这一节会弹的全部提示：三个确认框各点「取消」，后两个确认框
            // 点「删除」、随后那句结果提示点「确定」。
            PromptAnswerer answerer({1, 1, 1, 0, 0, 0, 0});
            answerer.start();
            QTimer::singleShot(0, [&] {
                OverlayHost *host = currentPageOverlay();
                // 取最后一个：上一扇页可能还在等 `deleteLater()`，刚开的这扇是最后加进来的子控件。
                const auto pages =
                    host != nullptr ? host->findChildren<InstanceSettingsDialog *>()
                                    : QList<InstanceSettingsDialog *>();
                auto *page = pages.isEmpty() ? nullptr : pages.last();
                if (page == nullptr) {
                    fixtureProblem = QStringLiteral("实例设置页没开起来");
                    return;
                }
                const auto buttonFor = [page](const QString &name, const QString &key) {
                    return page->findChild<QPushButton *>(
                        QStringLiteral("instanceLifecycleBatch%1_%2").arg(name, key));
                };
                // 行号只当"当下这一份列表"的坐标用，按文件名现找：列表按修改时间倒序排，
                // 重列之后同一份文件的行号就变了，旧行号一律不能用（`item()` 越界是空）。
                const auto rowOf = [](QListWidget *list, const QString &name) {
                    for (int row = 0; list != nullptr && row < list->count(); ++row) {
                        auto *item = list->item(row);
                        if (item != nullptr
                            && QFileInfo(item->data(Qt::UserRole).toString()).fileName() == name)
                            return row;
                    }
                    return -1;
                };
                const auto selectOnly = [&](QListWidget *list, const QStringList &wanted) {
                    if (list == nullptr) return;
                    list->clearSelection();
                    for (const QString &name : wanted) {
                        const int row = rowOf(list, name);
                        if (row >= 0) list->item(row)->setSelected(true);
                    }
                    QApplication::processEvents();
                };

                // ① 回放页：前面那句「目录共用」照旧，后面写文件名。
                page->showPage(QStringLiteral("replay"));
                QApplication::processEvents();
                auto *replayList = page->findChild<QListWidget *>(
                    QStringLiteral("instanceLifecycleFiles_replay"));
                auto *replayRemove = buttonFor(QStringLiteral("Remove"), QStringLiteral("replay"));
                if (replayList == nullptr || replayRemove == nullptr
                    || rowOf(replayList, QStringLiteral("replay-test.mcpr")) < 0) {
                    fixtureProblem = QStringLiteral("回放页没列出测试录像");
                } else {
                    selectOnly(replayList, {QStringLiteral("replay-test.mcpr")});
                    replayRemove->click();  // 确认框 → 取消
                }

                // ② 截图页：只选一份。
                page->showPage(QStringLiteral("screenshots"));
                QApplication::processEvents();
                auto *list = page->findChild<QListWidget *>(
                    QStringLiteral("instanceLifecycleFiles_screenshots"));
                // 这一页刚摆上来，QListView 的延后排版还没轮到：不排这一下，行高和滚动范围
                // 都还是 0（量"滚动位置"的断言会量到 0 上）。
                if (list != nullptr) list->doItemsLayout();
                QApplication::processEvents();
                auto *remove = buttonFor(QStringLiteral("Remove"), QStringLiteral("screenshots"));
                auto *search = page->findChild<QLineEdit *>(
                    QStringLiteral("instanceLifecycleSearch_screenshots"));
                if (list == nullptr || remove == nullptr) {
                    fixtureProblem = QStringLiteral("截图页的列表或「删除」不见了");
                    return;
                }
                selectOnly(list, {shotNames.value(0)});
                remove->click();  // 确认框 → 取消
                singleKept = list->selectedItems().size() == 1;

                // ③ 七份：列出前 5 个，剩下 2 个收成一行。
                selectOnly(list, shotNames.mid(0, 7));
                remove->click();  // 确认框 → 取消
                sevenKept = list->selectedItems().size() == 7;

                // ④ 真的删一份：滚动位置要回来，删掉的那一行不能顺手去选别人。
                if (search != nullptr) {
                    search->setText(QStringLiteral("shot-"));  // 只挡住 suppressed.png（目录名 screenshots 里没有这个连字符）
                    QApplication::processEvents();
                }
                selectOnly(list, {shotNames.value(1)});
                QScrollBar *bar = list->verticalScrollBar();
                // QListWidget 默认按**行**滚（`ScrollPerItem`），这个数是行号不是像素。
                scrollable = bar != nullptr && bar->maximum() >= 4;
                if (scrollable) {
                    bar->setValue(bar->maximum() - 2);
                    scrollBefore = bar->value();
                }
                scrollable = scrollable && scrollBefore > 0 && scrollBefore < bar->maximum();
                remove->click();  // 确认 → 删除，随后那句结果提示 → 确定
                QApplication::processEvents();
                deletedGone = rowOf(list, shotNames.value(1)) < 0
                              && !QFileInfo::exists(shotDir + QLatin1Char('/') + shotNames.value(1));
                deleteLeftNothingSelected = list->selectedItems().isEmpty();
                if (bar != nullptr) {
                    barValue = bar->value();
                    barMax = bar->maximum();
                }
                scrollKept = scrollable && bar->maximum() >= scrollBefore
                             && bar->value() == scrollBefore;
                const int hiddenRow = rowOf(list, filteredName);
                filterKept = search != nullptr && search->text() == QStringLiteral("shot-")
                             && hiddenRow >= 0 && list->item(hiddenRow)->isHidden();

                // ⑤ 删不动的那一份：仍然在盘上、而且仍然选着。
                selectOnly(list, {shotNames.value(2)});  // shot-03
#ifdef Q_OS_WIN
                const QString held = shotDir + QLatin1Char('/') + shotNames.value(2);
                const HANDLE lock = CreateFileW(
                    reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(held).utf16()),
                    GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (lock == INVALID_HANDLE_VALUE) {
                    fixtureProblem = QStringLiteral("占不住那份截图，删除失败的分支没演到");
                } else {
                    const int beforeFailure = bar != nullptr ? bar->value() : 0;
                    remove->click();  // 确认 → 删除（失败），结果提示 → 确定
                    QApplication::processEvents();
                    const int keptRow = rowOf(list, shotNames.value(2));
                    failedKept = keptRow >= 0 && QFileInfo::exists(held)
                                 && list->selectedItems().size() == 1
                                 && list->selectedItems().first() == list->item(keptRow);
                    scrollKept = scrollKept && bar != nullptr
                                 && bar->maximum() >= beforeFailure
                                 && bar->value() == beforeFailure;
                    CloseHandle(lock);
                }
#else
                // 回收站那一套是 Win32 的；别的平台上这一档（"删不动"）造不出来。
                failedKept = true;
#endif
                actionsReenabled = remove->isEnabled();

                // 提示都答完了，把这一节量到的东西取出来。
                replayText = answerer.answered.value(0).text;
                promptTitle = answerer.answered.value(1).title;
                promptButtons = answerer.answered.value(1).buttons;
                singleText = answerer.answered.value(1).text;
                sevenLines = answerer.answered.value(2).text.split(QLatin1Char('\n'));

                if (auto *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName()))
                    back->click();
            });
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idB));
            QApplication::processEvents();
            answerer.stop();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

            check(replayText
                      == QStringLiteral("回放目录由当前游戏目录内的实例共用。\n\n"
                                        "将「replay-test.mcpr」移入回收站？"),
                  "★★ T20.13 回放页那一问：前面那句「目录共用」照旧，后面写出文件名", replayText);
            check(promptTitle == QStringLiteral("删除文件")
                      && promptButtons == QStringList{QStringLiteral("删除"), QStringLiteral("取消")},
                  "★ T20.13 删除这一问的标题和两颗按钮没变",
                  promptTitle + QStringLiteral(" | ") + promptButtons.join(QLatin1Char('/')));
            check(singleText == QStringLiteral("将「shot-01.png」移入回收站？"),
                  "★★ T20.13 只选 1 个文件时确认框里写出文件名（不是「目录里 1 个文件」）",
                  singleText);
            const bool firstLineOk =
                sevenLines.value(0) == QStringLiteral("将以下 7 个文件移入回收站？");
            QStringList listed;
            for (int index = 2; index < 7 && index < sevenLines.size(); ++index)
                listed.append(sevenLines.at(index));
            QStringList uniqueListed = listed;
            uniqueListed.removeDuplicates();
            bool listedOk = sevenLines.size() == 8 && sevenLines.value(1).isEmpty()
                            && listed.size() == 5 && uniqueListed.size() == 5
                            && sevenLines.value(7) == QStringLiteral("另有 2 个文件");
            for (const QString &name : listed)
                listedOk = listedOk && shotNames.contains(name) && name.endsWith(QStringLiteral(".png"));
            check(firstLineOk && listedOk,
                  "★★ T20.13 选 7 个文件时列 5 个、末尾一句「另有 2 个文件」",
                  sevenLines.join(QLatin1Char('|')));
            check(singleKept && sevenKept,
                  "★★ T20.13 确认框上点「取消」之后原来的选择还在（旧写法这一下就清空了）",
                  QStringLiteral("1 个：%1，7 个：%2").arg(singleKept).arg(sevenKept));
            check(deletedGone && deleteLeftNothingSelected,
                  "★★ T20.13 删掉的那一行没了，而且没有顺手去选别的行");
            check(scrollKept,
                  "★★ T20.13 删完之后竖向滚动条回到原来的位置（列表短了就夹到最大值）",
                  QStringLiteral("之前 %1，之后 %2 / 最大 %3")
                      .arg(scrollBefore).arg(barValue).arg(barMax));
            check(failedKept,
                  "★★ T20.13 移入回收站失败的那一项还在盘上、而且仍然选着");
            check(filterKept, "★ T20.13 重列之后搜索框和它挡掉的那几行照旧");
            check(actionsReenabled, "★ T20.13 一次请求收尾之后工具条又点得动了");
            if (!fixtureProblem.isEmpty()) check(false, "T20.13 夹具没跑起来", fixtureProblem);

            QDir(shotDir).removeRecursively();
            QFile::remove(replayDir + QStringLiteral("/replay-test.mcpr"));
        }
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T20.9 这一节加的账号清干净了");

    LauncherSettings().setLastLaunchedVersion(lastVersionBefore);
    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T20.9 数据根目录换回来了");
}

