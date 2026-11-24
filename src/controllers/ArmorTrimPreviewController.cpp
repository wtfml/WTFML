#include "controllers/ArmorTrimPreviewController.hpp"
#include <QCoreApplication>
#include "ToolboxPage.hpp"
#include "ArmorTrimScene.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "download/ResumableDownload.hpp"
#include "manage/InstanceFiles.hpp"
#include "tools/ArmorTrim.hpp"
#include "tools/ArmorTrimAssets.hpp"
#include "util/AppPaths.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSet>
#include <QSharedPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace {
// 钉死官方 Java 1.21.4 客户端：它同时带着涡流/镶铆（1.21）与树脂质（1.21.4）的调色板，
// 而且已经是新的装备层路径（本程序会把新路径折回旧名，UV 逐字节没变）。版号钉死 =
// 官方哪天换了贴图布局，这里不会悄悄跟着变。
const QString kSha1 = QStringLiteral("a7e5a6024bfd3cd614625aa05629adf760020304");
constexpr qint64 kBytes = 28335587;
/// 后台读归档的总预算。**唯一出处**：工人循环的判据和析构里等它的时长都读它 ——
/// 两边写死两个数（90 秒 / 40 秒）时，析构会在工人还在跑的时候返回，见析构那段 NOTE。
constexpr int kLoadBudgetMs = 90000;
const QUrl kClient(QStringLiteral("https://piston-data.mojang.com/v1/objects/") + kSha1 + QStringLiteral("/client.jar"));

/// 这个版本（顺着 inheritsFrom 往上）是什么时候发布的 —— 给候选排序用。
///
/// 读 `versions/<id>/<id>.json` 的 `releaseTime`（快照/正式版都有）；融合实例的 json
/// 只有 `clientVersion`，所以调用方传的是 `InstanceFiles::jarOwnerId()`。
///
/// 整条链上都没有 `releaseTime` 的（魔改整包那份自己拼的 json、手写的壳）回 **0**、
/// 不拿 json 的 mtime 顶替：mtime 记的是"哪天解压进来的"，几十个版本同一天装的时候
/// 它能让一个魔改包排到原版前面 —— 预览要的是原版客户端里的纹理和调色板。0 排在最旧，
/// 有原版可读时永远轮不到它们；机器上只有魔改版时它们照样会被试到（只是排在后面）。
qint64 releaseStamp(const QString &versionId) {
    QString current = versionId;
    QSet<QString> visited;
    for (int depth = 0; depth < 8 && !current.isEmpty() && !visited.contains(current); ++depth) {
        visited.insert(current);
        const QString jsonPath = InstanceFiles::jsonPath(current);
        QFile file(jsonPath);
        if (!file.open(QIODevice::ReadOnly)) break;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        QString stamp = object.value(QStringLiteral("releaseTime")).toString();
        if (stamp.isEmpty()) stamp = object.value(QStringLiteral("time")).toString();
        if (!stamp.isEmpty()) {
            QDateTime when = QDateTime::fromString(stamp, Qt::ISODateWithMs);
            if (!when.isValid()) when = QDateTime::fromString(stamp, Qt::ISODate);
            if (when.isValid()) return when.toMSecsSinceEpoch();
        }
        current = object.value(QStringLiteral("inheritsFrom")).toString();
    }
    return 0;
}
}

ArmorTrimPreviewController::ArmorTrimPreviewController(ToolboxPage *page, OverlayHost *overlay,
                                                       QObject *parent)
    : QObject(parent), m_page(page), m_overlay(overlay) {
    m_download=new ResumableDownload(this);
    m_deadline=new QTimer(this); m_deadline->setSingleShot(true); m_deadline->setInterval(600000);
    connect(page,&ToolboxPage::trimResourcesRequested,this,[this](int action){request(action);});
    connect(m_deadline,&QTimer::timeout,this,[this] {
        // 先记下"是超时"，再置 m_cancelled：finished 那一发只看 m_cancelled 的话，
        // 超时会被当成用户取消（屏幕上就看到"已取消"，看不到为什么停）。
        m_timedOut=true; m_cancelled=true; m_download->cancel();
        status(tr("预览资源下载超过 600 秒，已停止。重新下载可继续未完成的部分。"));
    });
    connect(m_download,&ResumableDownload::progress,this,[this](qint64 done,qint64 total) {
        if (!m_page || m_cancelled) return;
        status(tr("正在下载官方预览资源：%1 / %2 MiB（不安装实例）")
               .arg(done/1048576.0,0,'f',1).arg(total/1048576.0,0,'f',1),true);
    });
    connect(m_download,&ResumableDownload::finished,this,[this](bool ok,const QString &error) {
        m_deadline->stop();
        if (m_timedOut) {
            m_timedOut=false;
            status(tr("预览资源下载超过 600 秒，已停止。重新下载可继续未完成的部分。"));
            return;
        }
        if (m_cancelled) { status(tr("已取消资源获取；未修改任何实例。")); return; }
        if (m_root != AppPaths::minecraftDir()) { m_checked=false; status(tr("游戏目录已切换，请重新读取预览资源。")); return; }
        if (!ok) { status(tr("资源下载失败：%1。可以重试或选择本地 JAR。").arg(error)); return; }
        load({cachePath()},true);
    });
}

ArmorTrimPreviewController::~ArmorTrimPreviewController() {
    shutdown();
    // 等多久必须跟工人自己的上限**对齐**。工人那段循环的判据是
    // `timer.elapsed() > kLoadBudgetMs`（90 秒），等待时长比它短的话，关掉工具箱
    // 时如果正好卡在最后一份归档的读取上，析构会在工人还在跑的时候返回，QThread
    // 对象随后被 deleteLater 掉（或者进程退出时被丢掉），那句
    // "Destroyed while thread is still running" 就是这么来的。
    // 多给 5 秒让 finished 信号和 deleteLater 走完。
    if (m_worker) {
        m_worker->wait(kLoadBudgetMs + 5000);
    }
}

void ArmorTrimPreviewController::shutdown() {
    m_cancelled=true;
    m_deadline->stop(); m_download->cancel();
    if(m_worker) m_worker->requestInterruption();
}

QString ArmorTrimPreviewController::cachePath() const {
    return m_root + QStringLiteral("/cache/preview-resources/java-1.21.4-client.jar");
}

void ArmorTrimPreviewController::status(const QString &text,bool busy) {
    m_busy=busy;
    if(m_page) m_page->setTrimResourceStatus(text,busy);
}

void ArmorTrimPreviewController::request(int action) {
    if(action==3) {
        m_cancelled=true; m_deadline->stop(); m_download->cancel();
        if(m_worker) { m_worker->requestInterruption(); status(tr("正在停止资源读取…"),true); }
        else status(tr("已取消资源获取；未修改任何实例。"));
        return;
    }
    if(m_busy || m_worker || m_download->isRunning() || !m_page) return;
    if(m_root!=AppPaths::minecraftDir()) { m_images.clear(); m_source.clear(); m_checked=false; refresh(); }
    m_root=AppPaths::minecraftDir(); m_cancelled=false; m_timedOut=false;
    if(action==0) { m_checked=false; ensureLocal(); }
    else if(action==1) {
        const QString file=QFileDialog::getOpenFileName(m_page,tr("选择 Java 1.20 及以上的客户端"),
                                   AppPaths::versionsDir(),tr("游戏客户端 (*.jar);;归档 (*.zip)"));
        if(!file.isEmpty()) load({file});
    } else if(action==2) download();
}

void ArmorTrimPreviewController::ensureLocal() {
    if(m_busy || m_worker || m_download->isRunning() || !m_page) return;
    const QString root=AppPaths::minecraftDir();
    if(m_checked && m_root==root) return;
    m_images.clear(); m_source.clear(); m_root=root; m_checked=true; m_cancelled=false; refresh();
    QStringList archives;
    // Resolve inherited client ownership through the existing instance resolver, not directory naming.
    const QDir versions(AppPaths::versionsDir());
    const auto entries=versions.entryList(QDir::Dirs|QDir::NoDotAndDotDot);
    QList<ArmorTrimAssets::ArchiveCandidate> candidates;
    for(const auto &id:entries) {
        const QString jar=InstanceFiles::jarPath(id);
        if(!QFileInfo::exists(jar)) continue;
        // 按**这个 jar 所代表的那个原版版本**的发布时间排：新的在前，认不出来是哪一版
        // 原版（魔改整包、手写壳）的按 0 排最后 —— 预览只认原版客户端里的纹理与调色板，
        // 魔改包就算装得更新也不许抢在前面。见 releaseStamp() 上面那段。
        candidates.append({id, jar, releaseStamp(InstanceFiles::jarOwnerId(id))});
    }
    // 最多试 16 份：1.21.2+ 的客户端本来就能读（新的装备层路径会折回旧名），
    // 试不下去的多半是魔改/损坏的包 —— 读失败只花一次列目录的时间。
    for(const auto &candidate:ArmorTrimAssets::newestFirst(candidates)) {
        archives.append(candidate.jar);
        if(archives.size()>=16) break;
    }
    if(!archives.isEmpty()) load(archives);
    else if(QFileInfo::exists(cachePath())) load({cachePath()},true);
    else status(tr("未找到资源：请选择本地 JAR 或下载预览资源（27.0 MiB）。"));
}

void ArmorTrimPreviewController::load(const QStringList &archives,bool verifyCache) {
    if(m_worker) return;
    status(tr("正在后台读取盔甲纹理与调色板…"),true);
    const auto result=QSharedPointer<ArmorTrimAssets::Result>::create();
    const QString root=m_root;
    auto *worker=QThread::create([result,archives,verifyCache] {
        QElapsedTimer timer; timer.start();
        for(const auto &archive:archives) {
            if(QThread::currentThread()->isInterruptionRequested() || timer.elapsed() > kLoadBudgetMs) break;
            if(verifyCache) {
                QFile file(archive); QCryptographicHash hash(QCryptographicHash::Sha1);
                bool valid=QFileInfo(file).size()==kBytes && file.open(QIODevice::ReadOnly);
                while(valid && !file.atEnd()) {
                    if(QThread::currentThread()->isInterruptionRequested()) { valid=false; break; }
                    const auto chunk=file.read(65536);
                    if(chunk.isEmpty()) { valid=false; break; }
                    hash.addData(chunk);
                }
                if(!valid || QString::fromLatin1(hash.result().toHex())!=kSha1) {
                    result->error=QCoreApplication::translate("ArmorTrimPreviewController", "资源缓存校验失败，请重新下载。"); continue;
                }
            }
            *result=ArmorTrimAssets::load(archive);
            if(result->error.isEmpty()) break;
        }
    });
    m_worker=worker;
    connect(worker,&QThread::finished,this,[this,result,root,verifyCache] {
        m_worker=nullptr;
        if(m_cancelled) { status(tr("已取消资源读取。")); return; }
        if(root!=AppPaths::minecraftDir()) { m_checked=false; status(tr("游戏目录已切换，请重新读取资源。")); return; }
        if(!result->error.isEmpty() || result->images.isEmpty()) {
            if(!verifyCache && QFileInfo::exists(cachePath()) && result->source!=cachePath()) {
                load({cachePath()},true); return;
            }
            status(result->error.isEmpty()?tr("没有可用资源，请选择本地 JAR 或下载预览资源。"):result->error);
            return;
        }
        m_images=result->images; m_source=result->source;
        status(tr("资源已载入：%1 / 固定站姿示意，拖动旋转 / 滚轮缩放；皮甲未染色，无附魔光效。")
               .arg(QFileInfo(m_source).fileName()));
        refresh();
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);
    worker->start();
}

void ArmorTrimPreviewController::download() {
    if(m_overlay==nullptr) { status(tr("现在开不了确认提示，请稍后再试。")); return; }
    // 「不出现独立窗口」：这一问和别处一样开在内容区中间（宽度只有一套，见 OverlayHost）。
    // EULA 那一行是**可点的链接**，点了由这里开浏览器 —— 光摆一串网址没人会去读。
    InlineMessageBox question(MessageKind::Question,tr("下载预览资源"),
        tr("将从 Mojang 官方下载 Java 1.21.4 客户端压缩包（27.0 MiB），校验后仅在内存读取预览纹理。\n缓存保存在游戏目录 cache/preview-resources；不安装实例、不执行 JAR、不下载依赖库。\n\n请确认你有权使用这些游戏资源，并阅读 Minecraft EULA。用户下载不代表获得额外的分发授权。"),
        QStringList{tr("确认并下载"),tr("取消")},-1,-1);
    question.setLink(tr("打开 Minecraft EULA"),QStringLiteral("https://www.minecraft.net/eula"));
    connect(&question,&InlineMessageBox::linkActivated,this,[this](const QString &url) {
        if(url.isEmpty()) return;
        if(!QDesktopServices::openUrl(QUrl(url)))
            InlineMessage::warning(m_overlay,tr("无法打开浏览器"),
                                   tr("无法打开网址：\n%1").arg(url));
    });
    m_overlay->execPrompt(&question,false);
    if(question.clickedIndex()!=0) return;
    if(m_root!=AppPaths::minecraftDir()) { m_checked=false; status(tr("游戏目录已切换，请重新获取资源。")); return; }
    const QString directory=QFileInfo(cachePath()).absolutePath();
    if(!QDir().mkpath(directory)) { status(tr("无法创建预览缓存目录。")); return; }
    m_cancelled=false; status(tr("正在连接官方资源服务器…"),true); m_deadline->start();
    m_download->start(kClient,cachePath(),kSha1,kBytes,true,directory+QStringLiteral("/partial"));
}

void ArmorTrimPreviewController::refresh() {
    if(!m_page) return;
    QList<ArmorTrimScene::Equipment> equipment;
    const bool ready=!m_images.isEmpty() && m_root==AppPaths::minecraftDir();
    if(ready) for(const auto &slot:m_page->trimEquipment())
        equipment.append({ArmorTrim::armorMaterialId(slot.armor),ArmorTrim::patternAt(slot.pattern).id,
                          ArmorTrim::trimMaterialAt(slot.material).id});
    // 「显示披风」默认关：只有勾上、且当前账号确实有披风时，才把那张图塞进场景。
    const QImage cape=m_page->trimShowCape() ? m_playerCape : QImage();
    const auto result=ArmorTrimScene::buildEquipment(m_images,equipment,m_playerSkin,m_slim,cape);
    m_page->setTrimPreview(result.scene);
    if(!ready) return;
    if(!m_busy) status(result.error.isEmpty()
        ? tr("资源：%1 / 拖动旋转，滚轮缩放")
              .arg(QFileInfo(m_source).fileName()) : result.error);
}

void ArmorTrimPreviewController::setPlayerSkin(const QImage &skin,const QImage &cape,bool slim) {
    m_playerSkin=skin; m_playerCape=cape; m_slim=slim;
    // 先告诉页面有没有披风，再 refresh：没有披风时那一格会被置灰并自动取消勾选，
    // refresh 读到的 trimShowCape() 才是收着之后的值。
    if(m_page) m_page->setTrimCapeAvailable(!cape.isNull());
    refresh();
}
