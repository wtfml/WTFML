#include "controllers/InstanceSettingsController.hpp"
#include <QCoreApplication>

#include "controllers/InstanceMaintenanceController.hpp"
#include "util/AppPaths.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/InstanceMetaFile.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QString>

#include <utility>

namespace {

/// 把用户挑的那张图落成实例目录下的 icon.png（要求：「选一张本地图片文件」）。
///
/// **解码再编码，不是原字节复制**：用户挑的可能是 jpg / webp / bmp，而读的那一侧只认
/// icon.png 这一个名字（见 AppPaths::versionIconPath()）。原字节拷过去的话，盘上就有一个
/// 扩展名写着 .png、内容却是 JPEG 的文件 —— 今天 Qt 按内容认、照样解得开，但那个名字从此
/// 是一句假话，谁照着它去写解析都会踩空。
///
/// NOTE: 走 QSaveFile（写临时文件 → 原子替换）：写到一半失败时盘上留着的要么是原来那张、
/// 要么是新那张，不会是一张半截的图 —— 那东西下次启动会被当成"这张图坏了"，而用户
/// 完全不知道自己哪里做错了。
///
/// @return 写成功了返回 true。失败时填 @p errorOut，调用方**必须把它说出来**：用户刚挑完
/// 图、按了确定，图标没换上却一声不响，正是最难查的那种"没生效"。
///
/// NOTE: 这是这一层里**唯一**一处直接造中文的地方，理由和 `VersionManager::renameInstance()`
/// 那个 `error` 出参一样：它是**数据层的错**（读不出图 / 写不进去 / 提交失败），
/// 是"事实"本身、不是"该怎么说"那一层。调用方原样转述给用户，不加工。
bool writeInstanceIcon(const QString &versionId, const QString &sourcePath, QString *errorOut) {
    const QImage source(sourcePath);
    if (source.isNull()) {
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("无法读取图片：%1\n\n请确认文件存在且格式受支持。")
                            .arg(QDir::toNativeSeparators(sourcePath));
        }
        return false;
    }
    QString lockProblem;
    if (!WriteDirectoryGuard::acquire(AppPaths::versionDir(versionId), &lockProblem)) {
        if (errorOut != nullptr) *errorOut = lockProblem;
        return false;
    }
    QSaveFile file(AppPaths::versionIconPath(versionId));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("无法保存图标：%1").arg(file.errorString());
        }
        return false;
    }
    // QSaveFile 的 commit() 失败时自己会把临时文件撤掉，所以这两句之间不用再清理一遍。
    if (!source.save(&file, "PNG") || !file.commit()) {
        if (errorOut != nullptr) {
            *errorOut = QObject::tr("无法将图标写入 %1：%2")
                            .arg(QDir::toNativeSeparators(AppPaths::versionIconPath(versionId)),
                                 file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace

InstanceSettingsController::InstanceSettingsController(VersionManager *versions,
                                                       InstanceGroupStore *groups,
                                                       LauncherSettings *settings, Host host,
                                                       QObject *parent)
    : QObject(parent), m_versions(versions), m_groups(groups), m_settings(settings),
      m_host(std::move(host)) {}

QString InstanceSettingsController::instanceKeyFor(const VersionManager *versions,
                                                   const QString &versionId) {
    // 有稳定 id 就用它，没有就退回目录名 —— 见 InstanceGroupStore 文件头那段。
    // NOTE: 退回目录名不是"临时凑合"，它是老实例（稳定 id 出现之前建的，一个都没有）
    // 唯一的现实身份。更要紧的是**读和写必须走同一个函数**：读的时候按目录名找、
    // 写的时候按 id 存，归属会在存下去的那一刻就对不上，而那个错不报任何东西。
    if (versions == nullptr) {
        return versionId;
    }
    const QString id = versions->instanceId(versionId);
    return id.isEmpty() ? versionId : id;
}

QString InstanceSettingsController::instanceKey(const QString &versionId) const {
    return instanceKeyFor(m_versions, versionId);
}

bool InstanceSettingsController::rename(const QString &oldId, const QString &newId) {
    const auto notify = [this](Notice::Kind kind, const QString &instanceId,
                               const QString &detail) {
        if (m_host.notice) {
            Notice notice;
            notice.kind = kind;
            notice.instanceId = instanceId;
            notice.detail = detail;
            m_host.notice(notice);
        }
    };

    // 闸口一：**正在跑的不能改名**，而且必须在调 renameInstance() **之前** —— 运行中的
    // 实例在 Windows 上 jar 被 JVM 锁着，改到一半才失败的话用户看到的是一句"拒绝访问"，
    // 而真正该说的是这一句。
    //
    // NOTE: 守卫放在这里而不是入口上：改名有"任何一条点进设置页的路"那么多入口，
    // 守卫放在入口上迟早会漏一条 —— 放在这里，所有调用点（含验证工程）都绕不过去。
    if (m_host.isRunning && m_host.isRunning(oldId)) {
        notify(Notice::Kind::RenameBlockedRunning, oldId, QString());
        return false;
    }
    // 闸口一之二：**正在体检的不能改名**（「同一实例不允许同时扫描和删除/
    // 改名」）。后台那一趟攥着的是改名前算好的那些**绝对路径**，改到一半才让它醒过来的话，
    // 报告里每一条都会指着一个已经不存在的路径（全是假的"缺文件"）。
    if (m_host.isScanning && m_host.isScanning(oldId)) {
        notify(Notice::Kind::RenameBlockedScanning, oldId, QString());
        return false;
    }
    // 闸口一之三：**正被别的作业占着**（安装 / 导入 / 复制 / 快照 / 还原）也不能改名。
    // 那些作业手里攥着的是按下那一刻算好的绝对路径（`AppPaths::versionDir(id)`），
    // 改了名之后它们会往已经不存在的旧目录里写 —— `mkpath()` 会把旧目录**重新建出来**，
    // 于是文件一半在新目录、一半在旧目录，两边都不完整。判据是
    // `MainWindow::installingVersionIds()`（"谁动不得"的唯一出处），不另立一个。
    if (m_host.isFileOperationBusy && m_host.isFileOperationBusy(oldId)) {
        notify(Notice::Kind::RenameBlockedBusy, oldId, QString());
        return false;
    }

    const QString name = newId.trimmed();

    // 名字合不合法。判据和数据层是**同一份**（InstanceFiles::validateRenameTarget），
    // 界面绝不自己抄一遍规则 —— 抄一遍的下场是两边慢慢漂开，出现"界面放行、数据层拒绝"
    // 或者更糟的"界面拒绝、数据层其实能接受"。
    const QString problem = InstanceFiles::validateRenameTarget(name, oldId);
    if (!problem.isEmpty()) {
        // 不循环重问：用户看到原因后可以自己再点一次，那样他能顺便改别的决定。
        notify(Notice::Kind::RenameRejected, oldId, problem);
        return false;
    }

    // 分组归属的**老键必须在改名之前取**：改名会把老目录名连同"按目录名认人"那条
    // 退路一起改掉，改完再问它，问的是磁盘上一个已经不存在的目录。
    const QString oldKey = instanceKey(oldId);

    // 执行：改磁盘、含失败回滚，都在数据层。失败原因是给用户看的一整句
    // （可能带"回滚到什么程度"），界面原样弹出来，不再自己加工。
    QString error;
    if (!m_versions->renameInstance(oldId, name, &error)) {
        // 失败的那一趟里，`renameInstance()` 有可能**已经把稳定 id 写进磁盘**了
        // （先写 id、再挪目录；目录挪不动时 id 留在原地）。目录名没变，但
        // `instanceKey()` 从"老目录名"变成了那个 uuid —— 分组表还挂在老键上，
        // 于是实例看着没动、却从组里掉了出来。所以这里要**重算一次键**，
        // 变了就把分组归属搬过去（和成功那条路同一把 `rekey()`）。
        const QString failedKey = instanceKey(oldId);
        QString groupError;
        if (failedKey != oldKey && m_groups != nullptr
            && !m_groups->rekey(oldKey, failedKey, &groupError)) {
            // 弹窗说的是改名失败；分组这一半也没搬成，再单独说一句。
            notify(Notice::Kind::RenameGroupNotMoved, oldId, groupError);
        }
        notify(Notice::Kind::RenameFailed, name, error);
        // NOTE: 失败可能是"回滚干净了"也可能是"停在半路上"。两种都不在这里重刷列表 ——
        // 刷新只会把界面对着磁盘重画一遍，而用户需要的是先看清弹窗里那句话。
        return false;
    }

    // 改完名字要跟着动的状态：「上次启动」「上次选中」记的是目录名，不跟着改的话下次
    // 开启动器按老 id 找不到，当前实例会落到列表第一个，用户以为刚改名的那个丢了。
    if (m_settings != nullptr && m_settings->lastLaunchedVersion() == oldId) {
        m_settings->setLastLaunchedVersion(name);
    }
    if (m_settings != nullptr && m_settings->lastSelectedVersion() == oldId) {
        m_settings->setLastSelectedVersion(name);
    }

    // 配置回退（`cache/config-backups/<id>`）、资源包顺序备份、存档 level.dat 的
    // 自动备份（格子名里带实例目录名和世界目录绝对路径的哈希）都是按目录名认人的。
    // 不搬的后果很具体：改名之后新名字底下**一个备份都查不到**、清理也够不着它们，
    // 界面上就是"改个名，自动备份全没了"—— 而磁盘上那份其实还在。
    //
    // NOTE: 失败**不算改名失败**。实例本身已经改好了，这些只是缓存；逐条写进日志，
    // 用户真需要那一份备份时至少知道它在哪儿、为什么没搬（多半是目标格子里
    // 已经有另一个曾经同名的实例留下的东西）。
    {
        int movedCaches = 0;
        int movedSlots = 0;
        QStringList problems = InstanceFiles::migrateInstanceCacheDirs(
            oldId, name,
            {AppPaths::configBackupDir(), AppPaths::resourcePackBackupDir()}, &movedCaches);
        problems += InstanceMaintenanceController::migrateWorldBackupSlots(
            AppPaths::versionDir(oldId), AppPaths::versionDir(name), &movedSlots);
        if (movedCaches + movedSlots > 0 && m_host.log) {
            m_host.log(tr("改名时把 %1 处缓存备份搬到了新名字底下")
                           .arg(movedCaches + movedSlots));
        }
        for (const QString &problem : problems) {
            qWarning() << "Cache backup migration after renaming" << oldId << "to" << name
                       << "did not finish:" << problem;
            if (m_host.log) {
                m_host.log(problem);
            }
        }
    }

    // 分组归属也要跟着挪。两件事叠在一起，所以这里必须按顺序取两次键：
    // - 改名**之前**那个键（上面取的 oldKey）：老实例可能还没有稳定 id，
    // 那时它的键是**老目录名**；
    // - 改名**之后**那个键：renameInstance() 刚好在这一次把稳定 id 生成出来了
    // （见它的第 2 步），所以新键多半是那个 uuid，而不是新目录名。
    // 只取一次（无论是哪一次）都会让归属在改名的同时丢掉 —— 而丢归属的表现是
    // "改完名它自己从组里出来了"，用户会以为分组没保存上。
    const QString newKey = instanceKey(name);
    QString groupError;
    if (m_groups != nullptr && !m_groups->rekey(oldKey, newKey, &groupError)) {
        // 实例本身已经改好名了，只是分组没搬过去。**不能当成改名失败** ——
        // 那会让用户以为名字没改成功，然后再点一次。说清楚哪一半成了、哪一半没成。
        notify(Notice::Kind::RenameGroupNotMoved, name, groupError);
    }

    if (m_host.instanceTreeChanged) {
        m_host.instanceTreeChanged();
    }
    // 刷新时原来那个 id 已经不在了，下拉框会顺延到同一位置上的实例（resolveCurrentInstance()）。
    // 显式选中新名字：用户刚改完，看到的应该是刚改完的那个实例。
    if (m_host.instanceRenamed) {
        m_host.instanceRenamed(name);
    }
    if (m_host.log) {
        m_host.log(tr("实例已重命名：%1 → %2").arg(oldId, name));
    }
    return true;
}

bool InstanceSettingsController::apply(const QString &versionId, const Edit &edit,
                                       bool *pickedIconStoredOut) {
    if (pickedIconStoredOut != nullptr) {
        *pickedIconStoredOut = false;
    }
    const auto notify = [this](Notice::Kind kind, const QString &instanceId,
                               const QString &detail) {
        if (m_host.notice) {
            Notice notice;
            notice.kind = kind;
            notice.instanceId = instanceId;
            notice.detail = detail;
            m_host.notice(notice);
        }
    };

    // 分组归属的**老键必须在写 meta 之前取**。下面那一句 `ensureInstanceId()` 会给
    // "还没有稳定 id"的老实例补一个，而归属是按 `instanceKey()`（有 id 用 id、没有就
    // 退回目录名）存的 ⇒ 补上 id 之后同一个实例的键**换了一个**，不跟着搬它就会
    // **静默掉出原来那一组**。用户看到的是"我只改了个内存设置，它自己从组里出来了"，
    // 而且没有任何日志、没有任何弹窗 —— 事后才发现。
    //
    // NOTE: 已经走过改名那条路的实例到这里时 id 早就有了（`renameInstance()` 那一步
    // 生成的），oldKey == newKey —— `rekey()` 自己认得出这是空操作，一个字节
    // 都不写盘（见它开头那一句）。
    const QString oldGroupKey = instanceKey(versionId);

    // 先把稳定 id 落下去。**必须排在最前**：这一项覆盖要"跟着这个实例走"，而实例的
    // 身份就是那个 id（目录名会被改，改名之后按目录名记的东西全丢）。失败就不写 ——
    // 写成一次"靠目录名认出来的覆盖"比这次没写成糟得多。
    if (m_versions->ensureInstanceId(versionId).isEmpty()) {
        notify(Notice::Kind::SettingsIdFailed, versionId, QString());
        return false;
    }

    // 键换过了（老实例刚拿到 id）就把归属搬过去 —— 这一步**必须在这里**，不能挪到
    // 下面那次写 meta 之后：`ensureInstanceId()` 已经把 id 落盘了，也就是说"键已经换了"
    // 是既成事实，再往下走无论成败，归属都只剩这一条路能保住。
    //
    // NOTE: 搬失败**不当成"设置没保存"**（照 applyRename 的先例）：那会让用户以为要重来
    // 一次，而设置那半边马上就写好了。说清楚哪一半没成，让他自己去「分组…」再归一次。
    const QString newGroupKey = instanceKey(versionId);
    QString groupError;
    if (m_groups != nullptr && !m_groups->rekey(oldGroupKey, newGroupKey, &groupError)) {
        notify(Notice::Kind::SettingsGroupNotMoved, versionId, groupError);
    }

    // "值 → 要写进 meta 的字段"整批交给 javaOverrideMetaFields()：五项里"跟随全局"
    // 的要**删键**而不是写一个等于全局的值（写了的话用户之后改全局，这个实例就不跟着
    // 走了，而他明明选的是「跟随全局」），追加参数为空时那个键也要删。这套判断放在
    // settings/ 那边是有意的 —— 在这里就没法被断言盯上（这个函数收的是对话框引用，
    // 验证工程驱动不了它）。
    QJsonObject fields = javaOverrideMetaFields(edit.java, edit.extraJvmArgs,
                                               VersionManager::jvmArgsField());

    // 窗口那三项（模式 / 宽 / 高，）。**同一个约定**（没有值 = 写 Null = 删键），
    // 由 gameWindowOverrideToJson() 一次性给全 —— 那三个键的读侧写侧都在 src/settings/
    // 那一处（键名一个都不在这边重写）。
    // NOTE: 一次一个键合进去（`QJsonObject` 没有整块注入的接口）。值本身是 Null 的那几项
    // 就是"删键"（updateVersionMeta 的约定），照原样交给它。
    const QJsonObject windowFields = gameWindowOverrideToJson(edit.window);
    for (auto it = windowFields.constBegin(); it != windowFields.constEnd(); ++it) {
        fields.insert(it.key(), it.value());
    }

    // 提醒开关：勾着（要提醒）就**删掉**那个键，取消勾选才写 true。
    // 和那五项同一个约定（Null = 删键）—— 写一个"等于默认值"的 false 进 meta 会让这个
    // 文件里多出一堆没信息量的键，而且"没写过"和"写过 false"从此分不开。
    fields.insert(VersionManager::skipJavaVersionWarningField(),
                  edit.warnJavaMismatch ? QJsonValue(QJsonValue::Null) : QJsonValue(true));

    // ---------- 图标----------
    //
    // 用户刚挑了一张图 ⇒ **先把它拷进实例目录**，再往 meta 里写"用本地那张"。顺序反过来的话，
    // 写成功、拷失败会在盘上留下一句指向一个不存在的文件的 meta（树那一格从此永远是默认图标，
    // 而文件里明明写着 local）。
    //
    // NOTE: 拷失败**不把整次保存赔进去**（只报一声、meta 里那一格保持原样）：用户改的内存 /
    // Java 路径和图标是两件事，为前者把后者一起丢掉是替用户做主。写回去的那个值是
    // **磁盘上现在是什么**、不是"这次没改"，所以这一次写入不会动它 —— 而要是直接写
    // `edit.iconChoice`，用户清掉图标就会把盘上那张删了却还以为换成了默认的。
    //
    // `iconBefore` 是**这一次写之前** disk 上那个值，用来回答"这次改动里包含图标吗"
    // （见函数末尾那一发"要不要重扫树"）。
    const QString iconBefore = m_versions->versionIcon(versionId);
    QString iconValue = edit.iconChoice;
    // 写新图之前把**盘上原来那张**收进内存，并记住"这次写没写过图"。
    // 往下 `updateVersionMeta()` 还有一次可能失败的写（盘满 / meta 文件被别的进程开着），
    // 那一步失败时这张刚落地的图**必须退回去**：meta 里还写着老值，而 icon.png 已经换成了
    // 新图 —— 实例树 / 文件页都是照着这个名字读的，用户看到的是"保存失败，但图标已经换了"
    // （再失败一次就再换一次，永久生效）。`writeInstanceIcon()` 走 QSaveFile 原子替换，
    // 老图**只剩内存里这一份**，不提前收就再也找不回来。
    const QString iconPath = AppPaths::versionIconPath(versionId);
    QByteArray iconBackup;
    bool iconBackupValid = false;
    bool iconWritten = false;
    if (!edit.pickedIconPath.isEmpty()) {
        QFile existing(iconPath);
        // 上限 8 MB：icon.png 正常只有几十 KB，读不进去/大得离谱的就当"原来没有"——
        // 那种情况下退回去的动作是删掉新图。
        if (existing.exists() && existing.size() <= 8 * 1024 * 1024
            && existing.open(QIODevice::ReadOnly)) {
            iconBackup = existing.readAll();
            iconBackupValid = iconBackup.size() > 0 && existing.error() == QFileDevice::NoError;
        }
    }
    if (!edit.pickedIconPath.isEmpty()) {
        QString iconError;
        if (writeInstanceIcon(versionId, edit.pickedIconPath, &iconError)) {
            iconValue = edit.localIconValue;
            iconWritten = true;
        } else {
            notify(Notice::Kind::IconNotSaved, versionId, iconError);
            iconValue = m_versions->versionIcon(versionId);
        }
    }
    // 空串 = 「默认」= **删掉那个键**，不写一个"等于默认"的值 —— 和上面两项同一个约定。
    fields.insert(VersionManager::iconField(),
                  iconValue.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(iconValue));

    // ---------- 左栏显示哪几页----------
    //
    // 一个**顶层对象**（`{"save": false}`）—— updateVersionMeta 的合并是"这一格的整块替换"，
    // 所以不用（也不能）逐条删键：整块交上去，那一格就以这份为准。
    // 空对象（七条全显示 = 没勾掉任何一条）写 Null ⇒ **删键**，和上面三项同一个约定。
    //
    // NOTE: 这里**不校验**那几页此刻可不可用 —— 值是从对话框现读的（`edit.pageChoices`），
    // 而它本来就只写"可用且没勾"的那几条（见 `pageChoices()` 那段说明）。可用性是
    // 每次现算的，不写盘。
    fields.insert(VersionManager::settingsPagesField(),
                  edit.pageChoices.isEmpty() ? QJsonValue(QJsonValue::Null)
                                             : QJsonValue(edit.pageChoices));

    // ---------- 什么都没改就别写盘 ----------
    //
    // 这一页是"改一格落一格"：用户打开设置、看一眼、按确定；或者只是把焦点从某一格移开
    // 触发了一次 `editingFinished` —— 都会走到这里。而"要写的每个键，盘上现在的值已经
    // 等于它"时再写一次 meta 只有坏处：把文件 mtime 推新、重扫实例树、还可能把别的进程
    // 刚写进去的东西按我们读到的旧值盖回去。
    //
    // 判据不是"这次碰没碰某一格"，而是**这一整份要写的字段和盘上那份比**（包括
    // `Null` = 删键那一档：盘上本来就没有这个键才算相等）。分组/置顶是另外两条路
    // （见下面那三档），它们各自也会发 `groupsChanged`，所以也得算进"有没有改"。
    const bool groupChanged = edit.chosenGroupId != edit.homeGroupAtOpen;
    const bool wasPinned = m_groups != nullptr && m_groups->isPinned(instanceKey(versionId));
    const bool pinChanged = edit.pinned != wasPinned;
    if (!iconWritten && !groupChanged && !pinChanged
        && InstanceMetaFile::fieldsAlreadyEqual(m_versions->versionMeta(versionId), fields)) {
        return true;
    }

    QString error;
    if (!m_versions->updateVersionMeta(versionId, fields, &error)) {
        if (iconWritten) {
            bool restored = false;
            QString lockProblem;
            if (!WriteDirectoryGuard::acquire(AppPaths::versionDir(versionId), &lockProblem)) {
                qWarning() << "InstanceSettingsController: cannot lock" << versionId
                           << "to restore" << iconPath << lockProblem;
            } else if (iconBackupValid) {
                QSaveFile restore(iconPath);
                restored = restore.open(QIODevice::WriteOnly)
                           && restore.write(iconBackup) == iconBackup.size()
                           && restore.commit();
            } else {
                restored = QFile::remove(iconPath) || !QFile::exists(iconPath);
            }
            if (!restored) {
                qWarning() << "InstanceSettingsController: could not restore" << iconPath
                           << "after the meta write failed";
            }
        }
        // 数据层不弹窗是约定，所以弹在这一层。error 里已经写清了"是哪个文件、为什么"。
        notify(Notice::Kind::SettingsSaveFailed, versionId, error);
        return false;
    }
    // "用户刚挑的那张图真的落进实例目录了" —— 到这里才算真的：上面那次 meta 写失败时
    // 新图已经被退回去了（见那段）。
    if (iconWritten && pickedIconStoredOut != nullptr) {
        *pickedIconStoredOut = true;
    }
    if (m_host.log) {
        m_host.log(tr("实例设置已保存：%1").arg(versionId));
    }

    // 图标换了，就得让实例树上那一格跟着换。`updateVersionMeta()` **没有任何"meta 变了"
    // 的信号**（它是数据层，不认得上层谁在看），而写入 Meta 之后树上那一格的图标是从
    // meta 现读的 —— 不重扫的话用户回到实例页看到的还是老图标，得等下一次下载或者重启。
    //
    // 判据是"写之前那个值和现在盘上那个值一样不一样"，**不是**"这次有没有碰图标那一格"：
    // 实时写盘之后 apply() 每次值一变就会被调一次（改个内存数字也算），
    // 无条件重扫等于每敲一下就全盘扫一遍目录。
    //
    // NOTE: 只有图标这一项在这儿重扫。名字那一项走 renameRequested → rename()，
    // 那边自己会刷。分组/置顶在下面，它们各自走 setHomeGroup / pin / unpin，
    // 那三个会发 groupsChanged 把树重灌。
    if (m_versions->versionIcon(versionId) != iconBefore && m_host.instanceTreeChanged) {
        m_host.instanceTreeChanged();
    }

    // ---------- 分组 / 置顶 ----------
    //
    // 排在写 meta **之后**、`rekey()` **之后**（那一步在上面）：归属的键刚刚才定下来，在它
    // 之前写等于写到旧键上。
    //
    // 三条规则，合起来就是"用户在「分组…」和那颗勾选框上摆成什么样，结果就是什么样"：
    // ① 老家被**用户改过** → 改老家（setHomeGroup：置顶着的那一档只改"老家"那一格，
    // members 继续指着置顶组）；
    // ② 原来没置顶、现在勾上了 → 置顶（记下"置顶前老家在哪一组"，老家那一条不动）；
    // ③ 原来置顶、现在取消了 → 取消置顶（回到记着的那一组）。
    //
    // NOTE: 判"改没改"是拿 `edit.chosenGroupId` 和**打开对话框时**那一格的值
    // （`edit.homeGroupAtOpen`）比，不是拿它和"现在存在哪一组"比 —— 和验证工程驱动
    // 这颗按钮时同一个口径，见 .hpp 里 setGroupChoices() 那段。
    //
    // NOTE: ②③ 和 ① **互不相干**：分组那一格是老家、勾选框只管多摆一条，两者不抢同一格。
    // 没有置顶组时勾选框本来就是灰的（setPinned()），勾不上。
    //
    // NOTE: 三条路都走**主窗口那几个现有入口**（走 `Host`），不在这里另写一遍 wtfml.folders
    // 的读写：它们各自会发 groupsChanged、自己把树重灌，而且"没有置顶组时什么都不做"
    // "认不出来就落未分组"这些判断都已经在那儿了。
    //
    // NOTE: 顺序是**先改老家、再摆置顶**。这一版两种顺序算出来的答案是同一个（理由写在
    // InstanceGroupStore::setInstanceHomeGroup() 上面那段 NOTE 里），钉死它只是为了让
    // 读代码的人不用先在脑子里把那四种组合各跑一遍。
    // `wasPinned` 在上面那条"什么都没改就别写盘"的判据里已经取过了（同一份盘上状态，
    // 拿两次只会多一次 `wtfml.folders` 的读缓存命中）。

    // ① 老家被改过。
    if (groupChanged) {
        if (m_host.setHomeGroup) {
            m_host.setHomeGroup(versionId, edit.chosenGroupId);
        }
    }
    // ② 刚勾上置顶。
    if (edit.pinned && !wasPinned) {
        if (m_host.pin) {
            m_host.pin(versionId);
        }
    }
    // ③ 原来置顶、现在取消了：回到记着的那一组（判据是本函数进来的那一刻，所以上面 ① 改
    // 没改过老家都不影响这一档 —— 换过老家之后再取消置顶，回的就是刚换的那一组）。
    if (wasPinned && !edit.pinned) {
        if (m_host.unpin) {
            m_host.unpin(versionId);
        }
    }
    return true;
}

