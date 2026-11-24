/**
 * @file PresetCompatibility.cpp
 * @brief `PresetCompatibility.hpp` 的实现（本地筛选 → 缓存 → 有限并发查询）。
 */

#include "PresetCompatibility.hpp"
#include <QCoreApplication>
#include <utility>

#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>

namespace Presets {

QString normalizeCompatibilityLoader(const QString &raw) {
    const QString value = raw.trimmed().toLower();
    if (value.isEmpty() || value == QLatin1String("vanilla") || value == QLatin1String("none") ||
        value == QLatin1String("original") || value == QCoreApplication::translate("PresetCompatibility", "原版")) {
        return QString();
    }
    // `neo forge` / `neo-forge` / `neoforge` 是同一个东西的三种写法。别的新写法不猜。
    if (value == QLatin1String("neoforge") || value == QLatin1String("neo forge") ||
        value == QLatin1String("neo-forge")) {
        return QStringLiteral("neoforge");
    }
    if (value == QLatin1String("fabric-loader")) return QStringLiteral("fabric");
    if (value == QLatin1String("quilt-loader")) return QStringLiteral("quilt");
    return value;
}

QString CompatibilityContext::cacheKey() const {
    // 次序和分隔符都是格式的一部分（缓存键只在本进程里用，但验证工程会读它）。
    return QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(gameVersion.trimmed().toLower(),
             normalizeCompatibilityLoader(loader),
             clientSide ? QStringLiteral("client") : QStringLiteral("server"),
             releaseFilter.trimmed().toLower(), QString::number(dataVersion), providerScope);
}

QString compatibilityStateKey(CompatibilityState state) {
    switch (state) {
    case CompatibilityState::Pending:
        return QStringLiteral("pending");
    case CompatibilityState::Checking:
        return QStringLiteral("checking");
    case CompatibilityState::Matched:
        return QStringLiteral("matched");
    case CompatibilityState::Missing:
        return QStringLiteral("missing");
    case CompatibilityState::Unknown:
        return QStringLiteral("unknown");
    case CompatibilityState::Invalid:
        return QStringLiteral("invalid");
    }
    return QStringLiteral("pending");
}

QString CompatibilitySummary::text() const {
    if (total == 0) {
        return QCoreApplication::translate("PresetCompatibility", "无待检查的模组引用。");
    }
    QStringList parts;
    if (pending > 0) {
        parts.append(QCoreApplication::translate("PresetCompatibility", "检查中 %1/%2").arg(finished()).arg(total));
        // 查的过程中也把**已经确认**的那几档说出来：用户不必等全部跑完才知道缺东西。
        if (missing > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "已确认缺失 %1").arg(missing));
        }
        if (unknown > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "暂无法确认 %1").arg(unknown));
        }
        if (invalid > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "引用无法识别 %1").arg(invalid));
        }
    } else if (missing == 0 && unknown == 0 && invalid == 0) {
        parts.append(QCoreApplication::translate("PresetCompatibility", "全部匹配（%1）").arg(matched));
    } else {
        // 「缺失」与「暂无法确认」**分开说**（验收：API 失败不算缺失）。
        if (missing > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "缺失 %1").arg(missing));
        }
        if (unknown > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "暂无法确认 %1").arg(unknown));
        }
        if (invalid > 0) {
            parts.append(QCoreApplication::translate("PresetCompatibility", "引用无法识别 %1").arg(invalid));
        }
    }
    if (requiredMissing > 0) {
        parts.append(QCoreApplication::translate("PresetCompatibility", "必要项缺失 %1").arg(requiredMissing));
    }
    return parts.join(QCoreApplication::translate("PresetCompatibility", "，")) + QCoreApplication::translate("PresetCompatibility", "。");
}

CompatibilityState localCompatibilityCheck(const ModReference &reference,
                                           const CompatibilityContext &context,
                                           QString *detail) {
    const auto report = [detail](CompatibilityState state, const QString &why) {
        if (detail != nullptr) {
            *detail = why;
        }
        return state;
    };
    if (normalizeModPlatform(reference.platform).isEmpty()) {
        return report(CompatibilityState::Invalid,
                      QCoreApplication::translate("PresetCompatibility", "仅支持 Modrinth 或 CurseForge 平台。"));
    }
    if (reference.projectId.isEmpty()) {
        return report(CompatibilityState::Invalid, QCoreApplication::translate("PresetCompatibility", "此条缺少项目 id。"));
    }
    if (!reference.gameVersion.isEmpty() &&
        reference.gameVersion.compare(context.gameVersion.trimmed(), Qt::CaseInsensitive) != 0) {
        return report(CompatibilityState::Missing,
                      QCoreApplication::translate("PresetCompatibility", "预设要求 %1，本次安装的是 %2。")
                          .arg(reference.gameVersion,
                               context.gameVersion.isEmpty() ? QCoreApplication::translate("PresetCompatibility", "（未指定）")
                                                            : context.gameVersion));
    }
    if (!reference.loader.isEmpty()) {
        const QString want = normalizeCompatibilityLoader(reference.loader);
        const QString have = normalizeCompatibilityLoader(context.loader);
        if (want != have) {
            return report(CompatibilityState::Missing,
                          QCoreApplication::translate("PresetCompatibility", "预设要求 %1，本次安装的是 %2。")
                              .arg(reference.loader,
                                   context.loader.isEmpty() ? QCoreApplication::translate("PresetCompatibility", "原版")
                                                            : context.loader));
        }
    }
    return CompatibilityState::Pending;
}

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------

CompatibilityCache::CompatibilityCache(qint64 ttlMs) : m_ttlMs(ttlMs) {}

QString CompatibilityCache::keyFor(const ModReference &reference,
                                   const CompatibilityContext &context) {
    return context.cacheKey() + QLatin1Char('#') + reference.key();
}

bool CompatibilityCache::lookup(const QString &key, Entry *entry, bool *stale) const {
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd()) {
        return false;
    }
    if (entry != nullptr) {
        *entry = it.value();
    }
    if (stale != nullptr) {
        const qint64 age = QDateTime::currentMSecsSinceEpoch() - it.value().stampMs;
        *stale = m_ttlMs <= 0 || age > m_ttlMs;
    }
    return true;
}

void CompatibilityCache::insert(const QString &key, CompatibilityState state,
                                const QString &detail, qint64 nowMs) {
    // 只缓存"平台给了准话"的那两档。`Unknown` 缓存起来会把一次断网变成十分钟的假结论。
    if (key.isEmpty() || m_ttlMs <= 0 ||
        (state != CompatibilityState::Matched && state != CompatibilityState::Missing)) {
        return;
    }
    Entry entry;
    entry.state = state;
    entry.detail = detail;
    entry.stampMs = nowMs > 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    m_entries.insert(key, entry);
}

// ---------------------------------------------------------------------------
// 检查器
// ---------------------------------------------------------------------------

CompatibilityChecker::CompatibilityChecker(CompatibilityCache *cache, QObject *parent)
    : QObject(parent), m_cache(cache) {
    if (m_cache != nullptr) {
        m_options.cacheTtlMs = m_cache->ttlMs();
    }
}

void CompatibilityChecker::setProvider(CompatibilityProvider *provider) {
    cancel();
    m_provider = provider;
}

CompatibilityChecker::~CompatibilityChecker() { cancel(); }

void CompatibilityChecker::setOptions(const Options &options) {
    m_options = options;
    m_options.maxInFlight = qBound(1, options.maxInFlight, 16);
    m_options.timeoutMs = qBound(1, options.timeoutMs, 600000);
    m_options.totalTimeoutMs = qBound(1, options.totalTimeoutMs, 600000);
    if (m_cache != nullptr) {
        m_cache->setTtlMs(options.cacheTtlMs);
    }
}

void CompatibilityChecker::dropLiveQueries() {
    const auto cancellations = std::exchange(m_cancellations, {});
    m_liveQueries.clear();
    m_inFlight = 0;
    for (Item &item : m_items) {
        item.queued = false;
    }
    m_queue.clear();
    for (const auto &cancel : cancellations) if (cancel) cancel();
}

void CompatibilityChecker::start(const QList<ModReference> &references,
                                 const CompatibilityContext &context) {
    // 新一代：旧的在途回调全部作废（generation 对不上就丢，见 beginQuery 里那个 lambda）。
    ++m_generation;
    dropLiveQueries();
    m_items.clear();
    m_indexByKey.clear();
    m_context = context;
    m_context.releaseFilter = context.releaseFilter.trimmed().toLower();
    if (m_provider) m_context.providerScope = m_provider->cacheScope();
    m_running = true;
    m_finishedEmitted = false;

    resolveLocalAndQueue(references, m_context);
    emitUpdate();
    pump();
    finishIfDone();
    const int generation = m_generation;
    QTimer::singleShot(m_options.totalTimeoutMs, this, [this, generation] {
        if (generation != m_generation || !m_running) return;
        dropLiveQueries();
        for (auto &item : m_items) {
            if (item.state != CompatibilityState::Pending && item.state != CompatibilityState::Checking) continue;
            item.state = CompatibilityState::Unknown;
            item.detail = QCoreApplication::translate("PresetCompatibility", "本次检查已到截止时间，暂无法确认；请重试未完成项。");
        }
        emitUpdate();
        finishIfDone();
    });
}

void CompatibilityChecker::resolveLocalAndQueue(const QList<ModReference> &references,
                                                const CompatibilityContext &context) {
    for (const ModReference &reference : references) {
        QString localDetail;
        const CompatibilityState local =
            localCompatibilityCheck(reference, context, &localDetail);
        const QString key = reference.key();

        if (key.isEmpty()) {
            // 坏引用不参与去重（两条坏引用是两件要如实报出来的事）。
            Item item;
            item.reference = reference;
            item.required = reference.required;
            item.state = CompatibilityState::Invalid;
            item.detail = localDetail.isEmpty() ? QCoreApplication::translate("PresetCompatibility", "引用不完整。") : localDetail;
            m_items.append(item);
            continue;
        }

        const auto existing = m_indexByKey.constFind(key);
        if (existing != m_indexByKey.constEnd()) {
            Item &item = m_items[existing.value()];
            // 同一项目出现两次：必要项取"或"（只要有一处要求必要，就是必要项）。
            item.required = item.required || reference.required;
            // 本地判死的那一条更硬：能本地判的结论优先于"还在排队等网络"。
            if (local != CompatibilityState::Pending && item.state == CompatibilityState::Pending) {
                item.state = local;
                item.detail = localDetail;
                item.queued = false;
            }
            continue;
        }

        Item item;
        item.reference = reference;
        item.key = key;
        item.required = reference.required;
        if (local != CompatibilityState::Pending) {
            item.state = local;
            item.detail = localDetail;
        } else if (m_cache != nullptr && m_options.cacheTtlMs > 0) {
            CompatibilityCache::Entry entry;
            bool stale = true;
            if (m_cache->lookup(CompatibilityCache::keyFor(reference, context), &entry, &stale) &&
                !stale) {
                item.state = entry.state;
                item.detail = entry.detail.isEmpty()
                                  ? QCoreApplication::translate("PresetCompatibility", "缓存结论。")
                                  : entry.detail + QCoreApplication::translate("PresetCompatibility", "（缓存结论）");
                item.fromCache = true;
            }
        }

        if (item.state == CompatibilityState::Pending) {
            if (m_provider == nullptr) {
                // 没有查询实现（测试替身没装 / 这一趟不联网）：如实报"暂时无法确认"，
                // 不谎报缺失，也不把弹窗挂住。
                item.state = CompatibilityState::Unknown;
                item.detail = QCoreApplication::translate("PresetCompatibility", "资源平台查询不可用，暂时无法确认兼容性。");
            } else if (context.gameVersion.trimmed().isEmpty()) {
                item.state = CompatibilityState::Unknown;
                item.detail = QCoreApplication::translate("PresetCompatibility", "未指定游戏版本，暂时无法确认兼容性。");
            } else {
                item.queued = true;
                m_queue.enqueue(static_cast<int>(m_items.size()));
            }
        }

        m_indexByKey.insert(key, static_cast<int>(m_items.size()));
        m_items.append(item);
    }
}

void CompatibilityChecker::pump() {
    const int limit = std::max(1, m_options.maxInFlight);
    while (m_inFlight < limit && !m_queue.isEmpty()) {
        const int index = m_queue.dequeue();
        if (index < 0 || index >= m_items.size()) {
            continue;
        }
        Item &item = m_items[index];
        if (!item.queued || item.state != CompatibilityState::Pending) {
            continue;
        }
        item.queued = false;
        beginQuery(index);
    }
}

void CompatibilityChecker::beginQuery(int index) {
    Item &item = m_items[index];
    item.state = CompatibilityState::Checking;
    const int generation = m_generation;
    const int queryId = m_nextQueryId++;
    m_liveQueries.insert(queryId);
    ++m_inFlight;

    // 超时：到点按 `Unknown` 报。迟到的平台回调会因为 queryId/generation 对不上被丢掉。
    QTimer::singleShot(m_options.timeoutMs, this, [this, generation, queryId, index]() {
        if (generation != m_generation || !m_liveQueries.contains(queryId)) {
            return;
        }
        m_liveQueries.remove(queryId);
        --m_inFlight;
        const auto cancel = m_cancellations.take(queryId);
        if (cancel) cancel();
        applyResult(index, CompatibilityState::Unknown,
                    QCoreApplication::translate("PresetCompatibility", "查询超时（%1 秒），暂时无法确认。")
                        .arg(std::max(1, m_options.timeoutMs / 1000)),
                    false);
    });

    const ModReference reference = item.reference;
    // 检查器可能比提供方先没（关闭那一页 / 换一份预设时整个弹窗拆掉）。提供方手里
    // 那发 `Reply` 之后还可能被调用 —— 直接 `invokeMethod(this, …)` 就是对已析构对象的
    // 一次跳转（「关闭弹窗」那一格）。`QPointer` 在对象析构时自动置空，晚到的那一发
    // 到这里就只是一次空判断。
    const QPointer<CompatibilityChecker> guard(this);
    auto cancel = m_provider->queryCancelable(reference, m_context,
                      [this, guard, generation, queryId, index](CompatibilityState state,
                                                                const QString &detail) {
                          if (guard.isNull()) {
                              return;
                          }
                          // 一律转成队列调用：提供方同步回调也不会在 pump 的循环里重入。
                          QMetaObject::invokeMethod(
                              this,
                              [this, guard, generation, queryId, index, state, detail]() {
                                  if (guard.isNull() || generation != m_generation ||
                                      !m_liveQueries.contains(queryId)) {
                                      return;
                                  }
                                  m_liveQueries.remove(queryId);
                                  m_cancellations.remove(queryId);
                                  --m_inFlight;
                                  // 提供方只该回那三种；真回了别的，按 `Unknown` 算 ——
                                  // 既不是"匹配"也不是"缺失"，不能拿去下结论。
                                  const CompatibilityState usable =
                                      (state == CompatibilityState::Matched ||
                                       state == CompatibilityState::Missing)
                                          ? state
                                          : CompatibilityState::Unknown;
                                  applyResult(index, usable, detail, false);
                              },
                              Qt::QueuedConnection);
                      });
    if (cancel) m_cancellations.insert(queryId, std::move(cancel));
}

void CompatibilityChecker::applyResult(int index, CompatibilityState state,
                                       const QString &detail, bool fromCache) {
    if (index < 0 || index >= m_items.size()) {
        return;
    }
    Item &item = m_items[index];
    item.state = state;
    item.detail = detail;
    item.fromCache = item.fromCache || fromCache;
    if (m_cache != nullptr && !item.fromCache && !item.key.isEmpty()) {
        m_cache->insert(CompatibilityCache::keyFor(item.reference, m_context), state, detail);
    }
    emitUpdate();
    pump();
    finishIfDone();
}

CompatibilitySummary CompatibilityChecker::computeSummary() const {
    CompatibilitySummary summary;
    summary.total = static_cast<int>(m_items.size());
    for (const Item &item : m_items) {
        switch (item.state) {
        case CompatibilityState::Pending:
        case CompatibilityState::Checking:
            ++summary.pending;
            break;
        case CompatibilityState::Matched:
            ++summary.matched;
            break;
        case CompatibilityState::Missing:
            ++summary.missing;
            if (item.required) {
                ++summary.requiredMissing;
            }
            break;
        case CompatibilityState::Unknown:
            ++summary.unknown;
            break;
        case CompatibilityState::Invalid:
            ++summary.invalid;
            break;
        }
    }
    return summary;
}

CompatibilitySummary CompatibilityChecker::summary() const {
    return computeSummary();
}

QList<CompatibilityResult> CompatibilityChecker::results() const {
    QList<CompatibilityResult> out;
    out.reserve(m_items.size());
    for (const Item &item : m_items) {
        CompatibilityResult result;
        result.key = item.key;
        result.platform = item.reference.platform;
        result.projectId = item.reference.projectId;
        result.name = item.reference.name;
        result.required = item.required;
        result.state = item.state;
        result.detail = item.detail;
        result.fromCache = item.fromCache;
        out.append(result);
    }
    return out;
}

void CompatibilityChecker::emitUpdate() {
    emit updated(results(), computeSummary());
}

void CompatibilityChecker::finishIfDone() {
    if (!m_running || m_finishedEmitted) {
        return;
    }
    if (!computeSummary().isFinished()) {
        return;
    }
    m_finishedEmitted = true;
    m_running = false;
    emit finished(results(), computeSummary());
}

void CompatibilityChecker::cancel() {
    ++m_generation;
    dropLiveQueries();
    m_running = false;
}

}  // namespace Presets
