#include "Support.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include "tasks/TaskHistory.hpp"
#include "manage/LaunchHistory.hpp"
#include <atomic>
#include <vector>
#include <QProcess>
#include <QThread>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // ZJ6：拿一个 `FileShare = 0` 的句柄当「游戏还开着」（QFile 做不到）

void runCopyInstanceAndWorldTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir sourceRoot;
    QTemporaryDir destRoot;
    QTemporaryDir scratch;
    check(sourceRoot.isValid() && destRoot.isValid() && scratch.isValid(),
          "ZJ0 三个临时目录建出来了");

    AppPaths::setMinecraftDir(sourceRoot.path());
    {
        QString problem;
        const QString missing = scratch.filePath("missing-copy-source.bin");
        const QString target = scratch.filePath("missing-copy-target.bin");
        check(!copyTreeWithoutFollowingLinks(missing, target, {}, &problem) &&
                  problem.contains(missing) && !QFileInfo::exists(target),
              "复制失败保留具体文件首因，不发布空目标", problem);
    }
    {
        const QString source = scratch.filePath(QStringLiteral("short-source.bin"));
        const QString target = scratch.filePath(QString(220, QLatin1Char('x')) + QStringLiteral(".bin"));
        writeFileAt(source, QByteArrayLiteral("LONG-NAME"));
        check(copyTreeWithoutFollowingLinks(source, target),
              "长文件名复制：临时名不追加到目标全名，避免超过单文件名长度上限");
        QFile copied(target);
        check(copied.open(QIODevice::ReadOnly) && copied.readAll() == "LONG-NAME",
              "长文件名复制后的内容完整");
    }

    // Use real processes: an in-process QLockFile cannot exercise stale PID recovery.
    {
        const QString source = scratch.filePath(QStringLiteral("space-world"));
        const QString saves = scratch.filePath(QStringLiteral("space-saves"));
        QDir().mkpath(source);
        QDir().mkpath(saves);
        writeFileAt(source + "/level.dat", "world-data");
        writeFileAt(source + "/.hidden", "hidden-data");
        QString problem;
        check(!WorldFiles::copyWorldInto(source, saves, "low-space", &problem, {}, 0) &&
                  problem.contains(QStringLiteral("空间不足")),
              "ZJ7 世界导入低空间预算在复制前拒绝", problem);
        check(QDir(saves).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty() &&
                  QFileInfo::exists(source + "/level.dat"),
              "ZJ7 空间拒绝不留暂存与半份世界且保留源存档");
        check(!WorldFiles::copyWorldInto(source, saves, "exact-short", &problem, {},
                                       32 * 1024 * 1024 + 19),
              "ZJ7 空间计算包含隐藏文件和安全余量", problem);
        check(WorldFiles::copyWorldInto(source, saves, "enough-space", &problem, {}, 64 * 1024 * 1024) &&
                  QFileInfo::exists(saves + "/enough-space/.hidden"),
              "ZJ7 空间充足时正常导入并保留隐藏文件", problem);
    }
    {
        QProcess owner;
        owner.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--directory-lock-probe"), scratch.path(), QStringLiteral("--hold")});
        const bool started = owner.waitForStarted(5000)
            && (owner.bytesAvailable() > 0 || owner.waitForReadyRead(5000))
            && owner.readAllStandardOutput().contains("LOCKED");
        check(started, "ZJ0 独立进程取得数据目录锁");
        auto probe = [](const QString &path) {
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(),
                        {QStringLiteral("--directory-lock-probe"), path});
            const bool done = child.waitForStarted(5000) && child.waitForFinished(5000);
            if (!done) { child.kill(); child.waitForFinished(5000); }
            return done && child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
        };
        if (started) {
            check(probe(scratch.path() + "/./") == 73,
                  "ZJ0 第二进程通过路径别名访问同目录也安全拒绝");
            check(probe(destRoot.path()) == 0, "ZJ0 不同数据目录允许独立进程取得锁");
        }
        owner.kill(); owner.waitForFinished(5000);
        check(probe(scratch.path()) == 0, "ZJ0 持锁进程异常结束后可重新取得锁");
    }

    section("ZJ1. looksLikeWorld：凭什么认一个目录是「世界」");
    {
        const QString world = scratch.path() + QStringLiteral("/有leveldat");
        QDir().mkpath(world + QStringLiteral("/region"));
        writeFileAt(world + QStringLiteral("/level.dat"), "nbt");
        check(WorldFiles::looksLikeWorld(world), "ZJ1 有 level.dat 就算世界");

        const QString bare = scratch.path() + QStringLiteral("/没leveldat");
        QDir().mkpath(bare + QStringLiteral("/region"));
        check(!WorldFiles::looksLikeWorld(bare),
              "ZJ1 没 level.dat 的不算（region/ 空着不算数：刚建好的世界 region 就是空的）");

        // 这一条是"用户挑错了"那一档里最容易漏的：他把 `level.dat` **这个文件**选进来了
        // （原生选择框在 Windows 上是可以选文件的）。判据只看"这个路径下面有没有
        // level.dat"，`<...>/level.dat/level.dat` 当然不存在 ⇒ 判 false，正是要的结果。
        check(!WorldFiles::looksLikeWorld(world + QStringLiteral("/level.dat")),
              "ZJ1 传进来一个文件（level.dat 本身）也不算世界");
    }

    section("ZJ2. nameProblem：世界目录名和实例目录名同一套规矩");
    check(WorldFiles::nameProblem(QStringLiteral("我的世界")).isEmpty(),
          "ZJ2 普通中文名字过");
    check(!WorldFiles::nameProblem(QStringLiteral("a/b")).isEmpty(), "ZJ2 带分隔符不过");
    check(!WorldFiles::nameProblem(QStringLiteral("a\\b")).isEmpty(), "ZJ2 带反斜杠不过");
    check(!WorldFiles::nameProblem(QString()).isEmpty(), "ZJ2 空名字不过");
    check(!WorldFiles::nameProblem(QStringLiteral("CON")).isEmpty(),
          "ZJ2 Windows 保留设备名不过（建不出目录来）");
    check(!WorldFiles::nameProblem(QStringLiteral("以点结尾.")).isEmpty(), "ZJ2 尾随点不过");
    check(!WorldFiles::nameProblem(QString(65, QLatin1Char('x'))).isEmpty(), "ZJ2 超过 64 个字符不过");
    // 复用 InstanceFiles::validateDirName() 的**证据**：两边对同一个输入给出同一句话
    // ——"同一套规矩"这句话只有这么钉才不是一句空话。
    check(WorldFiles::nameProblem(QStringLiteral("CON")) ==
              InstanceFiles::validateDirName(QStringLiteral("CON")),
          "ZJ2 和 InstanceFiles::validateDirName 逐字同源（不是各写一套）");

    section("ZJ3. copyWorldInto：把一个世界拷进 saves/");
    const QString saves = scratch.path() + QStringLiteral("/saves");
    const QString worldSrc = scratch.path() + QStringLiteral("/外面的世界");
    QDir().mkpath(saves);
    QDir().mkpath(worldSrc + QStringLiteral("/region"));
    writeFileAt(worldSrc + QStringLiteral("/level.dat"), "level-bytes");
    writeFileAt(worldSrc + QStringLiteral("/region/r.0.mca"), "region-bytes");
    writeFileAt(worldSrc + QStringLiteral("/.hidden"), "hidden-bytes");
    {
        using namespace DirectorySessionLock;
        const auto key = keyFor(saves);
        const bool wasEnabled = enabled;
        enabled = true;
        QString problem;
        check(!acquire(QString(), &problem) && !problem.isEmpty(),
              "ZJ3 空目录不得被隐式转换为当前目录锁");
        check(acquire(saves, &problem) && acquire(saves, &problem),
              "ZJ3 同进程重复取得目录锁是幂等的", problem);
        check(problem.isEmpty(), "ZJ3 取得锁成功清除之前的错误信息");
        {
            // 必须是 QThread，不能换回 std::thread：非 Qt 线程里调 QtCore，线程退出时
            // Qt6Core+0x255fbc 会去解一个空的 QThreadData TLS（0xC0000005，WER 落盘但进程
            // 不终止）。实测同一段代码 std::thread 2400 个线程落 10 份转储、QThread 0 份。
            std::atomic<int> acquired{0};
            std::vector<QThread *> workers;
            const QString concurrentRoot = scratch.path() + QStringLiteral("/concurrent-lock");
            for (int i = 0; i < 8; ++i) workers.push_back(QThread::create([&] {
                if (acquire(concurrentRoot)) ++acquired;
            }));
            for (auto *worker : workers) worker->start();
            for (auto *worker : workers) { worker->wait(); delete worker; }
            check(acquired == 8 && held.contains(keyFor(concurrentRoot)),
                  "ZJ3 并发取得同目录锁不误报自身冲突且保留到退出");
            held.remove(keyFor(concurrentRoot));
        }
        QLockFile contender(QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock").arg(key)));
        contender.setStaleLockTime(0);
        check(!contender.tryLock(0), "ZJ3 已持有的目录锁拒绝第二个锁持有者");
        // 陈旧锁必须能被接管：崩溃 / 强杀 / 断电之后，上一次留下的那个文件不能让这个
        // 数据目录永久锁死（以前 `setStaleLockTime(0)` 就是这个效果 —— Qt 在 0 时**永不**
        // 判陈旧，实测一台机器上积了 6 个死 PID 的锁）。
        // 这里手写一份"PID 早就不在了 + 文件是两小时前的"锁，再取同一个目录。
        {
            const QString staleRoot = scratch.path() + QStringLiteral("/陈旧锁");
            QDir().mkpath(staleRoot);
            const QString staleKey = keyFor(staleRoot);
            const QString stalePath =
                QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock").arg(staleKey));
            QFile stale(stalePath);
            if (stale.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
                // QLockFile 的格式：第一行 PID、第二行主机名、第三行应用名。
                stale.write(QByteArray("999999\n") + qgetenv("COMPUTERNAME") + "\n"
                            + QByteArray("WTFMinecraftLauncher.exe\n"));
                stale.flush();
                // NOTE: `setFileTime()` 要求文件**还开着**（QFile 的约定），关掉再调会静默
                // 失败 —— 那样 mtime 还是"刚刚"，锁自然不算陈旧，这条断言就会以一个
                // 假失败的形式冤枉被测代码。
                stale.setFileTime(QDateTime::currentDateTime().addSecs(-7200),
                                  QFileDevice::FileModificationTime);
                stale.close();
            }
            QString staleProblem;
            const bool reclaimed = acquire(staleRoot, &staleProblem);
            check(reclaimed, "★ ZJ3 死 PID + 两小时前的陈旧锁能被接管（不再永久锁死）",
                  staleProblem);
            held.remove(staleKey);
            QFile::remove(stalePath);
        }
        check(keyFor(saves + QStringLiteral("/../saves/")) == key,
              "ZJ3 路径别名不能绕过目录锁");
        // 大小写与盘符写法：Windows 上这就是同一个目录，别名不能变成第二把锁。
        // （canonicalFilePath 解不解得掉大小写都不影响这一条：解不掉时 keyFor 自己
        // 还会 toCaseFolded()。）
        check(keyFor(saves.toUpper()) == key,
              "★ ZJ3 全大写写法（盘符 + 路径）也是同一把锁",
              keyFor(saves.toUpper()));
        // junction：一根指着数据目录的目录联接。`canonicalFilePath()` **不**解 junction
        // （见 ZJ5 的前置事实），keyFor 靠 InstanceFiles::canonicalPath() 里那一轮
        // resolveJunctions() 才认得出"这是同一条路径"—— 否则从链接那头进来的第二份
        // 启动器会以为目录没人占。
        const QString lockAliasHost = scratch.path() + QStringLiteral("/锁别名宿主");
        QDir().mkpath(lockAliasHost);
        const QString lockAlias = lockAliasHost + QStringLiteral("/指到存档");
        const bool madeLockAlias =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(lockAlias),
                               QDir::toNativeSeparators(saves)}) == 0
            && QFileInfo(lockAlias).isJunction();
        check(madeLockAlias, "★ ZJ3 先造出一根真 junction 指着数据目录（mklink /J）");
        if (madeLockAlias) {
            check(keyFor(lockAlias) == key,
                  "★★ ZJ3 经 junction 走进来的同一条路径还是同一把锁（别名绕不过去）",
                  keyFor(lockAlias));
            // 链接本体必须用 rmdir 删：scratch 的 removeRecursively() 会**跟着**链接
            // 往下删，那会把 saves 里刚造的世界也删掉（见 ZJ5 那条 NOTE）。
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(lockAlias)});
        }
        held.remove(key);
        check(contender.tryLock(0), "ZJ3 释放后目录锁可重新取得");
        enabled = wasEnabled;
    }
    {
        QString error;
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("取消导入"), &error,
                                        [] { return true; }), "ZJ3 取消导入拒绝落盘");
        check(!QFileInfo::exists(saves + QStringLiteral("/取消导入")),
              "ZJ3 取消后没有正式世界半成品");
        int polls = 0;
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("中途取消"), &error,
                                        [&] { return ++polls >= 4; }), "ZJ3 拷贝途中可取消");
        check(!QFileInfo::exists(saves + QStringLiteral("/中途取消")) &&
              QDir(saves).entryList({QStringLiteral(".wtfml-world-import-*")},
                  QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
              "ZJ3 中途取消清理暂存且不发布");
    }
    {
        QString error;
        check(WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("搬进来的"), &error),
              "ZJ3 正常拷一份成功", error);
        const QString dest = saves + QStringLiteral("/搬进来的");
        check(existsAt(dest + QStringLiteral("/level.dat")) &&
                  existsAt(dest + QStringLiteral("/region/r.0.mca")),
              "ZJ3 level.dat 和 region 里的东西都过去了");
        // 隐藏文件（带隐藏属性位、或者名字以点开头）漏掉的症状是"世界能开，但少东西"
        // —— 而用户看不出少的是哪一个。这一位在删树那头也要，拷这一头同样要。
        check(existsAt(dest + QStringLiteral("/.hidden")), "ZJ3 隐藏文件也过去了（一个不落）");
        QFile f(dest + QStringLiteral("/region/r.0.mca"));
        QByteArray got;
        if (f.open(QIODevice::ReadOnly)) {
            got = f.readAll();
        }
        check(got == QByteArray("region-bytes"), "ZJ3 拷过去的字节逐字节一致");
        check(WorldFiles::looksLikeWorld(dest), "ZJ3 拷出来的那一份自己也是一个世界");
    }
    {
        // 重名：**绝不能**合并。这里连"原来那份一个字节都没动"一起钉住 —— 挡住重名的
        // 意义就在这儿，只判"返回了 false"的话，把旧内容覆盖掉也算通过。
        writeFileAt(saves + QStringLiteral("/搬进来的/其他文件.txt"), "老内容");
        QString error;
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("搬进来的"), &error),
              "ZJ3 目标重名就拒绝");
        check(error.contains(QStringLiteral("已经有一个叫")), "ZJ3 说清了是被重名挡住", error);
        QFile f(saves + QStringLiteral("/搬进来的/其他文件.txt"));
        QByteArray got;
        if (f.open(QIODevice::ReadOnly)) {
            got = f.readAll();
        }
        check(got == QByteArray("老内容"), "ZJ3 被挡住时原来那一份一个字节都没动");
    }
    {
        // 名字不合法 / 源不是世界 / saves 不存在：三条都在**碰盘之前**挡掉。
        QString error;
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("a/b"), &error),
              "ZJ3 名字带分隔符拒绝");
        check(!error.isEmpty(), "ZJ3 且给了一句原因", error);
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QString(), &error), "ZJ3 空名字拒绝");

        const QString notWorld = scratch.path() + QStringLiteral("/不是世界");
        QDir().mkpath(notWorld);
        check(!WorldFiles::copyWorldInto(notWorld, saves, QStringLiteral("试试"), &error),
              "ZJ3 源里没有 level.dat 拒绝（拿整个 .minecraft 当世界拷进来是最常见的点错）");
        check(error.contains(QStringLiteral("level.dat")), "ZJ3 原因里点名了 level.dat", error);
        check(!existsAt(saves + QStringLiteral("/试试")), "ZJ3 而且没在 saves 里留下一个空壳");

        const QString noSaves = scratch.path() + QStringLiteral("/根本没有saves");
        check(!WorldFiles::copyWorldInto(worldSrc, noSaves, QStringLiteral("试试"), &error),
              "ZJ3 目的地 saves 不存在也拒绝（不替他造一个）");
        check(!existsAt(noSaves), "ZJ3 而且没把那个 saves 建出来");
        check(!WorldFiles::copyWorldInto(saves + QStringLiteral("/搬进来的/level.dat"), saves,
                                         QStringLiteral("试试"), &error),
              "ZJ3 源传进来一个文件也拒绝");
    }

    section("ZJ4. renameWorld：改一个世界的目录名");
    {
        const QString dir = scratch.path() + QStringLiteral("/改名靶子");
        QDir().mkpath(dir + QStringLiteral("/region"));
        writeFileAt(dir + QStringLiteral("/level.dat"), "level-bytes");
        QString error;
        check(WorldFiles::renameWorld(dir, QStringLiteral("改过的"), &error), "ZJ4 改名成功", error);
        check(!existsAt(dir), "ZJ4 老目录不在了");
        check(existsAt(scratch.path() + QStringLiteral("/改过的/level.dat")),
              "ZJ4 新目录在、里面的东西也跟着过去了");
    }
    {
        const QString a = scratch.path() + QStringLiteral("/甲");
        const QString b = scratch.path() + QStringLiteral("/乙");
        QDir().mkpath(a);
        QDir().mkpath(b);
        writeFileAt(a + QStringLiteral("/level.dat"), "a");
        writeFileAt(b + QStringLiteral("/level.dat"), "b");
        QString error;
        check(!WorldFiles::renameWorld(a, QStringLiteral("乙"), &error), "ZJ4 重名拒绝");
        check(error.contains(QStringLiteral("已经有一个叫")), "ZJ4 原因说清了重名", error);
        check(existsAt(a + QStringLiteral("/level.dat")), "ZJ4 被挡住时老目录原样还在");
        QFile f(b + QStringLiteral("/level.dat"));
        QByteArray got;
        if (f.open(QIODevice::ReadOnly)) {
            got = f.readAll();
        }
        check(got == QByteArray("b"), "ZJ4 被挡住时同名的那一份也没被动过");

        // 名字逐字一样 = 用户没改（那一格回车了一遍）。**算成功、不动盘**，不是报错。
        check(WorldFiles::renameWorld(a, QStringLiteral("甲"), &error),
              "ZJ4 名字没变算成功（那不是「重名」，是没改）", error);
        check(existsAt(a), "ZJ4 而且那个目录还在原地");
        check(!WorldFiles::renameWorld(a, QStringLiteral("a/b"), &error),
              "ZJ4 不合法的名字拒绝");
        check(!WorldFiles::renameWorld(scratch.path() + QStringLiteral("/没有这个"), QStringLiteral("x"),
                                       &error),
              "ZJ4 源已经不在盘上时拒绝");
    }
    {
        // 只差大小写：Windows 的文件系统不区分大小写，`Foo` → `foo` 改的是**磁盘上那个
        // 名字的显示大小写**，是合法操作。判据里那一句「恰好等于现名的那一项不参与重名
        // 比较」就是为它留的 —— 少了那一句，这里会被自己挡住。
        const QString dir = scratch.path() + QStringLiteral("/CaseOnly");
        QDir().mkpath(dir);
        writeFileAt(dir + QStringLiteral("/level.dat"), "x");
        QString error;
        check(WorldFiles::renameWorld(dir, QStringLiteral("caseonly"), &error),
              "ZJ4 只差大小写的改名是合法的", error);
        check(QDir(scratch.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                  .contains(QStringLiteral("caseonly")),
              "ZJ4 盘上那个名字的大小写真的变了",
              QDir(scratch.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).join(QLatin1Char(',')));
    }

    section("ZJ6. 文件被别的进程占着（共享冲突）：照实失败，不留半截、不动旧的那一份");
    {
        // ★ 「文件被占用」那条话术（见 docs/features/DIRECTORY_SESSION_LOCK.md）背后得有这一条：
        // 别的程序拿着 `level.dat` 且**不共享删改**时，改名 / 拷贝必须失败且旧的那一份原样在，
        // 而不是让 Windows 抛在一半、留下一个名字已经改掉、内容却只挪了一半的目录。
        //
        // Qt 的 `QFile` 打不开这种锁（它自带共享标志），所以这一条借 Win32 的 `CreateFileW`
        // 拿一个 `FileShare = 0` 的句柄 —— 这就是"游戏还开着"在本进程里的等价物。
        const QString occRoot = scratch.filePath(QStringLiteral("占用夹具"));
        QDir().mkpath(occRoot);
        const QString worldDir = occRoot + QStringLiteral("/占着的世界");
        QDir().mkpath(worldDir);
        writeFileAt(worldDir + QStringLiteral("/level.dat"), QByteArrayLiteral("level-bytes"));
        const QString savesDir = occRoot + QStringLiteral("/saves");
        QDir().mkpath(savesDir);

        const QString heldPath = worldDir + QStringLiteral("/level.dat");
        const HANDLE held =
            CreateFileW(reinterpret_cast<LPCWSTR>(heldPath.utf16()), GENERIC_READ | GENERIC_WRITE,
                        /*dwShareMode=*/0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE,
              "★ ZJ6 前提：用 Win32 拿住 `level.dat`（不共享读写删）");
        if (held != INVALID_HANDLE_VALUE) {
            QString error;
            check(!WorldFiles::renameWorld(worldDir, QStringLiteral("改过的"), &error) &&
                      !error.isEmpty(),
                  "★★ ZJ6 被占着时改名失败（不是假装成功）", error);
            check(QFileInfo::exists(heldPath) && !QFileInfo::exists(occRoot + QStringLiteral("/改过的")),
                  "★★★ ZJ6 世界还在老名字下、也没有多出半截的「改过的」");

            // 拷贝那一头：源世界里有被占着的文件 ⇒ 整趟失败，saves 里不留半成品。
            check(!WorldFiles::copyWorldInto(worldDir, savesDir, QStringLiteral("搬进来的"), &error) &&
                      !error.isEmpty(),
                  "★★ ZJ6 有文件被占着时导入世界也失败并说清楚", error);
            check(!QFileInfo::exists(savesDir + QStringLiteral("/搬进来的")),
                  "★★★ ZJ6 失败之后 saves 里没有半截的世界");
            check(QDir(savesDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                      .isEmpty(),
                  "★★ ZJ6 本次的暂存目录也收拾干净了（一个条目都不剩）",
                  QDir(savesDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                      .join(QLatin1Char(',')));

            CloseHandle(held);
        }
        // 放开之后同一趟必须成：证明挡住它的确实是那把共享锁，不是别的什么。
        QString error;
        check(WorldFiles::renameWorld(worldDir, QStringLiteral("改过的"), &error),
              "★ ZJ6 放开之后改名成功（挡住的确实是共享锁）", error);
        check(WorldFiles::copyWorldInto(occRoot + QStringLiteral("/改过的"), savesDir,
                                        QStringLiteral("搬进来的"), &error),
              "★ ZJ6 放开之后导入世界也成", error);
        check(QFileInfo::exists(savesDir + QStringLiteral("/搬进来的/level.dat")),
              "ZJ6 搬进来的那一份连 level.dat 都在");
    }

    section("ZJ7. 目标目录没有写权限（ACL 拒绝 WD/AD）：导入照实失败，不留半截");
    {
        // 和 ZJ6 是同一族「操作做不成时不许留半截」的另一类原因：不是有人占着，而是那一格
        // 根本不许写（`icacls /deny …:(WD,AD)` —— 拒绝项压过允许项，管理员身份也绕不过）。
        // `QFile::setPermissions()` 表达不出 ACL，所以借 icacls。
        const QString user = qEnvironmentVariable("USERNAME");
        const auto icacls = [](const QStringList &args) {
            return QProcess::execute(QStringLiteral("cmd"),
                                     QStringList{QStringLiteral("/c"), QStringLiteral("icacls")} +
                                         args) == 0;
        };
        const QString aclRoot = scratch.filePath(QStringLiteral("权限夹具"));
        const QString worldDir = aclRoot + QStringLiteral("/好好的世界");
        const QString savesDir = aclRoot + QStringLiteral("/saves");
        QDir().mkpath(worldDir);
        QDir().mkpath(savesDir);
        writeFileAt(worldDir + QStringLiteral("/level.dat"), QByteArrayLiteral("level-bytes"));
        const QString nativeSaves = QDir::toNativeSeparators(savesDir);
        const bool denied = !user.isEmpty() &&
                            icacls({nativeSaves, QStringLiteral("/deny"),
                                    user + QStringLiteral(":(WD,AD)")});
        check(denied, "★ ZJ7 前提：给 `saves/` 挂了拒绝写/追加的 ACL", user);
        if (denied) {
            // 前提：这一条拒绝项真的挡住了"在它里面造东西"（造出来了就顺手拆掉，
            // 别让下面"一个条目都不剩"那条验的变成自己造的东西）。
            const QString probeDir = savesDir + QStringLiteral("/前提探针");
            const bool mkdirBlocked = !QDir().mkpath(probeDir);
            if (!mkdirBlocked) {
                QDir().rmdir(probeDir);
            }
            check(mkdirBlocked, "★ ZJ7 前提：拒绝项真的挡得住往 `saves/` 里造目录");

            QString error;
            const bool imported =
                WorldFiles::copyWorldInto(worldDir, savesDir, QStringLiteral("搬进来的"), &error);
            check(!imported && !error.isEmpty(),
                  "★★ ZJ7 没有写权限时导入世界失败，而且说得出一句话", error);
            check(!QFileInfo::exists(savesDir + QStringLiteral("/搬进来的")),
                  "★★★ ZJ7 失败之后 `saves/` 里没有半截的世界");
            check(QDir(savesDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                      .isEmpty(),
                  "★★★ ZJ7 暂存也没留下（一个条目都不剩）",
                  QDir(savesDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                      .join(QLatin1Char(',')));
            check(readBytesAt(worldDir + QStringLiteral("/level.dat")) ==
                      QByteArrayLiteral("level-bytes"),
                  "★★ ZJ7 源世界一个字节都没被动过（失败那一趟只读源）");

            const bool restored = icacls({nativeSaves, QStringLiteral("/remove:d"), user});
            check(restored, "★ ZJ7 收尾：拒绝项摘掉了");
            QString after;
            check(restored && WorldFiles::copyWorldInto(worldDir, savesDir,
                                                        QStringLiteral("搬进来的"), &after),
                  "★ ZJ7 权限放开之后同一趟照常成功（挡住的确实是权限）", after);
            check(QFileInfo::exists(savesDir + QStringLiteral("/搬进来的/level.dat")),
                  "ZJ7 搬进来的那一份连 level.dat 都在");
        }
        icacls({nativeSaves, QStringLiteral("/remove:d"), user});  // 兜底：别把拒绝项留给下一轮
    }

    section("ZJ8. 源里那一件读不了（ACL 拒绝 RD）：失败要说得出是哪个文件、卡在哪一步");
    {
        // 和 ZJ7 配成一对：ZJ7 是"目标不让写"，这里是"源不让读"。复制失败的首因现在从
        // `copyTreeWithoutFollowingLinks()` 的 `problemOut` 一路带到界面话术里 —— P0 要的是
        // 「可检测的 I/O 失败要能查清」，不是只回一句「中断了」。
        const QString user = qEnvironmentVariable("USERNAME");
        const auto icacls = [](const QStringList &args) {
            return QProcess::execute(QStringLiteral("cmd"),
                                     QStringList{QStringLiteral("/c"), QStringLiteral("icacls")} +
                                         args) == 0;
        };
        const QString root = scratch.filePath(QStringLiteral("读权限夹具"));
        const QString worldDir = root + QStringLiteral("/读不了的世界");
        const QString savesDir = root + QStringLiteral("/saves");
        QDir().mkpath(worldDir);
        QDir().mkpath(savesDir);
        writeFileAt(worldDir + QStringLiteral("/level.dat"), QByteArrayLiteral("level-bytes"));
        const QString blocked = worldDir + QStringLiteral("/读不了.dat");
        writeFileAt(blocked, QByteArrayLiteral("秘密"));
        const QString nativeBlocked = QDir::toNativeSeparators(blocked);
        const bool denied = !user.isEmpty() &&
                            icacls({nativeBlocked, QStringLiteral("/deny"),
                                    user + QStringLiteral(":(RD)")});
        check(denied, "★ ZJ8 前提：给源里那一件挂了拒绝读的 ACL（icacls /deny RD）", user);
        if (denied) {
            QFile probe(blocked);
            const bool unreadable = !probe.open(QIODevice::ReadOnly);
            if (!unreadable) {
                probe.close();
            }
            check(unreadable, "★ ZJ8 前提：那一条拒绝项真的挡得住读");
            QString error;
            const bool imported = WorldFiles::copyWorldInto(worldDir, savesDir,
                                                            QStringLiteral("搬进来的"), &error);
            // 两句话都认：整文件那条路给的是「文件复制失败」，分块那条路给的是
            // 「无法打开源或临时文件」；这里钉的是"说得出哪一件、卡在哪一步"，
            // 不是把某一条内部实现路径焊死。
            check(!imported && error.contains(QStringLiteral("原因：")) &&
                      error.contains(QStringLiteral("读不了.dat")) &&
                      (error.contains(QStringLiteral("文件复制失败")) ||
                       error.contains(QStringLiteral("无法打开源或临时文件"))),
                  "★★ ZJ8 复制失败的原因说到「哪一步、哪一件」（不是只说中断了）", error);
            check(!QFileInfo::exists(savesDir + QStringLiteral("/搬进来的")) &&
                      QDir(savesDir)
                          .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                          .isEmpty(),
                  "★★ ZJ8 失败后不发布、暂存也收拾干净了",
                  QDir(savesDir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
                      .join(QLatin1Char(',')));
            const bool restored = icacls({nativeBlocked, QStringLiteral("/remove:d"), user});
            check(restored, "★ ZJ8 收尾：拒绝项摘掉了");
            check(restored && readBytesAt(blocked) == QByteArrayLiteral("秘密"),
                  "★★ ZJ8 源那一件一个字节都没被动过（失败那一趟只读源）");
            QString after;
            check(restored && WorldFiles::copyWorldInto(worldDir, savesDir,
                                                        QStringLiteral("搬进来的"), &after),
                  "★ ZJ8 权限放开之后同一趟照常成功", after);
            check(QFileInfo::exists(savesDir + QStringLiteral("/搬进来的/读不了.dat")),
                  "ZJ8 搬进来的那一份连刚才被拒绝的那件都在");
        }
        icacls({nativeBlocked, QStringLiteral("/remove:d"), user});  // 兜底
    }

    section("ZJ5. VersionManager::copyInstance：多出一份实例");
    AppPaths::setMinecraftDir(sourceRoot.path());
    const QString srcId = QStringLiteral("源实例");
    const QString srcDir = makeInstanceDir(sourceRoot.path() + QStringLiteral("/versions"), srcId,
                                           R"({"name":"源实例","instanceId":"source-uuid-1111","loaderName":"Fabric"})");
    const QString destRootPath = destRoot.path();
    check(WriteDirectoryGuard::rootFor(sourceRoot.path() + QStringLiteral("/versions/Servers/saves/world"))
              == InstanceFiles::canonicalPath(sourceRoot.path()),
          "ZJ5 实例名与 Servers 同名时仍使用已选数据根锁");
    VersionManager vm;
    {
        const bool enabledBefore = DirectorySessionLock::enabled;
        DirectorySessionLock::enabled = true;
        QLockFile other(QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock")
            .arg(DirectorySessionLock::keyFor(sourceRoot.path()))));
        other.setStaleLockTime(0);
        check(other.tryLock(0), "ZJ5 写入口占用测试取得源数据根锁");
        QString error;
        const QString lockedLedger = sourceRoot.path() + QStringLiteral("/locked-ledger.json");
        check(!Tasks::save(lockedLedger, {}) && !LaunchHistory::save(lockedLedger, {})
                  && !QFileInfo::exists(lockedLedger),
              "ZJ5 任务与启动历史账本拒绝占用目录且不创建文件");
        check(!vm.copyInstance(srcId, destRootPath, QStringLiteral("locked-copy"), &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 复制实例底层拒绝占用目录", error);
        check(!vm.deleteInstance(srcId, &error) && error.contains(QStringLiteral("占用")),
              "ZJ5 删除实例底层拒绝占用目录", error);
        check(!vm.renameInstance(srcId, QStringLiteral("locked-name"), &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 改名实例底层拒绝占用目录", error);
        check(!vm.updateVersionMeta(srcId, {{QStringLiteral("name"), QStringLiteral("locked")}}, &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 实例元数据底层拒绝占用目录", error);
        const QString saves = srcDir + QStringLiteral("/saves");
        check(!WorldFiles::copyWorldInto(worldSrc, saves, QStringLiteral("locked-world"), &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 导入世界解析所属数据根锁", error);
        check(!WorldFiles::renameWorld(saves + QStringLiteral("/我的世界"), QStringLiteral("locked-world"), &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 世界改名解析所属数据根锁", error);
        check(!WorldFiles::writeLevelData(saves + QStringLiteral("/我的世界"), {}, &error)
                  && error.contains(QStringLiteral("占用")), "ZJ5 世界设定写入解析所属数据根锁", error);
        DirectorySessionLock::enabled = enabledBefore;
        check(QFileInfo::exists(srcDir) && !QFileInfo::exists(destRootPath + QStringLiteral("/versions/locked-copy"))
                  && !QFileInfo::exists(saves + QStringLiteral("/locked-world")),
              "ZJ5 拒绝后无副本半成品且源实例仍在");
    }
    {
        QString error;
        check(vm.copyInstance(srcId, destRootPath, QStringLiteral("副本"), &error),
              "ZJ5 正常复制成功", error);
        const QString dest = destRootPath + QStringLiteral("/versions/副本");
        check(existsAt(dest), "ZJ5 副本落在 <目标游戏文件夹>/versions/<名字> 下");
        check(existsAt(dest + QStringLiteral("/副本.jar")) &&
                  existsAt(dest + QStringLiteral("/副本.json")),
              "ZJ5 jar / json 跟着目录名改了（不改的话列表里根本认不出它）");
        check(!existsAt(dest + QLatin1Char('/') + srcId + QStringLiteral(".jar")) &&
                  !existsAt(dest + QLatin1Char('/') + srcId + QStringLiteral(".json")),
              "ZJ5 老名字那一份没留下（是改名不是多拷一份）");
        check(existsAt(dest + QStringLiteral("/mods/sodium.jar")) &&
                  existsAt(dest + QStringLiteral("/saves/我的世界/level.dat")),
              "ZJ5 嵌套目录也原样带过去了（模组、存档一起走）");
        check(existsAt(srcDir + QLatin1Char('/') + srcId + QStringLiteral(".jar")),
              "ZJ5 源一个字节都没动（这一条是整件事的底线）");

        const QJsonObject meta = readMetaAt(dest);
        check(meta.value(QStringLiteral("name")).toString() == QStringLiteral("副本"),
              "ZJ5 副本的 wtfml.meta 里显示名换成新的了",
              meta.value(QStringLiteral("name")).toString());
        const QString newId = meta.value(QStringLiteral("instanceId")).toString();
        check(!newId.isEmpty() && newId != QStringLiteral("source-uuid-1111"),
              "ZJ5 副本拿到一个**全新的**稳定 id（沿用源的话分组/图标会把两份认成一份）", newId);
        check(meta.value(QStringLiteral("loaderName")).toString() == QStringLiteral("Fabric"),
              "ZJ5 别的字段原样留着（只动 name 和 instanceId）");
        check(readMetaAt(srcDir).value(QStringLiteral("instanceId")).toString() ==
                  QStringLiteral("source-uuid-1111"),
              "ZJ5 源那份 meta 也没被动过");
        check(!existsAt(destRootPath + QStringLiteral("/versions/.wtfml-staging")),
              "★★ ZJ5 正常复制完成后暂存层也收掉了（不留一个永远认不出的目录）");
    }
    {
        // 慢盘 / 大半截失败时的底线：目标位置从来没有被写过，所以不存在“删掉用户刚放进去
        // 的东西”这一步。这里用源里的 junction 让拷树中途必然失败。
        const QString badId = QStringLiteral("坏源");
        const QString badDir = makeInstanceDir(sourceRoot.path() + QStringLiteral("/versions"),
                                               badId, R"({"name":"坏源","instanceId":"bad-uuid"})");
        const QString outside = scratch.path() + QStringLiteral("/坏源链接那一头");
        QDir().mkpath(outside);
        writeFileAt(outside + QStringLiteral("/keep.txt"), QByteArrayLiteral("KEEP"));
        const QString linkPath = badDir + QStringLiteral("/link");
        const bool madeLink =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"),
                               QStringLiteral("/J"), QDir::toNativeSeparators(linkPath),
                               QDir::toNativeSeparators(outside)}) == 0
            && QFileInfo(linkPath).isJunction();
        check(madeLink, "ZJ5 夹具：在坏源里造一根真 junction（mklink /J）");
        if (madeLink) {
            QString error;
            check(!vm.copyInstance(badId, destRootPath, QStringLiteral("坏副本"), &error),
                  "★★ ZJ5 源里有链接 ⇒ 复制失败", error);
            check(error.contains(QStringLiteral("链接")),
                  "★★ ZJ5 失败原因说到那根链接（不是只回一句「中断了」）", error);
            check(!existsAt(destRootPath + QStringLiteral("/versions/坏副本")),
                  "★★★ ZJ5 失败时 versions/ 下不出现半份实例");
            check(!existsAt(destRootPath + QStringLiteral("/versions/.wtfml-staging")),
                  "★★★ ZJ5 失败后本次暂存也清掉了");
            QFile keep(outside + QStringLiteral("/keep.txt"));
            check(keep.open(QIODevice::ReadOnly) && keep.readAll() == QByteArrayLiteral("KEEP"),
                  "★★ ZJ5 链接那一头一个字节都没动");
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(linkPath)});
            QDir().rmdir(badDir);
        }
    }
    {
        // 慢盘上最危险的那个窗口：拷树跑了几分钟，用户（或另一份启动器）在同样的名字下
        // 放了东西。旧写法会直接往目标目录里拷，失败时再把整个目录删掉 —— 那一下删的
        // 就不只是我们的半成品。现在只拷到暂存，发布那一下发现目标已被占用就只清暂存。
        const QString busyId = QStringLiteral("竞态源");
        const QString busyDir = makeInstanceDir(sourceRoot.path() + QStringLiteral("/versions"),
                                                busyId, R"({"name":"竞态源","instanceId":"race-uuid"})");
        QDir().mkpath(busyDir + QStringLiteral("/data"));
        for (int i = 0; i < 4000; ++i) {
            writeFileAt(busyDir + QStringLiteral("/data/%1.bin").arg(i, 5, 10, QLatin1Char('0')),
                        QByteArray(1024, 'x'));
        }
        const QString raceName = QStringLiteral("竞态副本");
        const QString raceDest = destRootPath + QStringLiteral("/versions/") + raceName;
        const QString stagingParent = destRootPath + QStringLiteral("/versions/.wtfml-staging");
        bool sawStaging = false;
        bool madeDest = false;
        // 必须用 QThread：在非 Qt 线程里调 QtCore，线程退出时可能去解一个空的
        // QThreadData TLS（见上面 ZJ3 的说明）。
        QThread *racer = QThread::create([&] {
            QElapsedTimer deadline;
            deadline.start();
            while (deadline.elapsed() < 10000) {
                if (QFileInfo::exists(stagingParent)) {
                    sawStaging = true;
                    break;
                }
                QThread::msleep(1);
            }
            if (!sawStaging) return;
            if (QDir().mkpath(raceDest)) {
                writeFileAt(raceDest + QStringLiteral("/用户刚写的.txt"),
                            QByteArrayLiteral("USER"));
                madeDest = true;
            }
        });
        racer->start();
        QString error;
        const bool copied = vm.copyInstance(busyId, destRootPath, raceName, &error);
        racer->wait();
        delete racer;
        check(sawStaging, "ZJ5 夹具：拷树期间真看到了暂存层");
        check(madeDest, "ZJ5 夹具：另一份确实在拷树期间创建了同名目录");
        check(!copied && error.contains(QStringLiteral("已存在名为")),
              "★★★ ZJ5 拷树期间目标名字被占用 ⇒ 发布失败、不报成功", error);
        QFile userFile(raceDest + QStringLiteral("/用户刚写的.txt"));
        check(userFile.open(QIODevice::ReadOnly) && userFile.readAll() == QByteArrayLiteral("USER"),
              "★★★ ZJ5 失败清理只删自己的暂存，用户在目标目录里放的东西原样保留");
        check(!existsAt(stagingParent), "★★ ZJ5 这次失败也没留下暂存层");
        QDir(raceDest).removeRecursively();
        QDir(busyDir).removeRecursively();
    }
    {
        // 目标文件夹里还没有 versions/ —— 要自己建出来（用户挑一个从没跑过游戏的新文件夹
        // 是常态）。这一条和上面那个"已经有了"是两回事。
        const QString fresh = scratch.path() + QStringLiteral("/新游戏文件夹");
        QDir().mkpath(fresh);
        QString error;
        check(vm.copyInstance(srcId, fresh, QStringLiteral("第一份"), &error),
              "ZJ5 目标里还没有 versions/ 时也成（自己建出来）", error);
        check(existsAt(fresh + QStringLiteral("/versions/第一份/第一份.jar")),
              "ZJ5 而且落在新建的 versions/ 下面");
    }
    {
        const QString decoyRoot = scratch.path() + QStringLiteral("/换过去的游戏文件夹");
        const QString decoyDir = makeInstanceDir(decoyRoot + QStringLiteral("/versions"), srcId,
                                                 R"({"name":"替身","instanceId":"decoy-uuid"})");
        writeFileAt(decoyDir + QLatin1Char('/') + srcId + QStringLiteral(".jar"),
                    "decoy-jar-bytes");
        AppPaths::setMinecraftDir(decoyRoot);
        QString error;
        check(vm.copyInstanceFrom(srcDir, srcId, destRootPath, QStringLiteral("显式源"), &error),
              "ZJ5 copyInstanceFrom 用传进来的源目录（不看当前数据目录）", error);
        const QString explicitDest = destRootPath + QStringLiteral("/versions/显式源");
        QFile copiedJar(explicitDest + QStringLiteral("/显式源.jar"));
        const bool jarReadable = copiedJar.open(QIODevice::ReadOnly);
        const QByteArray copiedBytes = jarReadable ? copiedJar.readAll() : QByteArray();
        check(jarReadable && copiedBytes == QByteArrayLiteral("fake-jar-bytes"),
              "★ ZJ5 拷的是传进来的那一份源，不是当前游戏文件夹里的同名替身", copiedBytes);
        check(!existsAt(explicitDest + QLatin1Char('/') + srcId + QStringLiteral(".jar")),
              "ZJ5 显式源这一份的 jar 也跟着新名字改了");
        AppPaths::setMinecraftDir(sourceRoot.path());  // 后面几条拒绝路径还在用当前目录
    }
    {
        QString error;
        check(!vm.copyInstance(srcId, destRootPath, QStringLiteral("副本"), &error),
              "ZJ5 目标重名拒绝");
        check(error.contains(QStringLiteral("已存在名为")), "ZJ5 原因说清了重名", error);

        // 挑的就是自己那一份所在的那个游戏文件夹 + 同一个名字：真正的原因是"那就是它自己"，
        // 不能报成"已存在同名实例"（用户会以为盘上真有两份）。
        check(!vm.copyInstance(srcId, sourceRoot.path(), srcId, &error),
              "ZJ5 目标是源自己那一份拒绝");
        check(error.contains(QStringLiteral("就是这一份自己")), "ZJ5 而且原因说的是这话", error);

        // 目标落在**源实例目录自己里面** → 拒（ 补的第四道闸）。这一条在数据
        // 层是兜底（界面那一层也有一条，用的是同一个 InstanceFiles::pathIsSameOrInside），
        // 但**必须自己站得住**：不然将来任何一个别的调用点喂进来一个这样的目标，下面那步
        // mkpath(versionsDir) 就会在源实例目录内部造出一个 `versions/`，紧接着"先 mkpath
        // 目标、再列源目录"的复制会把它自己刚造的这一层也列进去 —— **自己抄自己、每层再
        // 深一层**，一声不响地写到盘满。
        check(!vm.copyInstance(srcId, srcDir, QStringLiteral("里面"), &error),
              "★★ ZJ5 目标就是源实例目录自己拒绝（判据比的是规范化之后的真路径）");
        // 注意别把整个句子当成一截 —— 实例名是插在中间那句里的（「不能把「X」复制到它自己
        // 里面去」），所以只挑不被名字隔开的那半截来比。
        check(error.contains(QStringLiteral("目录相同，或位于该目录内")),
              "ZJ5 而且原因说的是这话", error);
        check(!existsAt(srcDir + QStringLiteral("/versions")),
              "★★ ZJ5 拒绝之后**源实例目录里没多出任何东西**（多出一个 "
              "`versions/`，那就是递归的第一层）");
        check(!vm.copyInstance(srcId, srcDir + QStringLiteral("/saves"), QStringLiteral("更里面"),
                               &error),
              "★★ ZJ5 挑的是它**底下**的子目录也一样拒（判的是「是不是在里面」）");
        check(!existsAt(srcDir + QStringLiteral("/saves/versions")),
              "★ ZJ5 同样是「盘上没多出东西来」");

        // 别名：拿一个**指着源实例目录的 junction** 当游戏文件夹。
        //
        // 这一条是 实测补上的：`canonicalFilePath()` **不解 junction**
        // （Windows 上它走 `slowCanonicalized()`、逐段判的是 `isSymLink()`，而 junction 在
        // Qt 里是 `JunctionType`；见 InstanceFiles.cpp 里 resolveJunctions() 那一大段）。
        // 只靠 canonicalFilePath 的话，这一条会让复制真的跑起来（自己抄自己，直到撞上路径
        // 长度上限才失败）—— 那时 `!ok` 还是绿的，是个**假绿**，而盘上已经多出一层
        // `versions/`。所以下面同时钉住"报的是哪一句"。
        //
        // NOTE: junction 必须**最后**清掉、而且只能 rmdir（删链接本体，不动它那一头）；
        // `QTemporaryDir::removeRecursively()` 是会**跟着** junction 往下删的。
        const QString aliasHost = scratch.path() + QStringLiteral("/别名宿主");
        QDir().mkpath(aliasHost);
        const QString aliasDir = aliasHost + QStringLiteral("/指到源实例");
        const bool madeAlias =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(aliasDir),
                               QDir::toNativeSeparators(srcDir)}) == 0
            && QFileInfo(aliasDir).isJunction();
        check(madeAlias, "★ ZJ5 先造出一个真 junction 指着源实例目录（mklink /J）");
        if (madeAlias) {
            // NOTE: 前置事实 —— canonicalFilePath **不**解 junction。这一条要是红了，
            // 说明 Qt 那边换了实现（那时候 resolveJunctions 可以删，但先得有人复核）。
            check(QDir::cleanPath(QFileInfo(aliasDir).canonicalFilePath())
                      != QDir::cleanPath(QFileInfo(srcDir).canonicalFilePath()),
                  "★ ZJ5 前置事实：canonicalFilePath 解不掉 junction（这一位就是那个洞的出处）",
                  QFileInfo(aliasDir).canonicalFilePath());
            check(!vm.copyInstance(srcId, aliasDir, QStringLiteral("别名副本"), &error),
                  "★★ ZJ5 目标是**指着源实例目录的 junction** 也拒（判的是它真指着哪儿）", error);
            check(error.contains(QStringLiteral("目录相同，或位于该目录内")),
                  "★ ZJ5 而且拒的是那一道闸，不是「跑下去撞了墙才失败」", error);
            check(!existsAt(srcDir + QStringLiteral("/versions")),
                  "★★ ZJ5 拒绝之后源实例目录里还是没多出东西来（假绿的出处就在这一格）");
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(aliasDir)});
        }

        check(!vm.copyInstance(srcId, destRootPath, QStringLiteral("a/b"), &error),
              "ZJ5 名字不合法拒绝");
        check(!vm.copyInstance(srcId, destRootPath, QString(), &error), "ZJ5 空名字拒绝");
        check(!vm.copyInstance(QString(), destRootPath, QStringLiteral("x"), &error),
              "ZJ5 空的源 id 拒绝（拼出来是 versions/ 本身）");
        check(!vm.copyInstance(srcId, scratch.path() + QStringLiteral("/不在盘上"), QStringLiteral("x"),
                               &error),
              "ZJ5 目标游戏文件夹不存在拒绝");
        check(!vm.copyInstance(srcId, QString(), QStringLiteral("x"), &error),
              "ZJ5 空的「复制到哪儿」拒绝 —— 空串会被 QDir 当成当前工作目录，那不是用户挑的地方");
    }
    {
        // 最后一步失败时**把自己刚拷出来的半份删掉**（和整合包导入那条"失败不删半成品"
        // 故意相反）。造法：源里那份 wtfml.meta 是坏 JSON ⇒ 拷完树、改完 jar/json 之后
        // 卡在第 4 步。这一条同时钉住两件事：那一趟**返回 false**，以及目标目录
        // **没留在盘上**（判据必须看盘，不能只看返回值 —— 半份留着才是真正的坑）。
        const QString badId = QStringLiteral("坏meta");
        makeInstanceDir(sourceRoot.path() + QStringLiteral("/versions"), badId,
                        QByteArray("{ 这不是 JSON"));
        QString error;
        check(!vm.copyInstance(badId, destRootPath, QStringLiteral("半份"), &error),
              "ZJ5 源那份 meta 是坏 JSON 时这一趟失败");
        // 措辞由 InstanceMetaFile 给（读不懂 / 版本不认识 / 写不进三种都走那一句），
        // 这里只要它点名是哪个文件 —— 用户得知道去改哪一份。
        check(error.contains(QStringLiteral("wtfml.meta")), "ZJ5 原因说清了是 meta 读不懂", error);
        check(!existsAt(destRootPath + QStringLiteral("/versions/半份")),
              "ZJ5 失败之后那份没拷完的自己删掉了（saves 里不该留一个来路不明的目录）");
    }

    AppPaths::setMinecraftDir(originalDir);  // 收摊
    check(AppPaths::minecraftDir() == originalDir, "ZJ6 数据根目录换回来了");

    // =====================================================================
    // ZK. 服务端那八档：装法、地址、以及"装完之后入口是什么"
    // =====================================================================
    // 这一节全是**纯函数**：地址怎么拼、接口回复怎么读、装一份服务端的计划长什么样。
    // 一个请求都不发、一个文件都不读、一个字节都不写盘。
    //
    // NOTE: 断言里那些地址原串是 逐条 curl 出来的（形状连同响应头一起记在
    // ServerInstall.hpp 的文件头）。**上游改了它们这里会红，红得对** —— 这一层存在的
    // 意义就是"别让一个改过期的地址活到用户点下去那一刻"。
    section("ZK1. 八档的身份：代号往返、认不出来不碰 out、顺序就是界面上的顺序");
    {
        using namespace ServerInstall;
        const QList<Flavor> flavors = allFlavors();
        check(flavors.size() == 8, "ZK1 一共八档", QString::number(flavors.size()));

        QStringList roundTripBad;
        QStringList tokens;
        QStringList blankNames;
        bool allNamesOk = true;
        bool allNotesOk = true;
        for (Flavor flavor : flavors) {
            const QString token = flavorToken(flavor);
            tokens.append(token);
            if (flavorName(flavor).isEmpty()) {
                allNamesOk = false;
            }
            if (flavorNote(flavor).isEmpty()) {
                allNotesOk = false;
            }
            Flavor back = Flavor::Spigot;  // 先塞一个"错"的，看它会不会被写坏
            if (!flavorFromToken(token, &back) || back != flavor) {
                roundTripBad.append(token);
            }
        }
        check(roundTripBad.isEmpty(), "ZK1 八档代号逐条往返（token -> Flavor -> token）",
              roundTripBad.join(QStringLiteral(", ")));
        check(QSet<QString>(tokens.begin(), tokens.end()).size() == tokens.size(),
              "ZK1 八个代号互不相同（撞了的话盘上两份服务端会被认成同一档）",
              tokens.join(QStringLiteral(" ")));
        check(allNamesOk, "ZK1 八档都有界面上那个名字");
        check(allNotesOk, "ZK1 八档都有那行小字（界面别自己另写一套 —— 这是唯一出处）");

        Flavor kept = Flavor::Purpur;
        check(!flavorFromToken(QStringLiteral("Vanilla"), &kept) && kept == Flavor::Purpur,
              "ZK1 大小写不对就不认（代号是持久化格式，不能「宽容」到把两种写法都收下）");
        check(!flavorFromToken(QString(), &kept) && kept == Flavor::Purpur,
              "ZK1 空串回 false，而且**不碰 out**");
        check(flavorFromToken(QStringLiteral("paper"), nullptr),
              "ZK1 out 给 nullptr 也不崩（只想知道「认不认得」时会这么用）");

        check(flavors.first() == Flavor::Vanilla && flavors.last() == Flavor::Spigot,
              "ZK1 首尾是原版和 Spigot —— allFlavors() 这个顺序就是界面上列出来的顺序");
    }

    section("ZK2. 装法：八档各是哪一种，那句小字是什么");
    {
        using namespace ServerInstall;
        check(installMethod(Flavor::Vanilla) == InstallMethod::DirectJar &&
                  installMethod(Flavor::Paper) == InstallMethod::DirectJar &&
                  installMethod(Flavor::Purpur) == InstallMethod::DirectJar &&
                  installMethod(Flavor::Fabric) == InstallMethod::DirectJar,
              "ZK2 原版 / Paper / Purpur / Fabric：下下来那个 jar 就能跑");
        check(installMethod(Flavor::Forge) == InstallMethod::RunInstaller &&
                  installMethod(Flavor::NeoForge) == InstallMethod::RunInstaller &&
                  installMethod(Flavor::Quilt) == InstallMethod::RunInstaller,
              "ZK2 Forge / NeoForge / Quilt：下下来的是安装器，要在安装目录里跑一次");
        check(installMethod(Flavor::Spigot) == InstallMethod::BuildRequired,
              "ZK2 Spigot：官方不发成品，得现编");

        const QString direct = installMethodText(InstallMethod::DirectJar);
        const QString run = installMethodText(InstallMethod::RunInstaller);
        const QString build = installMethodText(InstallMethod::BuildRequired);
        check(!direct.isEmpty() && !run.isEmpty() && !build.isEmpty() && direct != run &&
                  run != build && direct != build,
              "ZK2 三句话都不空、互相不一样");
    }

    section("ZK3. 地址怎么拼");
    {
        using namespace ServerInstall;
        check(paperBuildsUrl(QStringLiteral("1.21.4")) ==
                  QStringLiteral("https://fill.papermc.io/v3/projects/paper/versions/1.21.4/builds"),
              "ZK3 Paper 构建列表（旧接口 api.papermc.io/v2 已经 410 下线，别照老教程写）",
              paperBuildsUrl(QStringLiteral("1.21.4")));
        check(paperProjectUrl() == QStringLiteral("https://fill.papermc.io/v3/projects/paper"),
              "ZK3 Paper 工程信息");
        check(purpurVersionsUrl() == QStringLiteral("https://api.purpurmc.org/v2/purpur"),
              "ZK3 Purpur 版本清单");
        check(purpurLatestUrl(QStringLiteral("1.21.4")) ==
                  QStringLiteral("https://api.purpurmc.org/v2/purpur/1.21.4/latest"),
              "ZK3 Purpur 最新一条");
        check(purpurDownloadUrl(QStringLiteral("1.21.4"), QStringLiteral("latest")) ==
                  QStringLiteral("https://api.purpurmc.org/v2/purpur/1.21.4/latest/download"),
              "ZK3 Purpur 下载：latest 真的能这么用（回 200）",
              purpurDownloadUrl(QStringLiteral("1.21.4"), QStringLiteral("latest")));
        check(purpurDownloadUrl(QStringLiteral("1.21.4"), QStringLiteral("2416")) ==
                  QStringLiteral("https://api.purpurmc.org/v2/purpur/1.21.4/2416/download"),
              "ZK3 Purpur 下载：具体构建号");

        const QString fabricJar = fabricServerJarUrl(QStringLiteral("1.21.4"),
                                                     QStringLiteral("0.16.9"),
                                                     QStringLiteral("1.0.1"));
        check(fabricJar == QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/"
                                          "1.21.4/0.16.9/1.0.1/server/jar"),
              "ZK3 Fabric 服务端 jar", fabricJar);
        check(fabricJar.contains(QStringLiteral("/loader/1.21.4/0.16.9/1.0.1/")) &&
                  fabricJar.endsWith(QStringLiteral("/server/jar")),
              "ZK3 ★ Fabric 那条里游戏版本、加载器、安装器三段都在 —— 少任何一段上游就是 404"
              "（少了 loader 段和少了 installer 段两种都走过）");
        check(fabricInstallerListUrl() ==
                  QStringLiteral("https://meta.fabricmc.net/v2/versions/installer"),
              "ZK3 Fabric 安装器列表");

        check(quiltInstallerMetadataUrl() ==
                  QStringLiteral("https://maven.quiltmc.org/repository/release/org/quiltmc/"
                                 "quilt-installer/maven-metadata.xml"),
              "ZK3 Quilt 安装器清单");
        check(quiltInstallerUrl(QStringLiteral("0.15.1")) ==
                  QStringLiteral("https://maven.quiltmc.org/repository/release/org/quiltmc/"
                                 "quilt-installer/0.15.1/quilt-installer-0.15.1.jar"),
              "ZK3 Quilt 安装器 jar");

        check(spigotBuildToolsUrl() ==
                  QStringLiteral("https://hub.spigotmc.org/jenkins/job/BuildTools/"
                                 "lastSuccessfulBuild/artifact/target/BuildTools.jar"),
              "ZK3 Spigot 的 BuildTools（「最后一次成功的构建」—— 今天编和下周编不是同一个文件）",
              spigotBuildToolsUrl());
    }

    section("ZK4. Paper 的构建列表怎么读");
    {
        using namespace ServerInstall;
        const auto makeBuild = [](int id, const char *channel, const char *name, const char *sha,
                                  double size) {
            const QString fileName = QString::fromLatin1(name);
            const QJsonObject file{
                {QStringLiteral("name"), fileName},
                {QStringLiteral("url"), QStringLiteral("https://fill-data.example/") + fileName},
                {QStringLiteral("checksums"),
                 QJsonObject{{QStringLiteral("sha256"), QString::fromLatin1(sha)}}},
                {QStringLiteral("size"), size}};
            return QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("channel"), QString::fromLatin1(channel)},
                {QStringLiteral("downloads"),
                 QJsonObject{{QStringLiteral("server:default"), file}}}};
        };

        QJsonArray raw;
        raw.append(makeBuild(1, "ALPHA", "paper-1.21.4-1.jar", "aaa", 52673513));
        raw.append(makeBuild(2, "ALPHA", "paper-1.21.4-2.jar", "bbb", 52673514));
        raw.append(makeBuild(3, "STABLE", "paper-1.21.4-3.jar", "ccc", 52673515));
        // 上游真会给这种：那个构建的东西被撤了，downloads 是空的。
        raw.append(QJsonObject{{QStringLiteral("id"), 4},
                               {QStringLiteral("channel"), QStringLiteral("STABLE")},
                               {QStringLiteral("downloads"), QJsonObject{}}});

        const QList<Build> list = paperBuildsFromJson(raw);
        check(list.size() == 3, "ZK4 没有可下文件的那一条被跳过", QString::number(list.size()));
        check(!list.isEmpty() && list.first().number == 3 && list.last().number == 1,
              "ZK4 排成新到旧（上游回的是升序：224 条，头一条 id=1）",
              list.isEmpty() ? QString()
                             : QStringLiteral("%1 .. %2").arg(list.first().number).arg(list.last().number));
        check(!list.isEmpty() && list.first().sha256 == QStringLiteral("ccc") &&
                  list.first().size == 52673515 &&
                  list.first().fileName == QStringLiteral("paper-1.21.4-3.jar"),
              "ZK4 校验值 / 大小 / 上游那个文件名三样都读出来了",
              list.isEmpty() ? QString() : list.first().sha256);
        check(!list.isEmpty() && list.first().channel == QStringLiteral("STABLE"),
              "ZK4 频道原样留着（界面上要标「这是测试版」就靠它）");

        const Build mixed{0, QStringLiteral("stable"), {}, QStringLiteral("u"), {}, {}, -1};
        check(!list.isEmpty() && paperBuildIsStable(list.first()) && paperBuildIsStable(mixed),
              "ZK4 判 stable 不看大小写（上游现在写的大写，别为这个留个坑）");
        check(!paperBuildIsStable(Build{0, QStringLiteral("ALPHA"), {}, QStringLiteral("u"), {}, {}, -1}),
              "ZK4 ALPHA 不是稳定档");

        check(pickPaperBuild(list, true).number == 3, "ZK4 挑稳定档：最新那条 STABLE");
        check(pickPaperBuild(list, false).number == 3, "ZK4 挑最新档：就是最新那条");

        // 新游戏版本刚上的那几天上游只有 ALPHA（真会这样）：
        QJsonArray alphaOnly;
        alphaOnly.append(makeBuild(1, "ALPHA", "paper-x-1.jar", "a", 1));
        const QList<Build> alphaList = paperBuildsFromJson(alphaOnly);
        check(pickPaperBuild(alphaList, true).number == 1,
              "ZK4 只有 ALPHA 时「稳定档」退回最新那条 —— 不许挑不出来（挑不出来这一档就整个废掉）");
        check(!pickPaperBuild(QList<Build>(), true).ok() &&
                  !pickPaperBuild(QList<Build>(), false).ok(),
              "ZK4 空列表 -> 空那一条，ok() 为 false");

        const QJsonObject project{
            {QStringLiteral("project"),
             QJsonObject{{QStringLiteral("id"), QStringLiteral("paper")}}},
            {QStringLiteral("versions"),
             QJsonObject{
                 {QStringLiteral("1.21"),
                  QJsonArray{QStringLiteral("1.21.11"), QStringLiteral("1.21.4"),
                             QStringLiteral("1.21.4")}},
                 {QStringLiteral("1.20"),
                  QJsonArray{QStringLiteral("1.20.6"), QStringLiteral("1.20.1")}}}}};
        const QStringList versions = paperVersionsFromJson(project);
        check(versions.size() == 4, "ZK4 工程信息摊平成一维并去掉重复",
              versions.join(QStringLiteral(" ")));
        // 这条是**自己排出来的**，不是原序：上游那个对象走到这一层时键已经按字典序排过
        // （QJsonObject 就是这么存的），照原序摊平出来是 `1.20.6 1.20.1 1.21.11 1.21.4`
        // —— 正好是反的。真实数据里 `1.21` 还会排在 `26.1` 前面（字典序 `1` < `2`），
        // 拿它当"新到旧"用会挑中最老的版本。
        check(!versions.isEmpty() && versions.first() == QStringLiteral("1.21.11") &&
                  versions.last() == QStringLiteral("1.20.1"),
              "ZK4 ★ 摊平之后**自己排成新到旧**（1.21 那组排在 1.20 那组前面 —— "
              "字典序会把它排反）", versions.join(QStringLiteral(" ")));

        // 正式版排在它的 rc 前面：两个都解析成 [1,21,11]，只有 tie-break 能分开
        const QJsonObject withRc{
            {QStringLiteral("versions"),
             QJsonObject{{QStringLiteral("1.21"),
                          QJsonArray{QStringLiteral("1.21.11-rc3"), QStringLiteral("1.21.11")}}}}};
        const QStringList rcVersions = paperVersionsFromJson(withRc);
        check(rcVersions.size() == 2 && rcVersions.first() == QStringLiteral("1.21.11") &&
                  rcVersions.last() == QStringLiteral("1.21.11-rc3"),
              "ZK4 同一个版本号的正式版排在它的 rc 前面（沿用加载器那套 tie-break，"
              "界面上第一个不该是候选版）", rcVersions.join(QStringLiteral(" ")));
    }

    section("ZK5. Purpur 的两种回复：latest 那条和工程信息");
    {
        using namespace ServerInstall;
        const QJsonObject latest{{QStringLiteral("project"), QStringLiteral("purpur")},
                                 {QStringLiteral("version"), QStringLiteral("1.21.4")},
                                 {QStringLiteral("build"), QStringLiteral("2416")},
                                 {QStringLiteral("result"), QStringLiteral("SUCCESS")}};
        check(purpurBuildFromJson(latest) == QStringLiteral("2416"),
              "ZK5 从 latest 那条回复里读出构建号", purpurBuildFromJson(latest));

        QJsonObject numeric = latest;
        numeric.insert(QStringLiteral("build"), 2416);
        check(purpurBuildFromJson(numeric).isEmpty(),
              "ZK5 ★ build 写成数字而不是字符串时**当读不出来**，不许当成 2416 —— 按数字读会拼出"
              " /purpur/1.21.4/0/download，一路 404 而且看着像网络问题");

        QJsonObject failed = latest;
        failed.insert(QStringLiteral("result"), QStringLiteral("FAILURE"));
        check(purpurBuildFromJson(failed).isEmpty(),
              "ZK5 result 不是 SUCCESS 的那条不能下（上游会把失败那几条也报出来）");

        QJsonObject noBuild = latest;
        noBuild.remove(QStringLiteral("build"));
        check(purpurBuildFromJson(noBuild).isEmpty(), "ZK5 没有 build 字段那条也读不出来");
        check(purpurBuildFromJson(QJsonObject()).isEmpty(), "ZK5 空对象也读不出来（不崩）");

        const QJsonObject project{
            {QStringLiteral("project"), QStringLiteral("purpur")},
            {QStringLiteral("metadata"),
             QJsonObject{{QStringLiteral("current"), QStringLiteral("26.2")}}},
            {QStringLiteral("versions"),
             QJsonArray{QStringLiteral("1.21.11"), QStringLiteral("1.21.4"),
                        QStringLiteral("1.21.4"), QStringLiteral("1.8.9")}}};
        const QStringList versions = purpurVersionsFromJson(project);
        check(versions.size() == 3, "ZK5 Purpur 那份是**平铺**的一维数组（不是像 Paper 那样"
                                    "按大版本分组的），去重",
              versions.join(QStringLiteral(" ")));
        check(!versions.isEmpty() && versions.first() == QStringLiteral("1.21.11") &&
                  versions.last() == QStringLiteral("1.8.9"),
              "ZK5 ★ 排成新到旧（原序是升序，直接用就反了）",
              versions.join(QStringLiteral(" ")));
    }

    section("ZK6. Fabric 的安装器列表：挑一个出来才拼得出地址");
    {
        using namespace ServerInstall;
        const QJsonArray raw{
            QJsonObject{{QStringLiteral("url"),
                         QStringLiteral("https://maven.fabricmc.net/net/fabricmc/"
                                        "fabric-installer/1.1.2/fabric-installer-1.1.2.jar")},
                        {QStringLiteral("maven"),
                         QStringLiteral("net.fabricmc:fabric-installer:1.1.2")},
                        {QStringLiteral("version"), QStringLiteral("1.1.2")},
                        {QStringLiteral("stable"), true}},
            QJsonObject{{QStringLiteral("version"), QStringLiteral("1.1.1")},
                        {QStringLiteral("stable"), false}},
            QJsonObject{{QStringLiteral("stable"), true}}};  // 缺 version 的：跳过
        const LoaderVersionList installers = listFabricInstallerVersions(raw);
        check(installers.size() == 2, "ZK6 缺 version 的那一条被跳过",
              QString::number(installers.size()));
        check(!installers.isEmpty() && installers.first().version == QStringLiteral("1.1.2") &&
                  installers.first().stable,
              "ZK6 顺序原样保留（上游就是新到旧）+ stable 那个字段照读");
        check(pickFabricInstaller(raw, true) == QStringLiteral("1.1.2"),
              "ZK6 稳定档：第一条 stable 的");
        check(pickFabricInstaller(raw, false) == QStringLiteral("1.1.2"),
              "ZK6 最新档：就是第一条");

        const QJsonArray noStable{QJsonObject{{QStringLiteral("version"), QStringLiteral("2.0")},
                                              {QStringLiteral("stable"), false}}};
        check(pickFabricInstaller(noStable, true) == QStringLiteral("2.0"),
              "ZK6 一个 stable 都没有时退回最新那条 —— 挑不出来就拼不出地址，一整档废掉");
        check(pickFabricInstaller(QJsonArray(), true).isEmpty() &&
                  pickFabricInstaller(QJsonArray(), false).isEmpty(),
              "ZK6 空列表 -> 空串");
    }

    section("ZK7. Quilt 的安装器清单（maven 元数据）");
    {
        using namespace ServerInstall;
        const QByteArray xml = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<metadata>\n  <groupId>org.quiltmc</groupId>\n"
            "  <artifactId>quilt-installer</artifactId>\n  <versioning>\n"
            "    <latest>9.9.9</latest>\n    <release>9.9.9</release>\n    <versions>\n"
            "      <version>0.4.0</version>\n      <version>0.15.0</version>\n"
            "      <version>0.15.1</version>\n    </versions>\n  </versioning>\n</metadata>\n");
        const LoaderVersionList quilt = listQuiltInstallerVersions(xml);
        check(quilt.size() == 3, "ZK7 只扫 <version> 那一段（<latest>/<release> 不是版本号）",
              QStringLiteral("扫出 %1 条").arg(quilt.size()));
        check(!quilt.isEmpty() && quilt.first().version == QStringLiteral("0.15.1") &&
                  quilt.last().version == QStringLiteral("0.4.0"),
              "ZK7 排成新到旧（maven 清单是按发布时间排的，不排会挑中旧的那个）",
              quilt.isEmpty() ? QString() : quilt.first().version);
        check(!quilt.isEmpty() && quilt.first().version != QStringLiteral("9.9.9"),
              "ZK7 ★ <latest>/<release> 那两行不许被当成一个版本号收进来");

        const LoaderVersionList beta =
            listQuiltInstallerVersions(QByteArrayLiteral(
                "<versions><version>0.16.0-beta</version><version>0.16.0</version></versions>"));
        check(beta.size() == 2 && beta.first().stable && !beta.last().stable,
              "ZK7 stable 判据沿用 LoaderMeta 那一条：版本号里没有 - 也没有 +（不另写一套）；"
              "数字一样的两个**正式版排前面**（界面上第一个不该是 beta）",
              beta.isEmpty() ? QString() : beta.first().version);
        check(listQuiltInstallerVersions(QByteArrayLiteral("<metadata/>")).isEmpty(),
              "ZK7 一个 <version> 都没有 -> 空列表（不崩）");
    }

    section("ZK8. 八档各拼一份安装计划");
    {
        using namespace ServerInstall;
        Request base;
        base.gameVersion = QStringLiteral("1.21.4");
        base.loaderVersion = QStringLiteral("0.16.9");
        base.installerVersion = QStringLiteral("1.0.1");

        // ---- 原版 ----
        {
            Request request = base;
            request.flavor = Flavor::Vanilla;
            const Plan plan = planFor(request);
            check(plan.ok && plan.method == InstallMethod::DirectJar, "ZK8 原版：能装");
            check(plan.downloads.isEmpty(),
                  "ZK8 ★ 原版这一档的下载地址**不在这层**：它是版本 json 的一部分"
                  "（downloads.server，见 vanillaServerDownload）");
            check(plan.installedMarkers == QStringList{QStringLiteral("server.jar")} &&
                      plan.launchTokens == QStringList{QStringLiteral("-jar"),
                                                        QStringLiteral("server.jar"),
                                                        QStringLiteral("nogui")},
                  "ZK8 原版：装完看 server.jar，启动 java -jar server.jar nogui");
            check(!plan.firstLaunchNeedsNetwork, "ZK8 原版：那份 jar 是完整的，装完就能离线起");
            check(!plan.note.isEmpty(), "ZK8 原版：note 说清了地址去哪问", plan.note);
        }

        // ---- Paper ----
        {
            Request request = base;
            request.flavor = Flavor::Paper;
            request.buildId = QStringLiteral("232");
            const Plan plan = planFor(request);
            check(plan.ok && plan.method == InstallMethod::DirectJar, "ZK8 Paper：能装");
            check(plan.downloads.isEmpty(),
                  "ZK8 ★ Paper：给了构建号也**拼不出地址** —— 那条地址里的对象名带 sha256"
                  "（fill-data/objects/<sha256>/...），必须先把构建列表取回来");
            check(plan.installedMarkers == QStringList{QStringLiteral("server.jar")} &&
                      plan.launchTokens == QStringList{QStringLiteral("-jar"),
                                                        QStringLiteral("server.jar"),
                                                        QStringLiteral("nogui")},
                  "ZK8 Paper：装完看 server.jar，启动参数和原版一样");
            check(plan.note.contains(QStringLiteral("paperBuildsUrl")),
                  "ZK8 Paper：note 把「地址去哪问」点出来了（界面上要照着这句说）", plan.note);
        }

        // ---- Purpur ----
        {
            Request request = base;
            request.flavor = Flavor::Purpur;
            const Plan plan = planFor(request);
            check(plan.ok && plan.downloads.size() == 1 &&
                      plan.downloads.first().url ==
                          QStringLiteral("https://api.purpurmc.org/v2/purpur/1.21.4/latest/download") &&
                      plan.downloads.first().fileName == QStringLiteral("server.jar"),
                  "ZK8 Purpur：没给构建号就下 latest 那条（落地统一叫 server.jar）",
                  plan.downloads.isEmpty() ? QString() : plan.downloads.first().url);
            Request pinned = request;
            pinned.buildId = QStringLiteral("2416");
            const Plan pinnedPlan = planFor(pinned);
            check(pinnedPlan.downloads.size() == 1 &&
                      pinnedPlan.downloads.first().url ==
                          QStringLiteral("https://api.purpurmc.org/v2/purpur/1.21.4/2416/download"),
                  "ZK8 Purpur：给了构建号就下那一条",
                  pinnedPlan.downloads.isEmpty() ? QString() : pinnedPlan.downloads.first().url);
            check(plan.launchTokens == QStringList{QStringLiteral("-jar"),
                                                   QStringLiteral("server.jar"),
                                                   QStringLiteral("nogui")} &&
                      !plan.firstLaunchNeedsNetwork,
                  "ZK8 Purpur：那份 jar 是完整的服务端，装完就能离线起");
            check(!plan.downloads.isEmpty() && plan.downloads.first().sha1.isEmpty() &&
                      plan.downloads.first().sha256.isEmpty(),
                  "ZK8 ★ Purpur：两个校验字段都是空的（上游只在 latest 那条回复里给一个 md5）"
                  "—— 下载层要能接受「这一条没有校验值」，不能拿「没校验」当失败");
        }

        // ---- Fabric ----
        {
            Request request = base;
            request.flavor = Flavor::Fabric;
            const Plan plan = planFor(request);
            check(plan.ok && plan.downloads.size() == 1 &&
                      plan.downloads.first().url ==
                          QStringLiteral("https://meta.fabricmc.net/v2/versions/loader/"
                                         "1.21.4/0.16.9/1.0.1/server/jar"),
                  "ZK8 Fabric：那条三段都带的地址",
                  plan.downloads.isEmpty() ? QString() : plan.downloads.first().url);
            check(plan.downloads.size() == 1 &&
                      plan.downloads.first().fileName == QStringLiteral("fabric-server-launch.jar") &&
                      plan.downloads.first().size < 0,
                  "ZK8 ★ Fabric：落地叫 fabric-server-launch.jar；上游那条是 chunked，**没有大小**"
                  " ⇒ 进度只能报「已下多少」，算不出百分比");
            check(plan.launchTokens == QStringList{QStringLiteral("-jar"),
                                                   QStringLiteral("fabric-server-launch.jar"),
                                                   QStringLiteral("nogui")},
                  "ZK8 Fabric：启动参数");
            check(plan.firstLaunchNeedsNetwork,
                  "ZK8 ★ Fabric：下下来那份是**启动器**不是服务端，第一次跑要联网把库拉下来"
                  "（第一次跑下 8 个 loader 库 + 把原版 bundler jar "
                  "本地解开；第二次跑一条下载都没有 —— 落地名和字节都没被改过）");
            check(plan.method == InstallMethod::DirectJar,
                  "ZK8 Fabric：虽然第一次要联网，但对用户来说就是「下下来直接跑」"
                  "（不是 Forge 那种「先跑安装器」）");
        }

        // ---- Forge ----
        {
            Request request = base;
            request.flavor = Flavor::Forge;
            request.loaderVersion = QStringLiteral("47.4.10");
            const Plan plan = planFor(request);
            check(plan.ok && plan.method == InstallMethod::RunInstaller,
                  "ZK8 Forge：能装，装法是跑安装器");
            check(plan.downloads.size() == 1 &&
                      plan.downloads.first().url ==
                          LoaderMeta::forgeInstallerUrl(QStringLiteral("1.21.4"),
                                                        QStringLiteral("47.4.10")),
                  "ZK8 Forge：下的是 LoaderMeta 那条 maven 直链（不另拼一条 —— 两条迟早会漂开）",
                  plan.downloads.isEmpty() ? QString() : plan.downloads.first().url);
            check(plan.toolArguments == QStringList{QStringLiteral("--installServer"),
                                                    QStringLiteral(".")},
                  "ZK8 Forge：客户端那条路发的是 --installClient，服务端这条换成 --installServer",
                  plan.toolArguments.join(QLatin1Char(' ')));
            check(plan.toolRunDirectory.isEmpty(),
                  "ZK8 Forge：就在安装目录里跑（工具参数里那个 . 就是它）");
            check(plan.installedMarkers ==
                      QStringList{QStringLiteral("libraries/net/minecraftforge/forge/"
                                                 "1.21.4-47.4.10/win_args.txt"),
                                  QStringLiteral("user_jvm_args.txt")},
                  "ZK8 Forge：装完必须看到的那两张参数文件（路径是安装器自己定的，我们照着算）",
                  plan.installedMarkers.join(QStringLiteral(" | ")));
            check(plan.launchTokens ==
                      QStringList{QStringLiteral("@user_jvm_args.txt"),
                                  QStringLiteral("@libraries/net/minecraftforge/forge/"
                                                 "1.21.4-47.4.10/win_args.txt"),
                                  QStringLiteral("nogui")},
                  "ZK8 ★ Forge：入口不是 jar 是这两张 @ 参数文件；我们也不跑它的 run.bat"
                  "（内存要由实例设置说了算，就得自己拼这条命令）",
                  plan.launchTokens.join(QLatin1Char(' ')));
            check(!plan.launchTokens.isEmpty() &&
                      plan.launchTokens.first() == QStringLiteral("@user_jvm_args.txt") &&
                      plan.launchTokens.last() == QStringLiteral("nogui"),
                  "ZK8 ★ Forge：`@user_jvm_args.txt` 排在**第一个**，摆的顺序本身就是给驱动层"
                  "看的 —— 它把内存那几项接在**末尾**才盖得住那张文件里的 -Xmx"
                  "（同一个参数以最后出现的为准；接在前面，界面上那根内存滑杆就成了摆设）");
            check(!plan.firstLaunchNeedsNetwork,
                  "ZK8 Forge：库是**装的时候**下完的，装完就能离线起");
        }

        // ---- NeoForge ----
        {
            Request request = base;
            request.flavor = Flavor::NeoForge;
            request.loaderVersion = QStringLiteral("21.1.251");
            const Plan plan = planFor(request);
            check(plan.ok &&
                      plan.downloads.size() == 1 &&
                      plan.downloads.first().url ==
                          LoaderMeta::neoforgeInstallerUrl(QStringLiteral("21.1.251")),
                  "ZK8 NeoForge：安装器地址也复用 LoaderMeta",
                  plan.downloads.isEmpty() ? QString() : plan.downloads.first().url);
            check(plan.installedMarkers ==
                      QStringList{QStringLiteral("libraries/net/neoforged/neoforge/"
                                                 "21.1.251/win_args.txt"),
                                  QStringLiteral("user_jvm_args.txt")},
                  "ZK8 ★ NeoForge 那张参数文件的路径和 Forge **不一样**（没有游戏版本那一段："
                  "net/neoforged/neoforge/<版本>/），照 Forge 抄必错",
                  plan.installedMarkers.join(QStringLiteral(" | ")));
            check(plan.launchTokens ==
                      QStringList{QStringLiteral("@user_jvm_args.txt"),
                                  QStringLiteral("@libraries/net/neoforged/neoforge/"
                                                 "21.1.251/win_args.txt"),
                                  QStringLiteral("nogui")},
                  "ZK8 NeoForge：启动参数", plan.launchTokens.join(QLatin1Char(' ')));
            check(plan.toolArguments == QStringList{QStringLiteral("--installServer"),
                                                    QStringLiteral(".")},
                  "ZK8 NeoForge：装法和 Forge 同一句话");
        }

        // ---- Quilt ----
        {
            Request request = base;
            request.flavor = Flavor::Quilt;
            request.loaderVersion = QStringLiteral("0.27.1");
            request.installerVersion = QStringLiteral("0.15.1");
            const Plan plan = planFor(request);
            check(plan.ok && plan.method == InstallMethod::RunInstaller, "ZK8 Quilt：能装");
            check(plan.downloads.size() == 1 &&
                      plan.downloads.first().url ==
                          QStringLiteral("https://maven.quiltmc.org/repository/release/org/quiltmc/"
                                         "quilt-installer/0.15.1/quilt-installer-0.15.1.jar"),
                  "ZK8 Quilt：下的是安装器 jar");
            check(plan.toolArguments == QStringList{QStringLiteral("install"),
                                                    QStringLiteral("server"),
                                                    QStringLiteral("1.21.4"),
                                                    QStringLiteral("0.27.1"),
                                                    QStringLiteral("--install-dir=."),
                                                    QStringLiteral("--download-server")},
                  "ZK8 Quilt：install server <游戏版本> <加载器版本> --install-dir=<目录> "
                  "--download-server",
                  plan.toolArguments.join(QLatin1Char(' ')));
            check(plan.toolArguments.contains(QStringLiteral("--install-dir=.")) &&
                      !plan.toolArguments.contains(QStringLiteral("--install-dir")),
                  "ZK8 ★ Quilt 那个参数必须 `=` 连写成一段（分段写它不认，原话是 "
                  "`Option \"--install-dir\" must specify a value`）");
            check(plan.toolArguments.contains(QStringLiteral("--download-server")),
                  "ZK8 ★★ Quilt 必须带上 `--download-server`：不带的话装完的目录里**没有 "
                  "server.jar**，而它那份启动器**不会**自己去补 —— 会直接退出，"
                  "原话 `The Minecraft server .JAR is missing (…\\server.jar)!`，"
                  "用户看到的症状就是「装好了，一双击窗口闪一下没了」");
            check(plan.installedMarkers ==
                      QStringList{QStringLiteral("quilt-server-launch.jar"),
                                  QStringLiteral("server.jar")} &&
                      plan.launchTokens == QStringList{QStringLiteral("-jar"),
                                                       QStringLiteral("quilt-server-launch.jar"),
                                                       QStringLiteral("nogui")},
                  "ZK8 Quilt：产物 quilt-server-launch.jar，启动参数",
                  plan.installedMarkers.join(QStringLiteral(" | ")));
            check(plan.installedMarkers.contains(QStringLiteral("server.jar")),
                  "ZK8 ★ Quilt：「装好了没有」必须**连 server.jar 一起看**：只有 "
                  "quilt-server-launch.jar 的话安装器是跑完了，可那个目录还起不来"
                  "（缺游戏 jar 时它一句话不说就退出）");

            Request loose = request;
            loose.loaderVersion.clear();
            const Plan loosePlan = planFor(loose);
            check(loosePlan.ok && !loosePlan.toolArguments.contains(QStringLiteral("0.27.1")) &&
                      loosePlan.toolArguments.contains(QStringLiteral("--install-dir=.")) &&
                      loosePlan.toolArguments.contains(QStringLiteral("--download-server")),
                  "ZK8 Quilt：加载器版本可以不给（不给就是它自己挑最新那个），"
                  "两个选项一个不少",
                  loosePlan.toolArguments.join(QLatin1Char(' ')));
            check(!plan.firstLaunchNeedsNetwork,
                  "ZK8 ★★ Quilt 第一次启动**不联网**（44 条 Unpacking、"
                  "0 条下载 —— loader 的库是**安装器**下的，原版那部分是从本地 bundler jar "
                  "解开的）。★ "
                  "两档在这件事上**不一样**，别互相抄");
        }

        // ---- Spigot ----
        {
            Request request = base;
            request.flavor = Flavor::Spigot;
            const Plan plan = planFor(request);
            check(plan.ok && plan.method == InstallMethod::BuildRequired,
                  "ZK8 Spigot：能装，装法是现编一份");
            check(plan.downloads.size() == 1 &&
                      plan.downloads.first().url == spigotBuildToolsUrl() &&
                      plan.downloads.first().fileName == QStringLiteral("BuildTools.jar"),
                  "ZK8 Spigot：先下 BuildTools");
            check(plan.toolArguments == QStringList{QStringLiteral("--rev"),
                                                    QStringLiteral("1.21.4")},
                  "ZK8 Spigot：--rev <游戏版本>", plan.toolArguments.join(QLatin1Char(' ')));
            check(plan.toolRunDirectory == QStringLiteral("BuildTools"),
                  "ZK8 Spigot：在子目录里编 —— BuildTools 会铺一整套 git 仓库和中间产物，"
                  "直接铺在服务端目录里那个目录就废了");
            check(plan.installedMarkers.isEmpty() && plan.launchTokens.isEmpty(),
                  "ZK8 ★ Spigot：产物名带 -R0.1-SNAPSHOT，**拼不出来** ⇒ 这两样都空着，"
                  "靠 pickSpigotJar 现场扫（缩回去自己编一个名字就是错的）");
            check(plan.firstLaunchNeedsNetwork, "ZK8 Spigot：编的过程中要联网（git + maven）");
            check(plan.note.contains(QStringLiteral("BuildTools")) &&
                      plan.note.contains(QStringLiteral("spigot-")),
                  "ZK8 Spigot：note 说清了产物在子目录里要搬上来、名字要扫", plan.note);
        }
    }

    section("ZK9. 缺参数：一句能直接给用户看的话");
    {
        using namespace ServerInstall;
        const auto planWith = [](Flavor flavor, const QString &game, const QString &loader,
                                 const QString &installer) {
            Request request;
            request.flavor = flavor;
            request.gameVersion = game;
            request.loaderVersion = loader;
            request.installerVersion = installer;
            return planFor(request);
        };

        // 八档全试一遍"没给游戏版本" —— 一档都不许漏（原版也要：它的服务端在版本 json 里）
        QStringList missingGame;
        for (Flavor flavor : allFlavors()) {
            const Plan plan =
                planWith(flavor, QString(), QStringLiteral("1.0"), QStringLiteral("1.0"));
            if (plan.ok || plan.problem != QStringLiteral("请选择游戏版本")) {
                missingGame.append(flavorToken(flavor));
            }
        }
        check(missingGame.isEmpty(), "ZK9 八档全都：没给游戏版本就不 ok，而且那句话说的是这个",
              missingGame.join(QStringLiteral(", ")));

        {
            const Plan plan = planWith(Flavor::Forge, QStringLiteral("1.21.4"), QString(), QString());
            check(!plan.ok && plan.problem == QStringLiteral("请选择 Forge 版本"),
                  "ZK9 Forge 缺加载器版本", plan.problem);
            check(plan.downloads.isEmpty() && plan.toolArguments.isEmpty() &&
                      plan.installedMarkers.isEmpty() && plan.launchTokens.isEmpty(),
                  "ZK9 不 ok 的那份计划里别的字段一个都不填（省得驱动层拿着半份计划往下走）");
        }
        {
            const Plan plan =
                planWith(Flavor::NeoForge, QStringLiteral("1.21.4"), QString(), QString());
            check(!plan.ok && plan.problem.contains(QStringLiteral("NeoForge")),
                  "ZK9 NeoForge 缺加载器版本时说的是它自己的名字（不是笼统的「加载器」）",
                  plan.problem);
        }
        {
            const Plan plan = planWith(Flavor::Fabric, QStringLiteral("1.21.4"),
                                       QStringLiteral("0.16.9"), QString());
            check(!plan.ok && plan.problem.contains(QStringLiteral("安装器版本")),
                  "ZK9 ★ Fabric 缺安装器版本是**单独一条**错 —— 那条地址少一段就是 404，"
                  "报成「没挑加载器」会把用户指到错的地方去",
                  plan.problem);
        }
        {
            const Plan plan = planWith(Flavor::Fabric, QStringLiteral("1.21.4"), QString(), QString());
            check(!plan.ok && plan.problem.contains(QStringLiteral("加载器")),
                  "ZK9 Fabric 两样都缺时报的是更靠前的那一步（加载器版本）", plan.problem);
        }
        {
            const Plan plan = planWith(Flavor::Quilt, QStringLiteral("1.21.4"),
                                       QStringLiteral("0.27.1"), QString());
            check(!plan.ok && plan.problem.contains(QStringLiteral("安装器版本")),
                  "ZK9 Quilt 缺安装器版本", plan.problem);
        }

        // 不需要加载器 / 安装器的那几档：一个游戏版本就够
        check(planWith(Flavor::Vanilla, QStringLiteral("1.21.4"), QString(), QString()).ok,
              "ZK9 原版只要一个游戏版本");
        check(planWith(Flavor::Paper, QStringLiteral("1.21.4"), QString(), QString()).ok,
              "ZK9 Paper 不要加载器也不要安装器（构建号不给就是最新那条）");
        check(planWith(Flavor::Spigot, QStringLiteral("1.21.4"), QString(), QString()).ok,
              "ZK9 Spigot 只要一个游戏版本");
    }

    section("ZK10. 那些「拼不出来」的：原版的服务端、Spigot 的产物名");
    {
        using namespace ServerInstall;
        // ---- 原版：地址在版本 json 的 downloads.server 里 ----
        const QJsonObject versionJson{
            {QStringLiteral("id"), QStringLiteral("1.21.4")},
            {QStringLiteral("downloads"),
             QJsonObject{
                 {QStringLiteral("server"),
                  QJsonObject{
                      {QStringLiteral("url"),
                       QStringLiteral("https://piston-data.mojang.com/v1/objects/abc/server.jar")},
                      {QStringLiteral("sha1"), QStringLiteral("d0ae1e2e1b9f1e0a")},
                      {QStringLiteral("size"), 55916448}}}}}};
        const Download jar = vanillaServerDownload(versionJson);
        check(jar.ok() && jar.fileName == QStringLiteral("server.jar") &&
                  jar.url.endsWith(QStringLiteral("/server.jar")) &&
                  jar.sha1 == QStringLiteral("d0ae1e2e1b9f1e0a") && jar.size == 55916448,
              "ZK10 原版：三个字段全从 json 里读（它那条**有** size，所以进度是算得出百分比的；"
              "Fabric 那条没有 —— 同一个下载器要能同时伺候这两种）",
              jar.url);
        check(!vanillaServerDownload(
                   QJsonObject{{QStringLiteral("id"), QStringLiteral("1.0")}})
                   .ok(),
              "ZK10 原版：json 里没有 downloads.server = 这个版本没有官方服务端"
              "（远古版本确实没有）");
        check(!vanillaServerDownload(QJsonObject()).ok(), "ZK10 原版：空 json 也不崩");
        check(!vanillaServerDownload(
                   QJsonObject{{QStringLiteral("downloads"),
                                QJsonObject{{QStringLiteral("server"),
                                             QJsonObject{{QStringLiteral("sha1"),
                                                          QStringLiteral("abc")}}}}}})
                   .ok(),
              "ZK10 原版：只有 sha1 没有 url 的那一条不算（ok() 判的是 url）");

        // ---- Spigot：产物名扫出来 ----
        const QStringList files{QStringLiteral("BuildTools.jar"),
                                QStringLiteral("spigot-1.21.4-R0.1-SNAPSHOT.jar"),
                                QStringLiteral("apache-maven-3.9.6"), QStringLiteral("work"),
                                QStringLiteral("spigot.jar")};
        check(pickSpigotJar(files) == QStringLiteral("spigot-1.21.4-R0.1-SNAPSHOT.jar"),
              "ZK10 Spigot：从一堆东西里扫出那个产物", pickSpigotJar(files));
        check(pickSpigotJar(QStringList{QStringLiteral("BuildTools.jar"), QStringLiteral("work")})
                  .isEmpty(),
              "ZK10 Spigot：没编出来就是空 —— 别把 BuildTools.jar 自己当成产物");
        check(pickSpigotJar(QStringList{QStringLiteral("spigot.jar")}).isEmpty(),
              "ZK10 Spigot：`spigot.jar` 不算（上游产物名一定带 `spigot-` 加版本后缀）；"
              "认宽了会把用户自己改名的那份当成刚编出来的");
        check(pickSpigotJar(QStringList()).isEmpty(), "ZK10 Spigot：空目录回空");
    }

    // =====================================================================
    // ZL. 世界设置那条路：NbtTree（判据层）+ WorldFiles 的读写
    // =====================================================================
    //
    // 要求：「世界根据其存档文件在存档设置（那支笔）里面编辑游戏规则等，打开之前弹出
    // 坏档警告并提供备份按钮」。这一节量的是**判据那一半**：一棵树怎么读怎么写、"坏档"
    // 是谁说的、改一格会不会动到别的字节。界面上那一半（那支笔、那一页、那一句警告）在
    // mw_check 的 T27 那一节。
    //
    // NOTE: 树是**手工搭**出来的，不是从某个真存档里抄的。理由是这个模块的价值全在角落上
    // ——"长度前缀越界""多出来一截""嵌套太深""改一格别的地方一个字节不变"，随便找一
    // 个真存档都撞不上这些；而撞得上它们的那些输入（一个被写坏的世界）恰恰是这一整
    // 条路存在的原因。
    section("ZL1. NbtTree：手工搭一棵树，写出去再读回来一个字节都不差");
    {
        const auto field = [](const QString &name, const NbtTree::Node &node) {
            NbtTree::Field item;
            item.first = name;
            item.second = node;
            return item;
        };
        const auto number = [](NbtTree::Type type, qint64 value) {
            NbtTree::Node node;
            node.type = type;
            node.number = value;
            return node;
        };
        const auto real = [](NbtTree::Type type, double value) {
            NbtTree::Node node;
            node.type = type;
            node.real = value;
            return node;
        };
        const auto string = [](const QString &value) {
            NbtTree::Node node;
            node.type = NbtTree::Type::String;
            node.text = value;
            return node;
        };
        const auto compound = [](const NbtTree::FieldList &fields) {
            NbtTree::Node node;
            node.type = NbtTree::Type::Compound;
            node.fields = fields;
            return node;
        };

        // 世界名里塞两样"普通字符串不会碰到"的东西：
        // - 一个补充平面上的字（U+1F600）：NBT 的字符串是**改过的 UTF-8**，这一档要写
        // 成 CESU-8 的那对代理项，写错的表现是"存完这一份，游戏读出来是个乱码"；
        // - 一个真正的 NUL（U+0000）：它必须写成 `C0 80`，直接写 `00` 的话长度前缀还
        // 对得上、但游戏那边的读取会在那儿断掉。
        const char32_t trickyChars[4] = {U'我', U'\0', U'\U0001F600', U'界'};
        const QString trickyName = QString::fromUcs4(trickyChars, 4);

        NbtTree::Document document;
        document.rootName = QString();  // `level.dat` 的根是**无名**的 Compound
        document.root.type = NbtTree::Type::Compound;

        NbtTree::Node emptyList;
        emptyList.type = NbtTree::Type::List;
        emptyList.itemType = NbtTree::Type::Byte;  // 空表**也带着**它那一档的类型

        NbtTree::Node byteArray;
        byteArray.type = NbtTree::Type::ByteArray;
        byteArray.bytes = QByteArray("\x00\x01\xff", 3);

        NbtTree::Node intArray;
        intArray.type = NbtTree::Type::IntArray;
        intArray.ints = QList<qint32>{1, -2, 3};

        NbtTree::Node longArray;
        longArray.type = NbtTree::Type::LongArray;
        longArray.longs = QList<qint64>{1, -2};

        NbtTree::Node listOfCompound;
        listOfCompound.type = NbtTree::Type::List;
        listOfCompound.itemType = NbtTree::Type::Compound;
        listOfCompound.items.append(compound(NbtTree::FieldList{
            field(QStringLiteral("X"), number(NbtTree::Type::Int, 5))}));

        const NbtTree::FieldList data{
            field(QStringLiteral("LevelName"), string(trickyName)),
            field(QStringLiteral("LastPlayed"), number(NbtTree::Type::Long, 1758960000000LL)),
            field(QStringLiteral("DataVersion"), number(NbtTree::Type::Int, 3465)),
            field(QStringLiteral("Difficulty"), number(NbtTree::Type::Byte, 2)),
            field(QStringLiteral("allowCommands"), number(NbtTree::Type::Byte, 1)),
            field(QStringLiteral("hardcore"), number(NbtTree::Type::Byte, 0)),
            field(QStringLiteral("半个浮点"), real(NbtTree::Type::Float, 0.5)),
            field(QStringLiteral("十分之一"), real(NbtTree::Type::Double, 0.1)),
            field(QStringLiteral("字节数组"), byteArray),
            field(QStringLiteral("整数数组"), intArray),
            field(QStringLiteral("长整数数组"), longArray),
            field(QStringLiteral("空表"), emptyList),
            field(QStringLiteral("一个复合元素"), listOfCompound),
            field(QStringLiteral("Version"), compound(NbtTree::FieldList{
                      field(QStringLiteral("Name"), string(QStringLiteral("1.20.1"))),
                      field(QStringLiteral("Id"), number(NbtTree::Type::Int, 3465))})),
            field(QStringLiteral("GameRules"), compound(NbtTree::FieldList{
                      field(QStringLiteral("keepInventory"), string(QStringLiteral("true"))),
                      field(QStringLiteral("randomTickSpeed"), string(QStringLiteral("3"))),
                      field(QStringLiteral("doDaylightCycle"), string(QStringLiteral("false")))})),
            field(QStringLiteral("空复合"), compound(NbtTree::FieldList())),
        };
        document.root.fields = NbtTree::FieldList{field(QStringLiteral("Data"), compound(data))};

        const QByteArray bytes = NbtTree::serialize(document);
        check(!bytes.isEmpty(), "ZL1 一棵树写得出来字节");

        NbtTree::Document back;
        QString why;
        check(NbtTree::parse(bytes, &back, &why), "ZL1 再读回来不报错", why);
        // 这一条是这个模块的**地基**：写出去 → 读回来 → 再写出去，两次的字节必须一样。
        // 它是"改一格别的地方一个字节不变"那条保证的来源（改完写回去的树就是从盘上那份
        // 读出来的同一棵，只有被改的那一格不一样）。
        check(NbtTree::serialize(back) == bytes,
              "★★ ZL1 读回来的那棵树再写出去，和原来那串字节**逐字节相同**");

        const NbtTree::Node *root = &back.root;
        check(root->type == NbtTree::Type::Compound, "ZL1 根是一张表");
        const NbtTree::Node *dataBack = NbtTree::descend(back.root, QStringLiteral("Data"));
        check(dataBack != nullptr && dataBack->type == NbtTree::Type::Compound,
              "ZL1 路径 descend(Data) 走到那张表上");
        check(NbtTree::descend(back.root, QStringLiteral("Data/Version/Name")) != nullptr,
              "ZL1 两段的路径也走得通（Version/Name）");
        check(NbtTree::descend(back.root, QStringLiteral("Data/没有这一格")) == nullptr,
              "ZL1 不存在的路径回 nullptr");
        check(NbtTree::descend(back.root, QStringLiteral("Data/LevelName/再往下")) == nullptr,
              "ZL1 走到一个**不是表**的格子上也回 nullptr（不硬往下钻）");

        // 每一项都按名字取一遍（顺序也一起钉住：NBT 的表是保序的，界面上的规则表就是按它排的）
        if (dataBack != nullptr) {
            QStringList names;
            for (const NbtTree::Field &item : dataBack->fields) {
                names << item.first;
            }
            check(names == QStringList({QStringLiteral("LevelName"), QStringLiteral("LastPlayed"),
                                        QStringLiteral("DataVersion"), QStringLiteral("Difficulty"),
                                        QStringLiteral("allowCommands"), QStringLiteral("hardcore"),
                                        QStringLiteral("半个浮点"), QStringLiteral("十分之一"),
                                        QStringLiteral("字节数组"), QStringLiteral("整数数组"),
                                        QStringLiteral("长整数数组"), QStringLiteral("空表"),
                                        QStringLiteral("一个复合元素"), QStringLiteral("Version"),
                                        QStringLiteral("GameRules"), QStringLiteral("空复合")}),
                  "★★ ZL1 项的顺序就是写进去的顺序（改完存回盘上，游戏的读取顺序也不变）",
                  names.join(QLatin1Char('/')));

            const NbtTree::Node *name = NbtTree::field(*dataBack, QStringLiteral("LevelName"));
            check(name != nullptr && name->text == trickyName,
                  "★★ ZL1 名字里那个补充平面的字和一个真 NUL 都原样回来了"
                  "（改过的 UTF-8：CESU-8 代理项 + `C0 80`）");
            if (name != nullptr) {
                check(!name->text.contains(QChar(0)) == false,
                      "ZL1 那个 NUL 还在（不是被截掉的半截字符串）");
            }
            const NbtTree::Node *played = NbtTree::field(*dataBack, QStringLiteral("LastPlayed"));
            check(played != nullptr && played->type == NbtTree::Type::Long &&
                      played->number == 1758960000000LL,
                  "ZL1 Long 原样");
            const NbtTree::Node *half = NbtTree::field(*dataBack, QStringLiteral("半个浮点"));
            check(half != nullptr && half->type == NbtTree::Type::Float && half->real == 0.5,
                  "ZL1 float 按位搬过去（不是转成 double 再转回来）");
            const NbtTree::Node *tenth = NbtTree::field(*dataBack, QStringLiteral("十分之一"));
            check(tenth != nullptr && tenth->real == 0.1,
                  "ZL1 double 也是按位搬的（0.1 这种二进制除不尽的照样精确）");
            const NbtTree::Node *blob = NbtTree::field(*dataBack, QStringLiteral("字节数组"));
            check(blob != nullptr && blob->bytes == QByteArray("\x00\x01\xff", 3),
                  "ZL1 字节数组里的 0x00 / 0xff 没被当成结束符吃掉");
            const NbtTree::Node *ints = NbtTree::field(*dataBack, QStringLiteral("整数数组"));
            check(ints != nullptr && ints->ints == QList<qint32>({1, -2, 3}), "ZL1 整数数组连负数一起原样");
            const NbtTree::Node *longs = NbtTree::field(*dataBack, QStringLiteral("长整数数组"));
            check(longs != nullptr && longs->longs == QList<qint64>({1, -2}), "ZL1 长整数数组原样");
            const NbtTree::Node *empty = NbtTree::field(*dataBack, QStringLiteral("空表"));
            // 空列表的**元素类型**必须留着：靠"猜第一个元素的类型"来写的话，空表一律变成
            // TAG_End，游戏读出来是两个不一样的东西。
            check(empty != nullptr && empty->type == NbtTree::Type::List &&
                      empty->items.isEmpty() && empty->itemType == NbtTree::Type::Byte,
                  "★★ ZL1 空列表的元素类型留住了（空表不是「没有类型」）");
            const NbtTree::Node *nested = NbtTree::field(*dataBack, QStringLiteral("一个复合元素"));
            check(nested != nullptr && nested->itemType == NbtTree::Type::Compound &&
                      nested->items.size() == 1,
                  "ZL1 表里套表（列表元素是复合）也回来了");
            check(NbtTree::descend(back.root, QStringLiteral("Data/空复合")) != nullptr,
                  "ZL1 一张空复合还在（不是被当成「读到尾巴」删掉）");
        }
        check(NbtTree::typeName(NbtTree::Type::Compound) == QStringLiteral("复合"),
              "ZL1 类型的中文名来自唯一出处", NbtTree::typeName(NbtTree::Type::Compound));
    }

    section("ZL2. NbtTree：解不动的那些，一颗子树都不留");
    {
        // 哨兵：失败时**不许动** out（调用方原来拿着一份就还是那一份）。
        NbtTree::Document sentinel;
        sentinel.rootName = QStringLiteral("哨兵");
        sentinel.root.type = NbtTree::Type::Compound;
        sentinel.root.fields = NbtTree::FieldList{};
        QString why;

        check(!NbtTree::parse(QByteArray(), &sentinel, &why), "ZL2 空的字节流解不动");
        check(!why.isEmpty(), "ZL2 而且给了一句原因", why);
        check(sentinel.rootName == QStringLiteral("哨兵"), "ZL2 失败时 out 一个字段都没被碰");

        // 一个"根是空表"的最小合法树：`0A 00 00 00`
        const QByteArray minimal("\x0A\x00\x00\x00", 4);
        NbtTree::Document small;
        check(NbtTree::parse(minimal, &small, &why), "ZL2 最小的那棵树（根是一张空表）解得动", why);

        check(!NbtTree::parse(minimal.left(3), &sentinel, &why), "ZL2 少一个字节就解不动（截断）");
        check(!NbtTree::parse(minimal + QByteArray("\x00", 1), &sentinel, &why),
              "ZL2 末尾多出来一截也解不动（不许「读到我认识的地方就停」）");

        // 类型字节不认识：`0D` 不在 0~12 里。
        check(!NbtTree::parse(QByteArray("\x0D\x00\x00\x00", 4), &sentinel, &why),
              "ZL2 类型字节不认识（13）当场拒绝");
        check(why.contains(QStringLiteral("13")) || !why.isEmpty(),
              "ZL2 而且那句话里点了是哪一个类型字节", why);

        // 长度前缀越界：一张表里一格 String，名字叫 a，正文说 100 个字节但只跟了 3 个。
        QByteArray overrun;
        overrun.append("\x0A\x00\x00", 3);          // 根：Compound，无名
        overrun.append("\x08\x00\x01", 3);          // String，名字长 1
        overrun.append("a", 1);                     // 名字
        overrun.append("\x00\x64", 2);              // 正文自称 100 字节
        overrun.append("abc", 3);                   // 只有 3 个
        overrun.append("\x00", 1);                  // 表收尾
        check(!NbtTree::parse(overrun, &sentinel, &why),
              "★★ ZL2 长度前缀比剩下的字节多 ⇒ 拒绝（不许照着这个数去分配内存）");

        // 负数项数：一张表，一格 List，元素类型 Byte，项数 0xFFFFFFFF。
        QByteArray negative;
        negative.append("\x0A\x00\x00", 3);
        negative.append("\x09\x00\x01", 3);         // List，名字长 1
        negative.append("l", 1);
        negative.append("\x01", 1);                 // 元素类型 Byte
        negative.append("\xFF\xFF\xFF\xFF", 4);     // 项数 = -1
        negative.append("\x00", 1);
        check(!NbtTree::parse(negative, &sentinel, &why),
              "★★ ZL2 负数项数当场拒绝（不拿它去 reserve 一大片）");

        // 嵌套太深：一条 100 层往里的链子。
        NbtTree::Document deep;
        deep.root.type = NbtTree::Type::Compound;
        NbtTree::Node *cursor = &deep.root;
        for (int i = 0; i < 100; ++i) {
            NbtTree::Field item;
            item.first = QStringLiteral("下一层");
            item.second.type = NbtTree::Type::Compound;
            cursor->fields.append(item);
            cursor = &cursor->fields.last().second;
        }
        check(!NbtTree::parse(NbtTree::serialize(deep), &sentinel, &why),
              "★★ ZL2 嵌套超过 kMaxDepth（64）拒绝 —— 递归解下去会把栈吃光", why);
        check(!NbtTree::parse(NbtTree::serialize(deep),
                              &sentinel, &why) &&
                  sentinel.rootName == QStringLiteral("哨兵"),
              "ZL2 太深的那次也没在 out 上留下半棵树");
    }

    section("ZL3. NbtTree：改一格的值（改不动就一个字节都不改）");
    {
        NbtTree::Node byte;
        byte.type = NbtTree::Type::Byte;
        byte.number = 7;
        check(NbtTree::valueText(byte) == QStringLiteral("7"), "ZL3 值印成一行字", NbtTree::valueText(byte));
        check(NbtTree::setValueText(&byte, QStringLiteral("9")), "ZL3 改得动");
        check(byte.number == 9, "ZL3 改完就是新值");
        check(!NbtTree::setValueText(&byte, QStringLiteral("abc")), "ZL3 不是一个数就不收");
        check(byte.number == 9, "★ ZL3 收不下的时候**原来的值一个字节都没动**");
        check(!NbtTree::setValueText(&byte, QStringLiteral("200")),
              "★ ZL3 超出一个 Byte 的范围（127）也收不下");
        check(byte.number == 9, "ZL3 同上，值还是没变");
        check(NbtTree::setValueText(&byte, QStringLiteral("-128")), "ZL3 下界 -128 收得下");
        check(!NbtTree::setValueText(&byte, QStringLiteral("-129")), "ZL3 再低一格就不收");

        NbtTree::Node longNode;
        longNode.type = NbtTree::Type::Long;
        check(NbtTree::setValueText(&longNode, QStringLiteral("9223372036854775807")),
              "ZL3 Long 的最大值收得下");
        check(!NbtTree::setValueText(&longNode, QStringLiteral("9223372036854775808")),
              "ZL3 再大一位收不下（不是悄悄截成最大值）");

        NbtTree::Node text;
        text.type = NbtTree::Type::String;
        text.text = QStringLiteral("原来那句");
        // NOTE: 这里要的是 70000 个**中文字**（每个 3 字节，编码之后 21 万字节 > 65535）。
        // 不能写 `QLatin1Char('字')` —— Latin-1 装不下，编译器会把它当成一个
        // 多字符常量（`-Wmultichar` + `-Woverflow`），char 里只剩最后一个字节。
        check(!NbtTree::setValueText(&text, QString(70000, QChar(0x5B57))) ,
              "★★ ZL3 编码之后超过 65535 字节的文字拒收（长度前缀是 quint16，收下就是写一份被截断的东西）");
        check(text.text == QStringLiteral("原来那句"), "ZL3 拒收之后那一格还是原来那句");
        check(NbtTree::setValueText(&text, QString()), "ZL3 空串收得下（清空是一个合法的值）");

        NbtTree::Node table;
        table.type = NbtTree::Type::Compound;
        check(!NbtTree::setValueText(&table, QStringLiteral("x")),
              "ZL3 整棵子树（表 / 列表 / 数组）不接「一行字」");
        NbtTree::Node *nullNode = nullptr;
        check(!NbtTree::setValueText(nullNode, QStringLiteral("x")), "ZL3 空指针不崩、回 false");
        check(NbtTree::integerBits(NbtTree::Type::Byte) == 8 &&
                  NbtTree::integerBits(NbtTree::Type::String) == 0,
              "ZL3 整数族的位宽（不是整数族回 0）");
        check(NbtTree::isFloatingPoint(NbtTree::Type::Double) &&
                  !NbtTree::isFloatingPoint(NbtTree::Type::Long),
              "ZL3 浮点族认得出来");
    }

    section("ZL4. NbtTree：游戏规则那一张表");
    {
        check(NbtTree::ruleKind(QStringLiteral("true")) == NbtTree::ValueKind::Bool &&
                  NbtTree::ruleKind(QStringLiteral("FALSE")) == NbtTree::ValueKind::Bool,
              "ZL4 是/否那一档大小写都认（盘上两种都真见过）");
        check(NbtTree::ruleKind(QStringLiteral("3")) == NbtTree::ValueKind::Int, "ZL4 整数认得出");
        check(NbtTree::ruleKind(QStringLiteral("-3")) == NbtTree::ValueKind::Int, "ZL4 负数也算整数");
        check(NbtTree::ruleKind(QStringLiteral("2.5")) == NbtTree::ValueKind::Text,
              "★ ZL4 小数归「别的」（游戏规则没有小数，按小数读进去游戏那边当坏值）");
        check(NbtTree::ruleKind(QString()) == NbtTree::ValueKind::Text, "ZL4 空串是「别的」");
        check(NbtTree::ruleKind(QStringLiteral(" 3")) == NbtTree::ValueKind::Int,
              "ZL4 两头有空白也算整数（读的时候就 trim）");
        check(NbtTree::ruleKind(QStringLiteral("mobGriefing")) == NbtTree::ValueKind::Text,
              "ZL4 别的字符串就是「别的」（有模组往这儿塞任意字符串）");
        check(NbtTree::valueKindName(NbtTree::ValueKind::Bool) !=
                  NbtTree::valueKindName(NbtTree::ValueKind::Int),
              "ZL4 三种形状的名字两两不同");

        bool flag = false;
        check(NbtTree::boolValue(QStringLiteral("true"), &flag) && flag, "ZL4 true 读成真");
        check(NbtTree::boolValue(QStringLiteral("False"), &flag) && !flag, "ZL4 false 读成假（不区分大小写）");
        check(!NbtTree::boolValue(QStringLiteral("是"), &flag), "ZL4 别的字认不成布尔");
        check(NbtTree::boolValue(QStringLiteral("true"), nullptr), "ZL4 out 传空也不崩");

        // ---- 一张手搭的表 ----
        NbtTree::Document document;
        document.root.type = NbtTree::Type::Compound;
        NbtTree::Node rules;
        rules.type = NbtTree::Type::Compound;
        rules.fields = NbtTree::FieldList{
            {QStringLiteral("keepInventory"), [] {
                 NbtTree::Node n;
                 n.type = NbtTree::Type::String;
                 n.text = QStringLiteral("true");
                 return n;
             }()},
            {QStringLiteral("randomTickSpeed"), [] {
                 NbtTree::Node n;
                 n.type = NbtTree::Type::String;
                 n.text = QStringLiteral("3");
                 return n;
             }()},
            // 一条**值不是字符串**的（模组会写这种东西）：它要被跳过，而不是让整张表都读不出来。
            {QStringLiteral("模组塞的"), [] {
                 NbtTree::Node n;
                 n.type = NbtTree::Type::Int;
                 n.number = 1;
                 return n;
             }()},
        };
        NbtTree::Field dataField;
        dataField.first = QStringLiteral("Data");
        dataField.second.type = NbtTree::Type::Compound;
        dataField.second.fields = NbtTree::FieldList{
            {QStringLiteral("GameRules"), rules},
        };
        document.root.fields = NbtTree::FieldList{dataField};

        QList<NbtTree::GameRule> got;
        QString why;
        check(NbtTree::gameRulesOf(document.root, &got, &why), "ZL4 取出那张表", why);
        check(got.size() == 2, "★ ZL4 值不是字符串的那一条被**跳过**（不是让整张表读不出来）",
              QString::number(got.size()));
        check(got.size() == 2 && got[0].name == QStringLiteral("keepInventory") &&
                  got[0].value == QStringLiteral("true") && got[1].name == QStringLiteral("randomTickSpeed"),
              "★ ZL4 顺序就是盘上的顺序（界面要排是界面的事）");

        // 改一条**已经存在**的。
        check(NbtTree::setGameRule(&document.root, QStringLiteral("randomTickSpeed"),
                                   QStringLiteral("42"), &why),
              "ZL4 改一条已有的规则", why);
        got.clear();
        NbtTree::gameRulesOf(document.root, &got, &why);
        check(got.size() == 2 && got[1].value == QStringLiteral("42"), "ZL4 改完读出来就是新值");

        // 不发明新的。
        const int before = NbtTree::descend(document.root, QStringLiteral("Data/GameRules")) != nullptr
                               ? NbtTree::descend(document.root, QStringLiteral("Data/GameRules"))->fields.size()
                               : -1;
        check(!NbtTree::setGameRule(&document.root, QStringLiteral("doFireTick"),
                                    QStringLiteral("false"), &why),
              "★★ ZL4 不发明新规则：名字不存在就拒绝");
        check(!why.isEmpty(), "ZL4 而且说明了是找不到这一条", why);
        const NbtTree::Node *after = NbtTree::descend(document.root, QStringLiteral("Data/GameRules"));
        check(after != nullptr && after->fields.size() == before,
              "★★ ZL4 拒绝之后表里**一条都没多出来**（用户点一下就给别人的存档塞一条，那不是编辑）");

        // 改一条值不是字符串的。
        check(!NbtTree::setGameRule(&document.root, QStringLiteral("模组塞的"),
                                    QStringLiteral("2"), &why),
              "ZL4 值不是字符串的那一条也不改（那是别人写的东西）");

        // 没有那一格的老存档。
        NbtTree::Document bare;
        bare.root.type = NbtTree::Type::Compound;
        check(!NbtTree::gameRulesOf(bare.root, &got, &why), "ZL4 没有那一格 ⇒ 取不出来");
        check(why.contains(QStringLiteral("游戏规则")), "ZL4 而且那句原因点名了是哪一格", why);
        check(!NbtTree::setGameRule(&bare.root, QStringLiteral("keepInventory"),
                                    QStringLiteral("true"), &why),
              "★ ZL4 没有那一格时也**不替它造一格**");

        // `MissingRule::Create` 那一档 —— 界面上"改一条盘上还没有的规则"
        // 走的就是它（要求「下面的世界设置不要要求进了世界才显示而是把所有的都列出来，
        // 并允许修改值」）。上面那几条量的是**默认那一档**（拒绝、一个字节都不改），
        // 这里量的是"谁要建谁自己说"。
        //
        // NOTE: 下面这几棵树都是**新造的**，不在上面那几棵上接着改 —— 在"已经被拒绝过
        // 一次"的树上量"建出来了没有"，量到的是上一次留下的样子（那确实没变，
        // 但那是搭巧合，不是判据）。
        NbtTree::Document creatable;
        creatable.root.type = NbtTree::Type::Compound;
        {
            NbtTree::Field data;
            data.first = QStringLiteral("Data");
            data.second.type = NbtTree::Type::Compound;
            NbtTree::Node rules;
            rules.type = NbtTree::Type::Compound;
            data.second.fields = NbtTree::FieldList{{QStringLiteral("GameRules"), rules}};
            creatable.root.fields = NbtTree::FieldList{data};
        }
        check(NbtTree::setGameRule(&creatable.root, QStringLiteral("doFireTick"),
                                   QStringLiteral("false"), &why, NbtTree::MissingRule::Create),
              "★★ ZL4 `MissingRule::Create`：名字不在表里就**加一条**（默认那一档是拒绝）",
              why);
        got.clear();
        check(NbtTree::gameRulesOf(creatable.root, &got, &why) && got.size() == 1 &&
                  got[0].name == QStringLiteral("doFireTick") &&
                  got[0].value == QStringLiteral("false"),
              "★★ ZL4 加进去之后读得回来，值和名字都对", why);
        {
            const NbtTree::Node *added =
                NbtTree::descend(creatable.root, QStringLiteral("Data/GameRules/doFireTick"));
            check(added != nullptr && added->type == NbtTree::Type::String,
                  "★★ ZL4 建出来的是**字符串**那一档（`GameRules` 是一张「名字 → 字符串」的"
                  "表，加进去的必须也是字符串）");
        }

        // 连 `Data/GameRules` 那一格都没有的老档（工具做出来的、从没进过一次的）：`Create`
        // 把那一格一起建出来。这正是用户那句话里「不要要求进了世界才显示」的前提。
        NbtTree::Document noTable;
        noTable.root.type = NbtTree::Type::Compound;
        {
            NbtTree::Field data;
            data.first = QStringLiteral("Data");
            data.second.type = NbtTree::Type::Compound;
            noTable.root.fields = NbtTree::FieldList{data};
        }
        check(NbtTree::setGameRule(&noTable.root, QStringLiteral("keepInventory"),
                                   QStringLiteral("true"), &why, NbtTree::MissingRule::Create),
              "★★ ZL4 连 `Data/GameRules` 那一格都没有的老档也建得出来（一个刚建好、还没被"
              "游戏写过档的世界就是这样）",
              why);
        got.clear();
        check(NbtTree::gameRulesOf(noTable.root, &got, &why) && got.size() == 1,
              "★★ ZL4 新造出来那一格是可用的（读得出来）", why);

        // 但**连 `Data` 都不是复合标签**的时候仍然一个字节都不动：这份档的形状我们看不懂，
        // 看不懂就不碰（和 `parse()` 那条规矩一致）。
        NbtTree::Document weird;
        weird.root.type = NbtTree::Type::Compound;
        {
            NbtTree::Field data;
            data.first = QStringLiteral("Data");
            data.second.type = NbtTree::Type::Int;
            weird.root.fields = NbtTree::FieldList{data};
        }
        check(!NbtTree::setGameRule(&weird.root, QStringLiteral("keepInventory"),
                                    QStringLiteral("true"), &why, NbtTree::MissingRule::Create),
              "★★ ZL4 `Data` 不是复合标签 ⇒ 连 `Create` 那一档也拒绝（形状看不懂就不动它）");

        // 有一格**空的**表：取出来成功、零条（"这个老存档还没存过规则"和"坏了"是两回事）。
        NbtTree::Document emptyTable;
        emptyTable.root.type = NbtTree::Type::Compound;
        NbtTree::Field emptyData;
        emptyData.first = QStringLiteral("Data");
        emptyData.second.type = NbtTree::Type::Compound;
        NbtTree::Node emptyRules;
        emptyRules.type = NbtTree::Type::Compound;
        emptyData.second.fields = NbtTree::FieldList{{QStringLiteral("GameRules"), emptyRules}};
        emptyTable.root.fields = NbtTree::FieldList{emptyData};
        got.clear();
        check(NbtTree::gameRulesOf(emptyTable.root, &got, &why) && got.isEmpty(),
              "ZL4 空表取出来是「成功、零条」（不是失败）");
    }

    section("ZL5. WorldFiles::readWorldInfo / applyWorldInfo（纯函数，不碰盘）");
    {
        const auto stringNode = [](const QString &value) {
            NbtTree::Node n;
            n.type = NbtTree::Type::String;
            n.text = value;
            return n;
        };
        const auto byteNode = [](qint64 value) {
            NbtTree::Node n;
            n.type = NbtTree::Type::Byte;
            n.number = value;
            return n;
        };
        const auto intNode = [](qint64 value) {
            NbtTree::Node n;
            n.type = NbtTree::Type::Int;
            n.number = value;
            return n;
        };
        const auto longNode = [](qint64 value) {
            NbtTree::Node n;
            n.type = NbtTree::Type::Long;
            n.number = value;
            return n;
        };
        const auto compound = [](const NbtTree::FieldList &fields) {
            NbtTree::Node n;
            n.type = NbtTree::Type::Compound;
            n.fields = fields;
            return n;
        };
        const auto field = [](const QString &name, const NbtTree::Node &node) {
            NbtTree::Field item;
            item.first = name;
            item.second = node;
            return item;
        };

        NbtTree::Document document;
        document.root.type = NbtTree::Type::Compound;
        NbtTree::Node version = compound(NbtTree::FieldList{
            field(QStringLiteral("Name"), stringNode(QStringLiteral("1.20.1")))});
        NbtTree::Node rules = compound(NbtTree::FieldList{
            field(QStringLiteral("keepInventory"), stringNode(QStringLiteral("false"))),
            field(QStringLiteral("randomTickSpeed"), stringNode(QStringLiteral("3")))});
        document.root.fields = NbtTree::FieldList{field(
            QStringLiteral("Data"),
            compound(NbtTree::FieldList{
                field(QStringLiteral("LevelName"), stringNode(QStringLiteral("我的世界"))),
                field(QStringLiteral("LastPlayed"), longNode(1758960000000LL)),
                field(QStringLiteral("DataVersion"), intNode(3465)),
                field(QStringLiteral("Difficulty"), byteNode(2)),
                field(QStringLiteral("allowCommands"), byteNode(0)),
                field(QStringLiteral("hardcore"), byteNode(0)),
                field(QStringLiteral("Version"), version),
                field(QStringLiteral("GameRules"), rules)}))};

        WorldFiles::WorldInfo info;
        WorldFiles::readWorldInfo(document, true, &info);
        check(info.gzipped, "ZL5 那份是 gzip 包着的（原样带出来）");
        check(info.hasLevelName && info.levelName == QStringLiteral("我的世界"), "ZL5 世界名读出来了");
        check(info.hasVersionName && info.versionName == QStringLiteral("1.20.1"), "ZL5 版本名读出来了");
        check(info.hasLastPlayed && info.lastPlayedMs == 1758960000000LL, "ZL5 最后游玩时间读出来了");
        check(info.hasDataVersion && info.dataVersion == 3465, "ZL5 存档格式号读出来了");
        check(info.hasAllowCommands && !info.allowCommands, "ZL5 允许作弊读出来了（0 = 关）");
        check(info.hasHardcore && !info.hardcore, "ZL5 极限模式读出来了");
        check(info.hasDifficulty && info.difficulty == 2, "ZL5 难度读出来了");
        check(info.hasRules && info.rules.size() == 2, "ZL5 规则表读出来了", QString::number(info.rules.size()));

        // ---- 盘上没有的项**不发明** ----
        NbtTree::Document bare;
        bare.root.type = NbtTree::Type::Compound;
        bare.root.fields = NbtTree::FieldList{
            field(QStringLiteral("Data"), compound(NbtTree::FieldList{}))};
        WorldFiles::WorldInfo bareInfo;
        WorldFiles::readWorldInfo(bare, false, &bareInfo);
        check(!bareInfo.hasLevelName && !bareInfo.hasVersionName && !bareInfo.hasLastPlayed &&
                  !bareInfo.hasDataVersion && !bareInfo.hasAllowCommands && !bareInfo.hasHardcore &&
                  !bareInfo.hasDifficulty && !bareInfo.hasRules,
              "★★ ZL5 一个字段都没有的老存档：八位全是「没有」（**不许拿默认值顶替**）");
        check(!bareInfo.gzipped, "ZL5 不 gzip 的那份也如实报");

        // ---- 形状不对的：当"没有这一项"，不猜 ----
        NbtTree::Document weird;
        weird.root.type = NbtTree::Type::Compound;
        weird.root.fields = NbtTree::FieldList{field(
            QStringLiteral("Data"),
            compound(NbtTree::FieldList{
                field(QStringLiteral("allowCommands"), byteNode(2)),   // 只有 0/1 是真值
                field(QStringLiteral("hardcore"), byteNode(-1)),
                field(QStringLiteral("Difficulty"), byteNode(7)),      // 越界的难度
                field(QStringLiteral("LevelName"), byteNode(1)),       // 世界名不是文字
                field(QStringLiteral("LastPlayed"), stringNode(QStringLiteral("昨天"))),
                field(QStringLiteral("GameRules"),
                      stringNode(QStringLiteral("这不是一张表")))}))};
        WorldFiles::WorldInfo weirdInfo;
        WorldFiles::readWorldInfo(weird, true, &weirdInfo);
        check(!weirdInfo.hasAllowCommands, "★ ZL5 allowCommands = 2 当「没有这一项」（游戏只写 0/1）");
        check(!weirdInfo.hasHardcore, "★ ZL5 hardcore = -1 同上");
        check(!weirdInfo.hasDifficulty, "★ ZL5 难度 7 越界 ⇒ 「没有这一项」（界面上不许打「普通」）");
        check(!weirdInfo.hasLevelName, "ZL5 世界名那一格不是文字 ⇒ 没有");
        check(!weirdInfo.hasLastPlayed, "ZL5 最后游玩时间不是数字 ⇒ 没有");
        check(!weirdInfo.hasRules, "ZL5 GameRules 不是一张表 ⇒ 没有规则表");

        check(WorldFiles::difficultyName(0) == QStringLiteral("和平") &&
                  WorldFiles::difficultyName(1) == QStringLiteral("简单") &&
                  WorldFiles::difficultyName(2) == QStringLiteral("普通") &&
                  WorldFiles::difficultyName(3) == QStringLiteral("困难"),
              "ZL5 四档难度的名字");
        check(WorldFiles::difficultyName(4).isEmpty() && WorldFiles::difficultyName(-1).isEmpty(),
              "★★ ZL5 越界的难度**不给名字**（打「普通」就是在撒谎）");

        // ---- 写回去 ----
        WorldFiles::WorldInfo edited = info;
        edited.levelName = QStringLiteral("改过的世界");
        edited.allowCommands = true;
        edited.hardcore = true;
        edited.difficulty = 3;
        edited.rules[1].value = QStringLiteral("42");
        QString why;
        check(WorldFiles::applyWorldInfo(&document, edited, &why),
              "ZL5 改四项 + 一条规则，全写进去了", why);

        WorldFiles::WorldInfo reread;
        WorldFiles::readWorldInfo(document, true, &reread);
        check(reread.levelName == QStringLiteral("改过的世界"), "ZL5 世界名读回来是新的");
        check(reread.allowCommands, "ZL5 允许作弊读回来是新的");
        check(reread.hardcore, "ZL5 极限模式读回来是新的");
        check(reread.difficulty == 3, "ZL5 难度读回来是新的");
        check(reread.rules.size() == 2 && reread.rules[1].name == QStringLiteral("randomTickSpeed") &&
                  reread.rules[1].value == QStringLiteral("42"),
              "ZL5 规则读回来是新的");
        check(reread.hasLastPlayed && reread.lastPlayedMs == 1758960000000LL,
              "★ ZL5 没被改的那几格原样留着（我们只碰用户在页面上动过的地方）");
        check(reread.rules.size() == 2 && reread.rules[0].value == QStringLiteral("false"),
              "★ ZL5 没改的那条规则一个字都没动");

        // 盘上没有的那一格：`hasXxx = false` 时**不许**顺手造一格出来。
        WorldFiles::WorldInfo invent = bareInfo;
        invent.hasLevelName = false;
        const int bareFields =
            NbtTree::descend(bare.root, QStringLiteral("Data")) != nullptr
                ? NbtTree::descend(bare.root, QStringLiteral("Data"))->fields.size()
                : -1;
        check(WorldFiles::applyWorldInfo(&bare, invent, &why), "ZL5 什么都没改的一趟当然成功");
        check(NbtTree::descend(bare.root, QStringLiteral("Data")) != nullptr &&
                  NbtTree::descend(bare.root, QStringLiteral("Data"))->fields.size() == bareFields,
              "★★ ZL5 什么都没改 ⇒ 一个字段都没多出来");

        {
            NbtTree::Document oddTypes;
            oddTypes.root.type = NbtTree::Type::Compound;
            oddTypes.root.fields = NbtTree::FieldList{field(
                QStringLiteral("Data"),
                compound(NbtTree::FieldList{field(QStringLiteral("allowCommands"), intNode(0)),
                                            field(QStringLiteral("LevelName"), intNode(1))}))};
            WorldFiles::WorldInfo oddInfo;
            WorldFiles::readWorldInfo(oddTypes, false, &oddInfo);
            check(oddInfo.hasAllowCommands && !oddInfo.hasLevelName,
                  "ZL5' 数值那一格读得出来（Int 也算「有这一项」），文字那一格类型不对当没有");
            WorldFiles::WorldInfo fix;
            fix.hasAllowCommands = true;
            fix.allowCommands = true;
            QString oddWhy;
            check(WorldFiles::applyWorldInfo(&oddTypes, fix, &oddWhy),
                  "★★ ZL5' 类型不对的**数值格**照样写得进去（就地换成游戏认的类型）", oddWhy);
            const NbtTree::Node *fixedNode =
                NbtTree::descend(oddTypes.root, QStringLiteral("Data/allowCommands"));
            check(fixedNode != nullptr && fixedNode->type == NbtTree::Type::Byte &&
                      fixedNode->number == 1,
                  "★★ ZL5' 写完之后是 Byte 的 1 —— 游戏那边才真读得住（不是「写成功了但没人认」）");
            WorldFiles::WorldInfo again;
            WorldFiles::readWorldInfo(oddTypes, false, &again);
            check(again.hasAllowCommands && again.allowCommands, "ZL5' 读回来也是新的");
            fix.hasLevelName = true;
            fix.levelName = QStringLiteral("改个名");
            check(!WorldFiles::applyWorldInfo(&oddTypes, fix, &oddWhy),
                  "★★ ZL5' 文字那一格类型不对 ⇒ 明说写不进去（不写一个游戏不认的值）", oddWhy);
        }

        // `MissingField::Create` 那一档 —— 界面上那四格**不再按"盘上有没有"
        // 灰掉**（要求「并允许修改值」），所以"盘上没有这一格"必须有个着落。
        // 上面那一条量的是**默认那一档**（`Refuse`，一个字段都不多）；这里量 `Create`。
        //
        // NOTE: 判据里**连类型一起量** —— 建出来一个类型错的字段比"没建"更坏：游戏读到
        // 一个 Int 的 `LevelName` 会当成没有它，而"我们刚写进去了"这句话是真的。
        {
            // 一棵 `Data` 是空复合标签的树（= 一个老存档：这八格一格都没有）。
            NbtTree::Document fresh;
            fresh.root.type = NbtTree::Type::Compound;
            fresh.root.fields =
                NbtTree::FieldList{field(QStringLiteral("Data"), compound(NbtTree::FieldList{}))};
            WorldFiles::WorldInfo wanted;  // 八位全是 false
            wanted.hasLevelName = true;
            wanted.levelName = QStringLiteral("新起的名字");
            wanted.hasAllowCommands = true;
            wanted.allowCommands = true;
            wanted.hasDifficulty = true;
            wanted.difficulty = 3;
            check(WorldFiles::applyWorldInfo(&fresh, wanted, &why,
                                             WorldFiles::MissingField::Create),
                  "★★ ZL5 `MissingField::Create`：盘上没有那几格也写得进去（老档没有 "
                  "`allowCommands` 那一格是常事 —— 它是 1.8 才加的）",
                  why);
            const NbtTree::Node *createdName =
                NbtTree::descend(fresh.root, QStringLiteral("Data/LevelName"));
            check(createdName != nullptr && createdName->type == NbtTree::Type::String &&
                      createdName->text == QStringLiteral("新起的名字"),
                  "★★ ZL5 建出来的世界名是**字符串**那一档（类型错了游戏就当没有它）");
            const NbtTree::Node *createdAllow =
                NbtTree::descend(fresh.root, QStringLiteral("Data/allowCommands"));
            check(createdAllow != nullptr && createdAllow->type == NbtTree::Type::Byte &&
                      createdAllow->number == 1,
                  "★★ ZL5 建出来的「允许作弊」是 **Byte** 那一档（0/1）");
            const NbtTree::Node *createdDiff =
                NbtTree::descend(fresh.root, QStringLiteral("Data/Difficulty"));
            check(createdDiff != nullptr && createdDiff->type == NbtTree::Type::Byte &&
                      createdDiff->number == 3,
                  "★★ ZL5 建出来的难度也是 Byte，值就是刚才那个 3");
            check(NbtTree::descend(fresh.root, QStringLiteral("Data/hardcore")) == nullptr,
                  "★★ ZL5 而 `hasXxx = false` 的那几项**一个都没建** —— `Create` 是「允许建」，"
                  "不是「见到缺的就补」");
            WorldFiles::WorldInfo roundTrip;
            WorldFiles::readWorldInfo(fresh, false, &roundTrip);
            check(roundTrip.hasLevelName && roundTrip.levelName == QStringLiteral("新起的名字") &&
                      roundTrip.hasAllowCommands && roundTrip.allowCommands &&
                      roundTrip.hasDifficulty && roundTrip.difficulty == 3 &&
                      !roundTrip.hasHardcore,
                  "★★ ZL5 写下去再读回来，和要的一模一样（往返一趟对得上）");
        }

        // 连 `Data` 都不是复合标签的时候，`Create` 那一档也一个字节都不动。
        {
            NbtTree::Document odd;
            odd.root.type = NbtTree::Type::Compound;
            odd.root.fields =
                NbtTree::FieldList{field(QStringLiteral("Data"), intNode(7))};
            WorldFiles::WorldInfo wanted;
            wanted.hasLevelName = true;
            wanted.levelName = QStringLiteral("写不进去");
            check(!WorldFiles::applyWorldInfo(&odd, wanted, &why,
                                              WorldFiles::MissingField::Create),
                  "★★ ZL5 `Data` 不是复合标签 ⇒ `Create` 那一档也拒绝（形状看不懂就不动它）");
        }
    }

    section("ZL5b. WorldFiles::knownGameRules / allGameRules（界面上那一列是**全集**）");
    {
        // 要求：「下面的世界设置不要要求进了世界才显示而是把所有的都列出来，
        // 并允许修改值」。`level.dat` 里那份 `GameRules` 常常只有几条（工具做出来的档、从没
        // 进过一次的档**一条都没有**），所以界面上那一列是拿 `knownGameRules()`（启动器认识
        // 的规则全集）并出来的。这一段量的是那张表本身**站不站得住**。
        QString why;
        const auto compoundOf = [](const NbtTree::FieldList &fields) {
            NbtTree::Node node;
            node.type = NbtTree::Type::Compound;
            node.fields = fields;
            return node;
        };
        const auto fieldOf = [](const QString &name, const NbtTree::Node &node) {
            NbtTree::Field out;
            out.first = name;
            out.second = node;
            return out;
        };
        const QList<WorldFiles::GameRuleDefault> &catalog = WorldFiles::knownGameRules();
        check(catalog.size() >= 40,
              "★★ ZL5b 那张表里有原版那几十条规则（界面上的「列出来所有的」全靠它）",
              QStringLiteral("%1 条").arg(catalog.size()));

        // 名字不许重复（**大小写不敏感**：`GameRules` 是一张表，两个只差大小写的键在
        // 界面上一模一样，用户改哪一个都不知道）；默认值不许是"认不出形状"的那种
        // （那说明表里有个手滑写歪的字，界面上会变成一格随便填的框）。
        QSet<QString> seen;
        QStringList duplicated;
        QStringList oddValues;
        for (const WorldFiles::GameRuleDefault &item : catalog) {
            if (seen.contains(item.name.toLower())) {
                duplicated.append(item.name);
            }
            seen.insert(item.name.toLower());
            if (item.name.isEmpty() || NbtTree::ruleKind(item.value) == NbtTree::ValueKind::Text) {
                oddValues.append(item.name + QLatin1Char('=') + item.value);
            }
        }
        check(duplicated.isEmpty(),
              "★★ ZL5b 表里没有重名的规则（大小写不敏感 —— 两个只差大小写的键在界面上"
              "分不出来）",
              duplicated.join(QLatin1Char('/')));
        check(oddValues.isEmpty(),
              "★★ ZL5b 每一条的默认值都**认得出形状**（true/false 或者数字）—— 不是的话"
              "界面上会变成一格随便填的框，那说明表里有个写歪的字",
              oddValues.join(QLatin1Char('/')));

        // 盘上一条都没有：摆出来的就是全集本身。
        const QList<NbtTree::GameRule> nothing = WorldFiles::allGameRules({});
        check(nothing.size() == catalog.size(),
              "★★ ZL5b 盘上一条规则都没有时，列出来的就是全集（这正是用户那句话要的："
              "不用先进游戏玩一次）",
              QStringLiteral("%1 条").arg(nothing.size()));

        // 排序：大小写不敏感地排好（界面上那一列就是照这个顺序摆的，这一层不重排）。
        const auto sortedOk = [](const QList<NbtTree::GameRule> &list) {
            for (int i = 1; i < list.size(); ++i) {
                if (QString::compare(list[i - 1].name, list[i].name, Qt::CaseInsensitive) > 0) {
                    return false;
                }
            }
            return true;
        };
        check(sortedOk(nothing), "★ ZL5b 那个顺序是排好的（大小写不敏感）");

        // 盘上有值的那一条：**盘上那份说了算**（那是这个世界的真值）。
        QList<NbtTree::GameRule> onDisk;
        {
            NbtTree::GameRule keep;
            keep.name = QStringLiteral("keepInventory");
            keep.value = QStringLiteral("true");  // 默认值是 false
            onDisk.append(keep);
        }
        const QList<NbtTree::GameRule> merged = WorldFiles::allGameRules(onDisk);
        QString mergedKeep;
        for (const NbtTree::GameRule &rule : merged) {
            if (rule.name == QStringLiteral("keepInventory")) {
                mergedKeep = rule.value;
            }
        }
        check(merged.size() == catalog.size() && mergedKeep == QStringLiteral("true"),
              "★★ ZL5b 同一个名字两边都有时**盘上那份说了算**（那是这个世界的真值）",
              mergedKeep);

        // 盘上有、全集里没有的（模组写的、比这张表还新的）：**一条都不许少**。
        QList<NbtTree::GameRule> modded;
        {
            NbtTree::GameRule rule;
            rule.name = QStringLiteral("moddedRule");
            rule.value = QStringLiteral("whatever");
            modded.append(rule);
        }
        const QList<NbtTree::GameRule> withMod = WorldFiles::allGameRules(modded);
        bool found = false;
        for (const NbtTree::GameRule &rule : withMod) {
            if (rule.name == QStringLiteral("moddedRule") && rule.value == QStringLiteral("whatever")) {
                found = true;
            }
        }
        check(withMod.size() == catalog.size() + 1 && found,
              "★★ ZL5b 盘上有、全集里没有的那几条**原样留着**（少列一条就是瞒着用户 —— "
              "他看得见 mods 里那些模组，却看不见模组写进去的规则）",
              QStringLiteral("%1 条").arg(withMod.size()));
        check(sortedOk(withMod),
              "★ ZL5b 从盘上捡回来的那一条**插进了该在的位置**（不是甩到所有小写名字后面）");

        // ---- `applyChangedGameRules()`：只写点名的那些，可以新增 ----
        {
            NbtTree::Document target;
            target.root.type = NbtTree::Type::Compound;
            target.root.fields =
                NbtTree::FieldList{
                    fieldOf(QStringLiteral("Data"), compoundOf(NbtTree::FieldList{}))};
            QList<NbtTree::GameRule> changes;
            {
                NbtTree::GameRule a;
                a.name = QStringLiteral("doDaylightCycle");
                a.value = QStringLiteral("false");
                changes.append(a);
                NbtTree::GameRule b;
                b.name = QStringLiteral("randomTickSpeed");
                b.value = QStringLiteral("7");
                changes.append(b);
            }
            check(WorldFiles::applyChangedGameRules(&target, changes, &why),
                  "★★ ZL5b `applyChangedGameRules()` 把点名的两条都写进去了（连 "
                  "`Data/GameRules` 那一格都没有的档也建得出来）",
                  why);
            QList<NbtTree::GameRule> written;
            NbtTree::gameRulesOf(target.root, &written, &why);
            check(written.size() == 2 && written[0].name == QStringLiteral("doDaylightCycle") &&
                      written[0].value == QStringLiteral("false") &&
                      written[1].value == QStringLiteral("7"),
                  "★★ ZL5b 写进去的就是点名的**那两条**（不是整张全集 —— 一份 44 条的默认值"
                  "塞进用户的存档，那不是编辑）",
                  QStringLiteral("%1 条").arg(written.size()));
            // 空的那一发是空转：一条都不写（`Delta` 为空时走的就是这条路）。
            const NbtTree::Node *rulesNode =
                NbtTree::descend(target.root, QStringLiteral("Data/GameRules"));
            check(rulesNode != nullptr && WorldFiles::applyChangedGameRules(&target, {}, &why) &&
                      rulesNode->fields.size() == 2,
                  "★ ZL5b 一条都不点名的时候是**空转**（一条都不写）");
        }
    }

    section("ZL6. readLevelData / writeLevelData：盘上那一趟（gzip 进、gzip 出）");
    {
        // ---- 先把一棵树写成一个"真"的 level.dat（走 gzip，和盘上那份一样）----
        QString fixtureProblem;
        const auto makeWorldWith = [&](const QString &dirName) {
            const QString dir = scratch.path() + QStringLiteral("/") + dirName;
            QDir().mkpath(dir);
            NbtTree::Document document;
            document.root.type = NbtTree::Type::Compound;
            NbtTree::Field data;
            data.first = QStringLiteral("Data");
            data.second.type = NbtTree::Type::Compound;
            NbtTree::Node nameNode;
            nameNode.type = NbtTree::Type::String;
            nameNode.text = QStringLiteral("原本的名字");
            NbtTree::Node diffNode;
            diffNode.type = NbtTree::Type::Byte;
            diffNode.number = 1;
            NbtTree::Node keepNode;
            keepNode.type = NbtTree::Type::String;
            keepNode.text = QStringLiteral("false");
            NbtTree::Node rulesNode;
            rulesNode.type = NbtTree::Type::Compound;
            NbtTree::Field keepRule;
            keepRule.first = QStringLiteral("keepInventory");
            keepRule.second = keepNode;
            rulesNode.fields = NbtTree::FieldList{keepRule};
            NbtTree::Field nameField;
            nameField.first = QStringLiteral("LevelName");
            nameField.second = nameNode;
            NbtTree::Field diffField;
            diffField.first = QStringLiteral("Difficulty");
            diffField.second = diffNode;
            NbtTree::Field rulesField;
            rulesField.first = QStringLiteral("GameRules");
            rulesField.second = rulesNode;
            data.second.fields = NbtTree::FieldList{nameField, diffField, rulesField};
            document.root.fields = NbtTree::FieldList{data};

            const QString plain = dir + QStringLiteral("/plain.nbt.tmp");
            writeFileAt(plain, NbtTree::serialize(document));
            const bool ok = ZipUtil::gzipFrom(plain, dir + QStringLiteral("/level.dat"), &fixtureProblem);
            QFile::remove(plain);
            return ok;
        };

        const QString dirName = QStringLiteral("世界A");
        const bool fixtureReady = makeWorldWith(dirName);
        check(fixtureReady, "ZL6 造了一个真存档（level.dat 是 gzip 的）", fixtureProblem);
        // 夹具准备失败应保留首个错误，不继续索引空规则数组制造级联段错。
        if (!fixtureReady) return;
        const QString worldDir = scratch.path() + QStringLiteral("/") + dirName;
        check(QFileInfo(WorldFiles::levelDataPath(worldDir)).exists(), "ZL6 level.dat 就在那个位置");

        WorldFiles::LevelData data;
        QString why;
        check(WorldFiles::readLevelData(worldDir, &data, &why), "ZL6 读得动", why);
        check(data.info.gzipped, "ZL6 认出来它是 gzip 的");
        check(data.info.hasLevelName && data.info.levelName == QStringLiteral("原本的名字"),
              "ZL6 读出来的名字对");
        check(data.info.hasDifficulty && data.info.difficulty == 1, "ZL6 读出来的难度对");
        check(data.info.rules.size() == 1 && data.info.rules[0].name == QStringLiteral("keepInventory"),
              "ZL6 读出来一条规则");

        // 记下**没被改的那些字节**：写回去之后要比。
        const QByteArray before = NbtTree::serialize(data.document);

        data.info.levelName = QStringLiteral("改过的名字");
        data.info.rules[0].value = QStringLiteral("true");
        check(WorldFiles::applyWorldInfo(&data.document, data.info, &why), "ZL6 改两处", why);
        check(WorldFiles::writeLevelData(worldDir, data, &why), "ZL6 写回去", why);

        // 盘上那份要能再读回来，而且形状还是 gzip 的。
        WorldFiles::LevelData back;
        check(WorldFiles::readLevelData(worldDir, &back, &why), "ZL6 写完再读得动", why);
        check(back.info.gzipped, "★★ ZL6 写回去的还是 gzip（形状跟着读进来那一份，游戏才认得）");
        check(back.info.levelName == QStringLiteral("改过的名字"), "ZL6 新名字在盘上");
        check(back.info.rules.size() == 1 && back.info.rules[0].value == QStringLiteral("true"),
              "ZL6 新规则值在盘上");

        // 这一条是"只改用户动过的格子"那条承诺的**字节级**凭据：拿改之前那棵树和盘上
        // 这一份比，除了那两格，别的地方必须一模一样。
        NbtTree::Document expected = data.document;
        check(NbtTree::serialize(back.document) == NbtTree::serialize(expected),
              "★★ ZL6 写下去的就是「那棵树改了两格」（没有多出任何一格、顺序也没动）");
        check(!before.isEmpty(), "ZL6 改之前那棵树也写得出来（对照用）");

        // ---- 临时文件不许留下 ----
        // 落盘走 `QSaveFile`，它在同一个目录里开的临时文件叫 `level.dat.XXXXXX` ——
        // 只按某个前缀扫会漏过它。所以这里**把整个目录列出来**：这个世界里除了
        // `level.dat` 一个文件都不该有，临时文件换了名字这条还是拦得住。
        QStringList leftovers =
            QDir(worldDir).entryList(QDir::Files | QDir::Hidden | QDir::System);
        leftovers.removeAll(QStringLiteral("level.dat"));
        check(leftovers.isEmpty(),
              "★★ ZL6 世界目录里除了 level.dat 一个文件都没多出来（游戏会把它们当存档内容扫）",
              leftovers.join(QLatin1Char('/')));

        // ---- 坏档：一句话就说得清，而且**不许**动盘上那份 ----
        const QString badDir = scratch.path() + QStringLiteral("/坏档");
        QDir().mkpath(badDir);
        writeFileAt(WorldFiles::levelDataPath(badDir), QByteArrayLiteral("这不是 gzip，也不是 NBT"));
        WorldFiles::LevelData bad;
        bad.info.levelName = QStringLiteral("哨兵");
        check(!WorldFiles::readLevelData(badDir, &bad, &why), "★★ ZL6 一个坏文件读不动（这就是「坏档」的判据）");
        check(!why.isEmpty(), "ZL6 而且给了一句给人看的原因", why);
        check(bad.info.levelName == QStringLiteral("哨兵"),
              "ZL6 读失败时 out **一个字段都没被碰**（调用方拿着的那份还是那份）");

        // 一个截断的 gzip（头是对的、后面少了）：这是"存到一半断电"的样子。
        const QString cutDir = scratch.path() + QStringLiteral("/截断");
        QDir().mkpath(cutDir);
        QFile gz(WorldFiles::levelDataPath(worldDir));
        QByteArray gzBytes;
        if (gz.open(QIODevice::ReadOnly)) {
            gzBytes = gz.readAll();
        }
        writeFileAt(WorldFiles::levelDataPath(cutDir), gzBytes.left(gzBytes.size() / 2));
        WorldFiles::LevelData cut;
        check(!WorldFiles::readLevelData(cutDir, &cut, &why), "★★ ZL6 截断的 gzip 也读不动（半截的存档）");

        // 文件不在。
        WorldFiles::LevelData missing;
        check(!WorldFiles::readLevelData(scratch.path() + QStringLiteral("/没有这个目录"), &missing, &why),
              "ZL6 文件不在就说文件不在");

        // ---- 不 gzip 的那一份：**不算坏档**，而且写回去还是不加 gzip ----
        //
        // 这一档是被"有没有 gzip 头"那条判据故意放行的：第三方工具（以及少数版本的
        // 服务端）会写不压缩的 level.dat。把它报成"这个存档可能坏了"，用户就得去处理一个
        // 根本没坏的东西。
        const QString rawDir = scratch.path() + QStringLiteral("/不压缩");
        QDir().mkpath(rawDir);
        NbtTree::Document rawDoc;
        rawDoc.root.type = NbtTree::Type::Compound;
        NbtTree::Field rawData;
        rawData.first = QStringLiteral("Data");
        rawData.second.type = NbtTree::Type::Compound;
        NbtTree::Node rawName;
        rawName.type = NbtTree::Type::String;
        rawName.text = QStringLiteral("不压缩的世界");
        rawData.second.fields = NbtTree::FieldList{{QStringLiteral("LevelName"), rawName}};
        rawDoc.root.fields = NbtTree::FieldList{rawData};
        writeFileAt(WorldFiles::levelDataPath(rawDir), NbtTree::serialize(rawDoc));

        WorldFiles::LevelData raw;
        check(WorldFiles::readLevelData(rawDir, &raw, &why),
              "★★ ZL6 一份**没有** gzip 头的 level.dat 照样读得动（不是「坏档」）", why);
        check(!raw.info.gzipped, "ZL6 而且如实说它不是 gzip 的");
        check(raw.info.levelName == QStringLiteral("不压缩的世界"), "ZL6 里面那格名字也读出来了");
        raw.info.levelName = QStringLiteral("不压缩的世界改过");
        WorldFiles::applyWorldInfo(&raw.document, raw.info, &why);
        check(WorldFiles::writeLevelData(rawDir, raw, &why), "ZL6 写回去", why);
        QFile rawFile(WorldFiles::levelDataPath(rawDir));
        QByteArray rawBytes;
        if (rawFile.open(QIODevice::ReadOnly)) {
            rawBytes = rawFile.readAll();
        }
        check(!rawBytes.startsWith("\x1f\x8b"), "★★ ZL6 写回去的那份**没有**被硬套上 gzip（形状跟着原来那份）");
        WorldFiles::LevelData rawBack;
        check(!WorldFiles::writeLevelData(rawDir, raw, &why) && why.contains(QStringLiteral("修改")),
              "ZL6 旧读取快照不能覆盖已经保存的新文件", why);
        rawFile.close();
        check(WorldFiles::readLevelData(rawDir, &rawBack, &why), "ZL6 重新读取建立新基线", why);
        const QByteArray external = rawBytes + QByteArray("external");
        writeFileAt(WorldFiles::levelDataPath(rawDir), external);
        check(!WorldFiles::writeLevelData(rawDir, rawBack, &why),
              "ZL6 外部修改后拒绝保存旧设置", why);
        QFile preserved(WorldFiles::levelDataPath(rawDir));
        check(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == external,
              "ZL6 冲突拒绝完整保留外部文件");
        preserved.close();
        writeFileAt(WorldFiles::levelDataPath(rawDir), rawBytes);
        check(WorldFiles::readLevelData(rawDir, &rawBack, &why) &&
                  rawBack.info.levelName == QStringLiteral("不压缩的世界改过"),
              "ZL6 再把不压缩的那份读回来，改的地方在");
    }
#ifdef Q_OS_WIN
    {
        // 2.1.2 补失败证据：发布那一步被占用（杀毒实时扫描 / 游戏开着这个世界）。
        // QSaveFile 的 commit 是原子替换，被拒绝时盘上原来那一份必须**一个字节不变**，
        // 世界里也不许留下任何临时文件。
        const QString dirName = QStringLiteral("世界-发布失败");
        const QString dir = scratch.path() + QStringLiteral("/") + dirName;
        QDir().mkpath(dir);
        NbtTree::Document document;
        document.root.type = NbtTree::Type::Compound;
        NbtTree::Field data;
        data.first = QStringLiteral("Data");
        data.second.type = NbtTree::Type::Compound;
        NbtTree::Node nameNode;
        nameNode.type = NbtTree::Type::String;
        nameNode.text = QStringLiteral("原名字");
        NbtTree::Field nameField;
        nameField.first = QStringLiteral("LevelName");
        nameField.second = nameNode;
        data.second.fields = NbtTree::FieldList{nameField};
        document.root.fields = NbtTree::FieldList{data};
        const QString plain = dir + QStringLiteral("/plain.nbt.tmp");
        writeFileAt(plain, NbtTree::serialize(document));
        QString why;
        check(ZipUtil::gzipFrom(plain, WorldFiles::levelDataPath(dir), &why),
              "ZL7 造一个 gzip 的 level.dat", why);
        QFile::remove(plain);
        QByteArray originalBytes;
        {
            QFile file(WorldFiles::levelDataPath(dir));
            check(file.open(QIODevice::ReadOnly), "ZL7 记下原来那份的字节");
            originalBytes = file.readAll();
        }
        WorldFiles::LevelData loaded;
        check(WorldFiles::readLevelData(dir, &loaded, &why), "ZL7 读得动", why);
        loaded.info.levelName = QStringLiteral("新名字");
        WorldFiles::applyWorldInfo(&loaded.document, loaded.info, &why);
        const HANDLE held = CreateFileW(
            reinterpret_cast<LPCWSTR>(WorldFiles::levelDataPath(dir).utf16()),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE, "ZL7 占住 level.dat（允许读写、拒绝替换）");
        if (held != INVALID_HANDLE_VALUE) {
            const bool saved = WorldFiles::writeLevelData(dir, loaded, &why);
            CloseHandle(held);
            check(!saved && !why.isEmpty(),
                  "★★ ZL7 发布被占用时保存失败，而且说得出一句话", why);
            QFile preserved(WorldFiles::levelDataPath(dir));
            check(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == originalBytes,
                  "★★ ZL7 失败时原来那份 level.dat 一个字节不变");
            preserved.close();
            check(QDir(dir).entryList(QDir::Files | QDir::Hidden).size() == 1,
                  "★★ ZL7 世界里没有留下临时文件（只有 level.dat）",
                  QDir(dir).entryList(QDir::Files | QDir::Hidden).join(QLatin1Char(',')));
            WorldFiles::LevelData after;
            check(WorldFiles::readLevelData(dir, &after, &why) &&
                      after.info.levelName == QStringLiteral("原名字"),
                  "ZL7 重新打开还是原来那份（下次可以再试）");
        }
    }
#endif
}

