#include "Support.hpp"
#include "ModpackDomainChecks.hpp"
#include <QUuid>

void runThemeImageTests(const QString &sandbox) {
    // --- V5v. 用户挑的那张背景图是怎么收进数据目录的（主题那两张图） -----------------
    //
    // 这一整节在**临时数据根目录**里跑（`AppPaths::setMinecraftDir()` 一处切换，和
    // runIconCacheTests 那一段同一形状）：下面这些断言全都要"看着 cache/theme 里现在有哪
    // 几个文件"，不隔离就是拿用户盘上已经存着的那张图做实验。跑完必须还原 —— 所以这一节里
    // **一条 return 都不许有**（连"读不到夹具就 return"那种也不行，夹具那一条只在有它时再往下）。
    {
        const QString originalDir = AppPaths::minecraftDir();
        QTemporaryDir themeRoot;
        QTemporaryDir sourceRoot;
        check(themeRoot.isValid() && sourceRoot.isValid(),
              "V5v 两个临时目录建出来了（临时数据根目录 + 放源图的那个）");
        AppPaths::setMinecraftDir(themeRoot.path());
        const QString src = sourceRoot.path();
        const QString slotA = QStringLiteral("card");
        const QString slotB = QStringLiteral("pageBackground");
        const QString slotC = QStringLiteral("titleBar");
        QString error;

        // 前提：这会儿 cache/theme **还不存在**。下面第一条 store() 能成，才说明那个目录是
        // 它自己按需建的（写盘前那一次 mkpath），不是靠谁先替它摆好了。
        check(!QFileInfo::exists(AppPaths::themeImageDir()),
              "V5v 前提：cache/theme 这会儿还不存在");

        // 源图三张，各有各要证的那一条：带透明的 PNG（必须存成 .png）、不带透明的 JPEG
        // （存成 .jpg）、3000 px 宽的大图（长边必须被缩到 2560 以内）。
        const QString alphaPng = src + QStringLiteral("/带透明.png");
        const QString plainJpg = src + QStringLiteral("/不透明.jpg");
        const QString bigJpg = src + QStringLiteral("/大图.jpg");
        {
            QImage withAlpha(64, 32, QImage::Format_ARGB32);
            withAlpha.fill(QColor(255, 0, 0, 128));
            check(withAlpha.save(alphaPng, "PNG"), "V5v 造出一张带透明的 PNG");
            QImage opaque(64, 32, QImage::Format_RGB32);
            opaque.fill(QColor(0, 0, 255));
            check(opaque.save(plainJpg, "JPG", 92), "V5v 造出一张不带透明的 JPEG");
            QImage big(3000, 1500, QImage::Format_RGB32);
            big.fill(QColor(0, 128, 0));
            check(big.save(bigJpg, "JPG", 92), "V5v 造出一张 3000x1500 的大图");
        }

        // ① 带透明的那张：落点固定 `<数据目录>/cache/theme/<槽位>.png`。
        const QString storedPng = ThemeImage::store(slotA, alphaPng, &error);
        check(!storedPng.isEmpty() && error.isEmpty(), "V5v 带透明的 PNG 收得进来", error);
        check(storedPng == AppPaths::themeImageDir() + QLatin1Char('/') + slotA +
                               QStringLiteral(".png") &&
                  !storedPng.contains(QLatin1Char('\\')),
              "V5v 落点是 <数据目录>/cache/theme/<槽位>.png（正斜杠，槽位名就是文件名）",
              storedPng);
        {
            const QImage back(storedPng);
            check(!back.isNull() && back.size() == QSize(64, 32),
                  "V5v 收进来那一份读得回来、尺寸没变",
                  QStringLiteral("%1x%2").arg(back.width()).arg(back.height()));
        }

        // ② 不带透明的那张存成 .jpg（省下一半以上的地方）。
        const QString storedJpg = ThemeImage::store(slotB, plainJpg, &error);
        check(storedJpg == AppPaths::themeImageDir() + QLatin1Char('/') + slotB +
                               QStringLiteral(".jpg"),
              "V5v 不带透明的存成 .jpg", storedJpg);

        // ③ 名字撒谎的那张：同一段 JPEG 字节换个 .png 的名字挑进来，**后缀仍然跟着内容走**。
        // 这一条是"用户把桌面上的图重命名过"的日常 —— 认名字的话这里就存出一份 .png。
        const QString lyingPng = src + QStringLiteral("/名字是png其实是jpeg.png");
        QFile::copy(plainJpg, lyingPng);
        const QString storedLying = ThemeImage::store(slotB, lyingPng, &error);
        check(storedLying == storedJpg &&
                  !QFileInfo::exists(AppPaths::themeImageDir() + QLatin1Char('/') + slotB +
                                     QStringLiteral(".png")),
              "★ V5v 扩展名跟着内容走：名字叫 .png 的 JPEG 也没留下一份 .png",
              storedLying);

        // ④ 大图 + "一个槽位只有一份"：换过扩展名的那份旧图必须被删掉，不能攒着。
        const QString storedBig = ThemeImage::store(slotA, bigJpg, &error);
        check(!storedBig.isEmpty(), "V5v 大图也收得进来", error);
        {
            const QImage back(storedBig);
            check(back.size() == QSize(2560, 1280),
                  "★ V5v 长边被缩到 2560（3000 原样就是 48 MB 位图换一块 1200 px 宽的底）",
                  QStringLiteral("%1x%2").arg(back.width()).arg(back.height()));
        }
        check(!QFileInfo::exists(AppPaths::themeImageDir() + QLatin1Char('/') + slotA +
                                 QStringLiteral(".png")) &&
                  QFileInfo::exists(storedBig),
              "★ V5v 同一个槽位换图之后盘上只剩一份（先是 .png，这回是 .jpg，旧那份删掉了）",
              storedBig);

        // ⑤ 挑了一张坏的（一段文字当图片）：如实拒绝，而且**原来那张一个字节都不许动**
        // —— 用户手滑挑错一次，不该顺手把他现在用的那张也弄没。
        const QString notImage = src + QStringLiteral("/不是图片.png");
        writeFileAt(notImage, QByteArrayLiteral("这不是一张图片，只是一段文字。"));
        QByteArray keepBytes;
        {
            QFile keeper(storedBig);
            if (keeper.open(QIODevice::ReadOnly)) {
                keepBytes = keeper.readAll();
            }
        }
        check(!keepBytes.isEmpty(), "V5v 前提：记下了现在那一份的字节");
        const QString broken = ThemeImage::store(slotA, notImage, &error);
        check(broken.isEmpty() && error.contains(QStringLiteral("不是一张能打开的图片")),
              "V5v 一段文字当成图挑进来时如实拒绝（给用户的话里说清了是哪一张）", error);
        QByteArray afterBytes;
        {
            QFile keeper(storedBig);
            if (keeper.open(QIODevice::ReadOnly)) {
                afterBytes = keeper.readAll();
            }
        }
        check(afterBytes == keepBytes,
              "★ V5v 挑坏图那一发没能动到原来那张（字节一模一样）");

        // ⑥ 太大：**只 resize、内容一个字节都不写**（全是 0 也够了 —— 这一条判的是文件大小，
        // 而且判在解码之前，所以它挡的正是"手滑选了个视频/压缩包"）。
        const QString huge = src + QStringLiteral("/太大.png");
        {
            QFile file(huge);
            if (file.open(QIODevice::WriteOnly)) {
                file.resize(ThemeImage::kMaxSourceBytes + 1);
            }
        }
        check(QFileInfo(huge).size() > ThemeImage::kMaxSourceBytes,
              "V5v 前提：造出一个超过 64 MB 的源文件",
              QStringLiteral("%1 字节").arg(QFileInfo(huge).size()));
        const QString tooBig = ThemeImage::store(slotA, huge, &error);
        check(tooBig.isEmpty() && error.contains(QStringLiteral("太大")) &&
                  QFileInfo::exists(storedBig),
              "V5v 超过 64 MB 的源文件当场拒绝（拒绝的那一发也没动原来那张）", error);

        // ⑦ 源文件根本不在（用户挑完就把图删了 / 挪走了）。
        const QString missing =
            ThemeImage::store(slotA, src + QStringLiteral("/根本没这个文件.png"), &error);
        check(missing.isEmpty() && !error.isEmpty(), "V5v 源文件不在时如实拒绝", error);

        // ⑧ 槽位名不安全：这是我们自己那张表给的字符串，这一道防的是"以后有人拿用户输入喂进来"。
        const QString escape = ThemeImage::store(QStringLiteral("../跑出去"), alphaPng, &error);
        check(escape.isEmpty() && !error.isEmpty(),
              "V5v 槽位名里带 ../ 时拒绝（不拿它去拼路径）", error);
        check(!QFileInfo::exists(themeRoot.path() + QStringLiteral("/cache/跑出去.png")) &&
                  !QFileInfo::exists(themeRoot.path() + QStringLiteral("/跑出去.png")),
              "★ V5v 那一发一个文件都没写出去（槽位名要是被当路径用，它就跑出 cache/theme 了）");

        // ⑨ 这份 Qt 自己解不了的那种（**webp**）：挑图那个对话框的筛选里写着 `*.webp`，
        // 它就得真的打得开 —— 走的是 `ImageDecode`（WIC）那第二条路。夹具就是 ZC 段
        // 用的同一份（从 Modrinth CDN 上真取下来的 96px 缩略图）。
        QByteArray webpBytes;
        {
            QFile fixture(QStringLiteral(REPO_ROOT_PATH) + "/tests/meta/icon-sample.webp");
            if (fixture.open(QIODevice::ReadOnly)) {
                webpBytes = fixture.readAll();
            }
        }
        check(!webpBytes.isEmpty(), "V5v 读到了 webp 夹具（meta/icon-sample.webp）");
        if (!webpBytes.isEmpty()) {
            const QString webpPath = src + QStringLiteral("/浏览器存下来的.webp");
            writeFileAt(webpPath, webpBytes);
            const QString storedWebp = ThemeImage::store(slotC, webpPath, &error);
            // 判据跟着**这条路真能不能解**走，不跟 `webpAvailable()` 那个
            // 枚举结果走 —— Windows Server 镜像上它会说有、真解却失败（见 ZC2 那段）。
            // ThemeImage 自己先让 Qt 试、再走 `ImageDecode::decode`，这里用的正是同一个
            // 判据（同一份夹具、同一条路）。
            if (!ImageDecode::decode(webpBytes).isNull()) {
                check(!storedWebp.isEmpty() && QFileInfo::exists(storedWebp),
                      "★ V5v webp 也收得进来（Qt 认不得，退给 WIC 那条路）", error);
                const QImage back(storedWebp);
                check(back.size() == QSize(96, 96),
                      "V5v webp 收进来的那一份读得回来、尺寸对",
                      QStringLiteral("%1x%2").arg(back.width()).arg(back.height()));
            } else {
                // 这台机器没有那个编解码器（Win10 上多半没有，见 ImageDecode.hpp）：如实拒绝
                // —— 那时候**硬解出个什么来**才是最糟的（存进去一张残图，屏幕上还说是好的）。
                check(storedWebp.isEmpty() && !error.isEmpty(),
                      "V5v 这台机器解不了 webp 时如实拒绝", error);
            }
        }

        // ⑩ remove()：删点名的那一个、不碰别的槽位、没有就什么都不做（不崩、不乱删）。
        ThemeImage::remove(slotA);
        check(!QFileInfo::exists(storedBig), "V5v remove() 把点名那个槽位的那一份删掉了");
        check(QFileInfo::exists(storedJpg), "V5v remove() 不碰别的槽位（另一张还在）");
        ThemeImage::remove(slotA);
        ThemeImage::remove(QStringLiteral("../跑出去"));
        check(QFileInfo::exists(storedJpg),
              "V5v 再删一次 / 槽位名不安全的那一发都什么都不做（盘上一个文件没少）");

        ThemeImage::remove(slotB);
        ThemeImage::remove(slotC);
        check(QDir(AppPaths::themeImageDir()).entryList(QDir::Files).isEmpty(),
              "V5v 三个槽位都清干净了，cache/theme 里一份图都不剩");

        AppPaths::setMinecraftDir(originalDir);
        check(AppPaths::minecraftDir() == originalDir, "V5v 数据根目录还原回去了");
    }

}
