/**
 * @file UiLanguage.hpp
 * @brief 界面语言：设置值怎么解析、翻译从哪儿装。
 *
 * 源语言就是中文（代码里的字面量），所以 **zh_CN 不装翻译**、直接用源文；
 * English 装编进资源的 `:/i18n/wtfml_en.qm`（构建时由 lrelease 从
 * `assets/i18n/wtfml_en.ts` 生成，见 CMakeLists 里那一段）。
 *
 * 解析写成纯函数 `resolve()`：给它设置值和系统 UI 语言表，它说该用哪一份。
 * 产品在 `src/main.cpp` 里调它，验证工程也直接调它（不必起窗口）。
 *
 * NOTE: 这个头放在 ui/ 而不是 src/util/：`src/` 可以 include `ui/`，反过来不行
 * （见 AGENTS.md 那套分层）。产品、界面和验证工程三边都要用它。
 */

#pragma once

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTranslator>

namespace UiLanguage {

/// `General/Language` 这一格存什么：`system` / `zh_CN` / `en`。默认跟随系统。
///
/// 界面英文已覆盖主要页面，默认跟随系统 UI 语言。
inline constexpr const char *kSettingsKey = "General/Language";
inline constexpr const char *kSystem = "system";
inline constexpr const char *kChinese = "zh_CN";
inline constexpr const char *kEnglish = "en";
inline constexpr const char *kDefault = kSystem;

/// 归一化：认不出来的一律当默认档。
inline QString normalizedSetting(const QString &raw) {
    const QString value = raw.trimmed();
    if (value.compare(QLatin1String(kChinese), Qt::CaseInsensitive) == 0) return QStringLiteral("zh_CN");
    if (value.compare(QLatin1String(kEnglish), Qt::CaseInsensitive) == 0) return QStringLiteral("en");
    if (value.compare(QLatin1String(kSystem), Qt::CaseInsensitive) == 0) return QStringLiteral("system");
    return QString::fromLatin1(kDefault);
}

/// 盘上那一格现在的值（已归一化）。没设过 = 跟随系统。
inline QString readSetting() {
    return normalizedSetting(
        QSettings().value(QLatin1String(kSettingsKey), QLatin1String(kDefault)).toString());
}

/// 纯函数：这一档到底该用哪一份界面语言。
/// @param setting `system` / `zh_CN` / `en`（别的值按默认档算）。
/// @param systemUiLanguages 系统 UI 语言表，通常是 `QLocale::system().uiLanguages()`。
/// @return `"zh_CN"` 或 `"en"`。
///
/// `system` 跟随系统 UI 语言表。有中文（含繁体、`zh-HK` 等）时用 `zh_CN`，否则用 `en`。
inline QString resolve(const QString &setting, const QStringList &systemUiLanguages) {
    const QString normalized = normalizedSetting(setting);
    if (normalized == QLatin1String(kEnglish)) return QStringLiteral("en");
    if (normalized == QLatin1String(kChinese)) return QStringLiteral("zh_CN");
    for (const QString &language : systemUiLanguages) {
        if (language.startsWith(QLatin1String("zh"), Qt::CaseInsensitive)) return QStringLiteral("zh_CN");
    }
    return QStringLiteral("en");
}

/// 英文翻译在资源里的路径（构建时编进程序，产品与验证工程同一份）。
inline QString englishCatalogPath() { return QStringLiteral(":/i18n/wtfml_en.qm"); }

/// 把英文那一份读进 `translator`。给验证工程自己装/卸时用。
inline bool loadEnglish(QTranslator *translator) {
    return translator != nullptr && translator->load(englishCatalogPath());
}

/// 按 `resolve()` 的结果装翻译。`zh_CN` 什么都不做（源文就是中文）。
/// @return false = 要英文、资源里却没有那一份。这是**构建管线**出了问题，
///         运行期没有补救办法 —— 产品只记一句警告，界面退回中文。
inline bool installTranslation(QCoreApplication &app, const QString &resolved) {
    if (resolved != QLatin1String(kEnglish)) return true;
    auto *translator = new QTranslator(&app);  // 归 app：装上了就活到进程结束
    if (!loadEnglish(translator)) {
        delete translator;
        return false;
    }
    app.installTranslator(translator);
    return true;
}

}  // namespace UiLanguage

