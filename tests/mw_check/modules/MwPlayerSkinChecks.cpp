#include "Support.hpp"
#include "controllers/PlayerSkinController.hpp"
int MwPlayerSkinChecks(MainWindow &window) {
    // 全程离线：贴图是测试自己写进沙盒的两张 PNG，走 applyPlayerSkin() 直接喂进去
    // （**不经过** PlayerSkinStore，所以既不联网也不依赖任何人的账号）。这一段是"界面
    // 接线"的断言，网络那一半在 verify 的 T 段（纯函数）和真机验证里。
    {
        SkinViewer *viewer = byObjectName<SkinViewer>(&window, MainWindowUI::skinViewerName());
        QLabel *hint = byObjectName<QLabel>(&window, MainWindowUI::skinHintLabelName());
        QLabel *avatar = byObjectName<QLabel>(&window, MainWindowUI::avatarLabelName());
        check(viewer != nullptr && hint != nullptr && avatar != nullptr,
              "右栏那个小人和它下面那句话都按 objectName 找得到");

        if (viewer != nullptr && hint != nullptr && avatar != nullptr) {
            // 两张测试贴图往沙盒里写（= 这个工程自己的 .minecraft，绝不是真的那份）。
            const QString skinSandbox = AppPaths::minecraftDir();

            // ---- X1 开窗（没登录）就是默认外观 ----
            // 这条是兜底：**任何时候都不该看到一块空白**。没登录、还没取到、取不到，
            // 三种情况看到的都是它。
            check(!viewer->skin().isNull() && viewer->skin() == SkinModel::defaultSteveSkin(),
                  "没登录时右栏画的是默认外观");
            check(hint->text() == MainWindowUI::skinHintNotLoggedInText(),
                  "那句话说的是「未登录，显示的是默认外观」", hint->text());
            const QImage defaultAvatar = avatar->pixmap().toImage();

            // ---- X2 喂一份真外观（右栏 + 头像 + 那句话一起跟着走） ----
            const QString skinPath = skinSandbox + "/x-skin.png";
            const QString capePath = skinSandbox + "/x-cape.png";
            QImage testSkin(64, 64, QImage::Format_ARGB32_Premultiplied);
            testSkin.fill(Qt::transparent);
            {
                QPainter painter(&testSkin);
                // 整张涂成一个**默认外观里没有的颜色**：这样"画没画成这一张"只要比
                // 一个像素就知道，不用逐张图对。
                painter.fillRect(0, 0, 64, 64, QColor(20, 200, 60));
                painter.end();
            }
            QImage testCape(64, 32, QImage::Format_ARGB32_Premultiplied);
            testCape.fill(QColor(200, 30, 30));
            check(testSkin.save(skinPath) && testCape.save(capePath), "沙盒里写出两张测试贴图");

            QMetaObject::invokeMethod(&window, "applyPlayerSkin", Qt::DirectConnection,
                                      Q_ARG(QString, skinPath), Q_ARG(QString, capePath),
                                      Q_ARG(bool, true));
            QApplication::processEvents();
            check(!viewer->skin().isNull() && viewer->skin().pixelColor(0, 0) == QColor(20, 200, 60),
                  "右栏换成了喂进去的那张皮肤",
                  viewer->skin().pixelColor(0, 0).name());
            check(!viewer->cape().isNull(), "披风也画上了（这张图有披风）");
            check(viewer->isSlim(), "纤细模型这个开关也传到了控件里");
            check(hint->text() == MainWindowUI::skinHintLoadedText(QStringLiteral("Player"), true),
                  "那句话换成了「…的正版外观（含披风）」", hint->text());
            check(!avatar->pixmap().isNull() &&
                      avatar->pixmap().toImage() != defaultAvatar,
                  "底栏头像跟着换成了这张皮肤的头（不再是默认外观那个）");

            // ---- X3 退回默认外观（传空路径） ----
            // 这一条是"注销/换账号/取不到"都会走的那条路。
            QMetaObject::invokeMethod(&window, "applyPlayerSkin", Qt::DirectConnection,
                                      Q_ARG(QString, QString()), Q_ARG(QString, QString()),
                                      Q_ARG(bool, false));
            QApplication::processEvents();
            check(viewer->skin() == SkinModel::defaultSteveSkin() && viewer->cape().isNull(),
                  "传空路径 → 退回默认外观、披风也清掉");
            check(hint->text() == MainWindowUI::skinHintNotLoggedInText(),
                  "那句话也跟着退回去了", hint->text());
            check(avatar->pixmap().toImage() == defaultAvatar,
                  "头像退回默认外观那个头");

            // ---- X4 路径存在但载不进来（下了一半 / 文件被删了） ----
            // 这条**必须**和 X3 表现一致：载入失败也是"退回默认外观"，而不是把一块空白
            // 或者半张图塞进控件里。
            QMetaObject::invokeMethod(&window, "applyPlayerSkin", Qt::DirectConnection,
                                      Q_ARG(QString, skinSandbox + "/这个文件根本不存在.png"),
                                      Q_ARG(QString, QString()), Q_ARG(bool, false));
            QApplication::processEvents();
            check(viewer->skin() == SkinModel::defaultSteveSkin(),
                  "贴图载不进来时也是默认外观（不是空白）");
            check(avatar->pixmap().toImage() == defaultAvatar,
                  "同上：头像也是默认外观那个头");

            QFile::remove(skinPath);
            QFile::remove(capePath);

            // ---- X5 披风先到、皮肤后到（真机上折的就是这个顺序） ----
            //
            // 上面几条是"直接喂 applyPlayerSkin"，验的是那个函数本身。这一条走的是**真的
            // 那两条路**：档案到了（onSkinProfileReady）→ 贴图到了（onSkinTextureReady）→
            // 画出来，全程**不联网**。
            //
            // 要钉的那件事：披风那张小得多，经常**比皮肤先下完**。它先到的时候皮肤那一路
            // 还是空的，而"皮肤载不进来"那条路会把披风一起清掉 —— 清掉之后就再也没有哪一趟
            // 会把披风补回来，表现是**皮肤有了、披风永远没有**（真机上就是这个）。
            //
            // 怎么把"皮肤还没到"这个状态**离线**摆出来：给皮肤那一路一个**取不到的**地址
            // （不是官方那个 host，textureRequestUrl 直接判死、同步发 failed，一个包都不发）。
            // 真机上那个状态是"请求还在路上"，这里换成"请求根本发不出去" —— 对被验的那段
            // 代码来说两者**一模一样**（都是 m_skinPath 还空着），但这一条不用等网络、也不必
            // 赌谁先下完。皮肤那一趟随后从 onSkinTextureReady 补进来（Store 用的就是它）。
            {
                // 【前提：这个窗口得有一个**正版身份**】
                // "档案到了"这条路是**正版账号**的（离线账号走的是"挑一张"那条，见 AF 节），
                // 而那一发带回自己的 uuid：落盘和画之前都要和"当下是谁"对一下
                // （见 onSkinProfileReady 开头那道闸）—— 对不上的整份丢掉。所以这一节先按
                // 产品那套格式往沙盒的活动槽里放一个正版凭据：只放 minecraft 那一份（名字和
                // uuid 都在里面），一个包都不发。
                const QString x5Uuid = QStringLiteral("5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a");
                const QString x5Name = QStringLiteral("X5Player");
                QJsonObject x5Minecraft;
                x5Minecraft.insert(QStringLiteral("access_token"), QStringLiteral("at-x5"));
                x5Minecraft.insert(QStringLiteral("expires_at"),
                                   QStringLiteral("2999-01-01T00:00:00Z"));
                x5Minecraft.insert(QStringLiteral("name"), x5Name);
                x5Minecraft.insert(QStringLiteral("uuid"), x5Uuid);
                AuthManager *x5Auth = window.findChild<AuthManager *>();
                const bool x5Wrote = TokenStore::writeJson(TokenStore::minecraftKey(), x5Minecraft);
                if (x5Auth != nullptr) {
                    x5Auth->reloadStoredAccount();
                }
                check(x5Wrote && x5Auth != nullptr && x5Auth->loggedIn() &&
                          x5Auth->playerUuid() == QStringLiteral("5a5a5a5a-5a5a-5a5a-5a5a-5a5a5a5a5a5a"),
                      "X5 前提：活动槽里放着一个正版身份，内存里也是它",
                      x5Auth == nullptr ? QStringLiteral("<没有 AuthManager>") : x5Auth->playerUuid());

                const QString skinUrl =
                    QStringLiteral("https://textures.minecraft.net/texture/x5-skin");
                const QString capeUrl =
                    QStringLiteral("https://textures.minecraft.net/texture/x5-cape");
                const QString pendingSkinUrl = QStringLiteral("https://evil.example.com/x5.png");
                const QString skinCached = PlayerSkinStore::cachePathForUrl(skinUrl);
                const QString capeCached = PlayerSkinStore::cachePathForUrl(capeUrl);
                const QString x5SkinPath = skinSandbox + QStringLiteral("/x5-skin.png");
                QDir().mkpath(skinSandbox + QStringLiteral("/skins"));

                QImage cachedSkin(64, 64, QImage::Format_ARGB32_Premultiplied);
                cachedSkin.fill(QColor(20, 200, 60));
                QImage cachedCape(64, 32, QImage::Format_ARGB32_Premultiplied);
                cachedCape.fill(QColor(200, 30, 30));
                check(cachedSkin.save(skinCached) && cachedCape.save(capeCached) &&
                          cachedSkin.save(x5SkinPath),
                      "X5 贴图按内容寻址塞进缓存（下面的取图因此一个请求都不发）");

                PlayerSkinProfile racy;
                racy.skinUrl = pendingSkinUrl;
                racy.capeUrl = capeUrl;
                racy.variant = QStringLiteral("classic");
                QMetaObject::invokeMethod(&window, "onSkinProfileReady", Qt::DirectConnection,
                                          Q_ARG(QString, x5Uuid),
                                          Q_ARG(PlayerSkinProfile, racy));
                QApplication::processEvents();
                // 披风命中缓存、**同步**回来了，而这时候 m_skinPath 还是空的 —— 就是那个 race。
                check(viewer->skin() == SkinModel::defaultSteveSkin() && viewer->cape().isNull(),
                      "X5 披风先到时先**记住**、不画（这时候屏幕上是默认外观）");

                QMetaObject::invokeMethod(&window, "onSkinTextureReady", Qt::DirectConnection,
                                          Q_ARG(QString, pendingSkinUrl),
                                          Q_ARG(QString, x5SkinPath));
                QApplication::processEvents();
                check(viewer->skin().pixelColor(0, 0) == QColor(20, 200, 60),
                      "X5 皮肤那一趟到了，画上了",
                      viewer->skin().pixelColor(0, 0).name());
                check(!viewer->cape().isNull(),
                      "X5 并且把先前记住的披风一起画出来了（没有被那次空皮肤清掉）");
                check(hint->text() == MainWindowUI::skinHintLoadedText(x5Name, true),
                      "X5 那句话也说「含披风」（名字是账号那个，不是默认的 Player）",
                      hint->text());

                // ---- X6 换了档案、新档案里**没有披风**（用户在游戏里把披风摘了） ----
                // 手上那条披风路径必须跟着作废，否则它会被下一趟 applyPlayerSkin 又画回去，
                // 而新的披风那一张根本不会到达 —— 没有任何一趟会来纠正它。
                //
                // 先回到"有皮肤、有披风"的常态（这一趟两个地址都命中缓存）。
                PlayerSkinProfile normal;
                normal.skinUrl = skinUrl;
                normal.capeUrl = capeUrl;
                normal.variant = QStringLiteral("classic");
                QMetaObject::invokeMethod(&window, "onSkinProfileReady", Qt::DirectConnection,
                                          Q_ARG(QString, x5Uuid),
                                          Q_ARG(PlayerSkinProfile, normal));
                QApplication::processEvents();
                check(!viewer->cape().isNull(), "X6 前提：现在画面上有披风");

                PlayerSkinProfile noCape;
                noCape.skinUrl = skinUrl;
                noCape.variant = QStringLiteral("classic");
                QMetaObject::invokeMethod(&window, "onSkinProfileReady", Qt::DirectConnection,
                                          Q_ARG(QString, x5Uuid),
                                          Q_ARG(PlayerSkinProfile, noCape));
                QApplication::processEvents();
                check(viewer->cape().isNull(), "X6 新档案没有披风 → 画面上那条披风也没了");
                check(hint->text() == MainWindowUI::skinHintLoadedText(x5Name, false),
                      "X6 那句话回到不带「含披风」的那一档", hint->text());

                // ---- X6b 换披风成功后，按新地址**立即**换图 ----
                // 不等下一次档案请求：换披风接口返回成功时这张图已经在披风列表里了。
                auto *controller = window.findChild<PlayerSkinController *>();
                const QString newCapeUrl =
                    QStringLiteral("https://textures.minecraft.net/texture/x6b-new-cape");
                const QString newCapeCached = PlayerSkinStore::cachePathForUrl(newCapeUrl);
                QImage newCape(64, 32, QImage::Format_ARGB32_Premultiplied);
                newCape.fill(QColor(30, 60, 220));
                check(controller != nullptr && newCape.save(newCapeCached),
                      "X6b 准备一张新披风贴图");
                if (controller != nullptr) {
                    controller->setCapeUrl(newCapeUrl);
                    QApplication::processEvents();
                }
                check(!viewer->cape().isNull() && viewer->cape().pixelColor(0, 0) == QColor(30, 60, 220),
                      "X6b setCapeUrl 立即把主页披风换成新地址那张",
                      viewer->cape().isNull() ? QStringLiteral("<无披风>")
                                              : viewer->cape().pixelColor(0, 0).name());
                QFile::remove(newCapeCached);
                QFile::remove(skinCached);
                QFile::remove(capeCached);
                QFile::remove(x5SkinPath);

                // 身份还回去：后面几节看到的是**同一个窗口**，这一节不该把它的账号状态留成
                // "登录着 X5Player"（那会让后面那些节看到另一幅底栏）。顺序是"先清盘上那份、
                // 再重读内存、最后把外观退回默认" —— 最后那一步会顺手重算那句话，
                // 判据（loggedIn()）这时候已经是假了，于是它回到「未登录」那一档。
                TokenStore::clear(TokenStore::minecraftKey());
                if (x5Auth != nullptr) {
                    x5Auth->reloadStoredAccount();
                }
                QMetaObject::invokeMethod(&window, "applyPlayerSkin", Qt::DirectConnection,
                                          Q_ARG(QString, QString()), Q_ARG(QString, QString()),
                                          Q_ARG(bool, false));
                QApplication::processEvents();
                check(x5Auth != nullptr && !x5Auth->loggedIn() &&
                          hint->text() == MainWindowUI::skinHintNotLoggedInText(),
                      "X5 验完把身份还回去了（后面几节看到的还是那个没登录的窗口）",
                      hint->text());
            }
        }
    }
    return 0;
}


