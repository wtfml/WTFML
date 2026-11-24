/**
 * @file AgentModPlatform.hpp
 * @brief 模组平台查询与下载地址解析（只接 Modrinth，阻塞执行）。
 *
 * 给 `AgentLauncherHostImpl` 的 `AgentModHost` 用：把 SHA-1 批量问 Modrinth 拿身份与版本
 * 元数据；按项目号/版本号解析下载地址。**只在线程里调用** —— 这里自己建 QNAM、用
 * `HttpClient::send`（内嵌事件循环），和 `InstanceIconLookup` 同一个理由与写法。
 *
 * 复用现成的请求/解析纯函数（`ModpackExport::` / `ResourceCatalog::`），不另写一套响应解析；
 * 地址根与 `InstanceIconLookup` 同一条规矩：验证工程的口子优先，否则官方地址。
 */

#pragma once

#include "agent/AgentModCompat.hpp"
#include "agent/AgentModHost.hpp"

#include <QList>
#include <QString>
#include <QStringList>

namespace AgentModPlatform {

struct LookupQuery {
    QStringList sha1s;
    QString gameVersion;
    QString loader;
    QString apiBase;      ///< 空 = `ResourceCatalog::modrinthApiRoot()`
    qint64 deadlineMs = 180000;
};

struct LookupResult {
    QList<AgentModCompat::PlatformEntry> entries;
    QString problem;      ///< 非空 = 整趟/部分没问成；已问到的条目照样有效
};

/// 阻塞：分批（每批 ≤100）问 `POST {root}/version_files`。
LookupResult lookupModrinth(const LookupQuery &query);

struct TargetQuery {
    QString projectId;
    QString versionId;    ///< 空 = 取最新兼容版本
    QString expectedSha1; ///< 非空时只接受哈希对得上的那个文件
    QString gameVersion;
    QString loader;
    QString apiBase;
};

struct TargetResult {
    AgentHostModTarget target;
    QString problem;
};

/// 阻塞：`GET {root}/project/{id}/version` → 按版本/哈希挑文件。
TargetResult resolveModrinth(const TargetQuery &query);

}  // namespace AgentModPlatform
