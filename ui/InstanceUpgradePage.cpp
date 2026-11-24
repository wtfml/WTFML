/**
 * @file InstanceUpgradePage.cpp
 * @brief 见头文件：只画状态、只转发动作。
 */

#include "InstanceUpgradePage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "Theme.hpp"
#include "operations/UpgradeSelection.hpp"

namespace {

QLabel *mutedLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    QPalette palette = label->palette();
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlpha(150);
    palette.setColor(QPalette::WindowText, muted);
    label->setPalette(palette);
    label->setWordWrap(true);
    return label;
}

QString categoryDescription(const QString &key) {
    if (key == QStringLiteral("saves")) return QCoreApplication::translate("InstanceUpgradePage", "完整复制世界目录");
    if (key == QStringLiteral("mods")) return QCoreApplication::translate("InstanceUpgradePage", "按兼容性结果替换或跳过");
    if (key == QStringLiteral("config")) return QCoreApplication::translate("InstanceUpgradePage", "config 与 defaultconfigs");
    if (key == QStringLiteral("options")) return QCoreApplication::translate("InstanceUpgradePage", "键位、视频、声音");
    if (key == QStringLiteral("resourcepacks")) return QCoreApplication::translate("InstanceUpgradePage", "文件与启用顺序");
    if (key == QStringLiteral("shaderpacks")) return QCoreApplication::translate("InstanceUpgradePage", "需要渲染器才生效");
    if (key == QStringLiteral("screenshots")) return QCoreApplication::translate("InstanceUpgradePage", "作为普通文件复制");
    if (key == QStringLiteral("schematics")) return QCoreApplication::translate("InstanceUpgradePage", "structures 等结构文件");
    if (key == QStringLiteral("servers")) return QCoreApplication::translate("InstanceUpgradePage", "servers.dat 里的条目");
    return QString();
}

QFrame *card(QWidget *parent, const QString &objectName) {
    auto *frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

QString humanBytes(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    if (bytes < qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace

QString InstanceUpgradePage::sourceNameLabelName() { return QStringLiteral("upgradeSourceName"); }
QString InstanceUpgradePage::sourceFactsLabelName() { return QStringLiteral("upgradeSourceFacts"); }
QString InstanceUpgradePage::targetVersionName() { return QStringLiteral("upgradeTargetVersion"); }
QString InstanceUpgradePage::targetLoaderName() { return QStringLiteral("upgradeTargetLoader"); }
QString InstanceUpgradePage::targetLoaderVersionName() {
    return QStringLiteral("upgradeTargetLoaderVersion");
}
QString InstanceUpgradePage::instanceNameEditName() { return QStringLiteral("upgradeInstanceName"); }
QString InstanceUpgradePage::categoryCheckName(const QString &categoryKey) {
    return QStringLiteral("upgradeCategory_%1").arg(categoryKey);
}
QString InstanceUpgradePage::categoryNoteName(const QString &categoryKey) {
    return QStringLiteral("upgradeCategoryNote_%1").arg(categoryKey);
}
QString InstanceUpgradePage::summaryLabelName() { return QStringLiteral("upgradeSummary"); }
QString InstanceUpgradePage::blockerLabelName() { return QStringLiteral("upgradeBlocker"); }
QString InstanceUpgradePage::reportButtonName() { return QStringLiteral("upgradeReportButton"); }
QString InstanceUpgradePage::historyButtonName() { return QStringLiteral("upgradeHistoryButton"); }
QString InstanceUpgradePage::previewButtonName() { return QStringLiteral("upgradePreviewButton"); }

InstanceUpgradePage::InstanceUpgradePage(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("instanceUpgradePage"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // ---------- 双栏：原实例 / 新实例 ----------
    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);
    {
        QFrame *source = card(this, QStringLiteral("upgradeSourceCard"));
        auto *layout = new QVBoxLayout(source);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(6);
        auto *tag = mutedLabel(source, QStringLiteral("upgradeSourceTag"));
        tag->setText(QCoreApplication::translate("InstanceUpgradePage", "来源实例"));
        layout->addWidget(tag);
        m_sourceName = new QLabel(source);
        m_sourceName->setObjectName(sourceNameLabelName());
        QFont nameFont = m_sourceName->font();
        nameFont.setBold(true);
        m_sourceName->setFont(Theme::textFont(nameFont, Theme::TextRole::Heading));
        layout->addWidget(m_sourceName);
        m_sourceFacts = mutedLabel(source, sourceFactsLabelName());
        layout->addWidget(m_sourceFacts);
        layout->addStretch();
        columns->addWidget(source, 1);
    }
    {
        QFrame *target = card(this, QStringLiteral("upgradeTargetCard"));
        auto *layout = new QVBoxLayout(target);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(8);
        auto *tag = mutedLabel(target, QStringLiteral("upgradeTargetTag"));
        tag->setText(QCoreApplication::translate("InstanceUpgradePage", "目标实例"));
        layout->addWidget(tag);
        m_targetVersion = new QComboBox(target);
        m_targetVersion->setObjectName(targetVersionName());
        m_targetVersion->setEditable(false);
        layout->addWidget(m_targetVersion);
        m_targetLoader = new QComboBox(target);
        m_targetLoader->setObjectName(targetLoaderName());
        layout->addWidget(m_targetLoader);
        m_targetLoaderVersion = new QComboBox(target);
        m_targetLoaderVersion->setObjectName(targetLoaderVersionName());
        layout->addWidget(m_targetLoaderVersion);
        m_instanceName = new QLineEdit(target);
        m_instanceName->setObjectName(instanceNameEditName());
        m_instanceName->setPlaceholderText(QCoreApplication::translate("InstanceUpgradePage", "新实例的名字"));
        InputSizing::shortValue(m_instanceName, InputSizing::kNameChars, InputSizing::Script::Han);
        layout->addWidget(m_instanceName);
        layout->addStretch();
        columns->addWidget(target, 1);
    }
    root->addLayout(columns);

    // ---------- 报告 / 历史两个入口 ----------
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        m_report = new QPushButton(QCoreApplication::translate("InstanceUpgradePage", "兼容性报告…"), this);
        m_report->setObjectName(reportButtonName());
        Theme::markButton(m_report, Theme::ButtonKind::Neutral);
        m_history = new QPushButton(QCoreApplication::translate("InstanceUpgradePage", "更新记录…"), this);
        m_history->setObjectName(historyButtonName());
        Theme::markButton(m_history, Theme::ButtonKind::Neutral);
        row->addWidget(m_report);
        row->addWidget(m_history);
        row->addStretch();
        root->addLayout(row);
        connect(m_report, &QPushButton::clicked, this, &InstanceUpgradePage::reportRequested);
        connect(m_history, &QPushButton::clicked, this, &InstanceUpgradePage::historyRequested);
    }

    // ---------- 九类选择（三列） ----------
    {
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(8);
        int index = 0;
        for (const UpgradeSelection::Category &category : UpgradeSelection::categories()) {
            auto *cell = new QWidget(this);
            auto *layout = new QVBoxLayout(cell);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            // 九类是一组「选哪几样」⇒ chip（步骤 133），不排一列开关；说明照旧压在下面一行。
            auto *check = new QPushButton(category.label, cell);
            check->setObjectName(categoryCheckName(category.key));
            check->setProperty("categoryKey", category.key);
            check->setCheckable(true);
            Theme::markChipButton(check);
            layout->addWidget(check, 0, Qt::AlignLeft);
            auto *note = mutedLabel(cell, categoryNoteName(category.key));
            note->setText(categoryDescription(category.key));
            layout->addWidget(note);
            grid->addWidget(cell, index / 3, index % 3);
            m_checks.append(check);
            const QString key = category.key;
            connect(check, &QPushButton::toggled, this, [this, key](bool on) {
                emit categoryToggled(key, on);
            });
            ++index;
        }
        root->addLayout(grid);
    }

    // ---------- 底部摘要 ----------
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(10);
        m_summary = new QLabel(this);
        m_summary->setObjectName(summaryLabelName());
        m_blocker = mutedLabel(this, blockerLabelName());
        m_preview = new QPushButton(QCoreApplication::translate("InstanceUpgradePage", "预览并确认迁移"), this);
        m_preview->setObjectName(previewButtonName());
        Theme::markButton(m_preview, Theme::ButtonKind::Primary);
        row->addWidget(m_summary);
        row->addWidget(m_blocker, 1);
        row->addWidget(m_preview);
        root->addLayout(row);
        connect(m_preview, &QPushButton::clicked, this, &InstanceUpgradePage::previewRequested);
    }

    // 用户动作原样转发（状态由调用方改完再 setState 送回来）。
    connect(m_targetVersion, &QComboBox::currentTextChanged, this,
            &InstanceUpgradePage::targetVersionChanged);
    connect(m_targetLoader, &QComboBox::currentIndexChanged, this, [this](int) {
        emit targetLoaderChanged(m_targetLoader->currentData().toString(),
                                 m_targetLoaderVersion->currentText());
    });
    connect(m_targetLoaderVersion, &QComboBox::currentTextChanged, this, [this](const QString &version) {
        emit targetLoaderChanged(m_targetLoader->currentData().toString(), version);
    });
    connect(m_instanceName, &QLineEdit::textEdited, this, &InstanceUpgradePage::instanceNameEdited);

    rebuildSummary();
}

void InstanceUpgradePage::setVersionOptions(const QStringList &gameVersions) {
    const QSignalBlocker blocker(m_targetVersion);
    const QString current = m_targetVersion->currentText();
    m_targetVersion->clear();
    m_targetVersion->addItems(gameVersions);
    const int index = m_targetVersion->findText(current);
    if (index >= 0) m_targetVersion->setCurrentIndex(index);
}

void InstanceUpgradePage::setLoaderOptions(const QList<QPair<QString, QString>> &loaders,
                                           const QStringList &loaderVersions) {
    {
        const QSignalBlocker blocker(m_targetLoader);
        const QString current = m_targetLoader->currentData().toString();
        m_targetLoader->clear();
        for (const auto &loader : loaders) {
            m_targetLoader->addItem(loader.second, loader.first);
        }
        const int index = m_targetLoader->findData(current);
        if (index >= 0) m_targetLoader->setCurrentIndex(index);
    }
    {
        const QSignalBlocker blocker(m_targetLoaderVersion);
        const QString current = m_targetLoaderVersion->currentText();
        m_targetLoaderVersion->clear();
        m_targetLoaderVersion->addItems(loaderVersions);
        const int index = m_targetLoaderVersion->findText(current);
        if (index >= 0) m_targetLoaderVersion->setCurrentIndex(index);
    }
}

void InstanceUpgradePage::setState(const UpgradePageState::State &state) {
    m_state = state;
    {
        const QSignalBlocker blocker(m_sourceName);
        m_sourceName->setText(state.sourceId);
        QStringList facts;
        if (!state.sourceGameVersion.isEmpty()) facts.append(state.sourceGameVersion);
        if (!state.sourceLoaderKey.isEmpty()) {
            facts.append(state.sourceLoaderVersion.isEmpty()
                             ? state.sourceLoaderKey
                             : QStringLiteral("%1 %2").arg(state.sourceLoaderKey,
                                                           state.sourceLoaderVersion));
        }
        m_sourceFacts->setText(facts.join(QStringLiteral(" · ")));
    }
    {
        const QSignalBlocker blockVersion(m_targetVersion);
        const QSignalBlocker blockLoader(m_targetLoader);
        const QSignalBlocker blockLoaderVersion(m_targetLoaderVersion);
        const QSignalBlocker blockName(m_instanceName);
        if (!state.target.gameVersion.isEmpty()) {
            const int index = m_targetVersion->findText(state.target.gameVersion);
            if (index >= 0) m_targetVersion->setCurrentIndex(index);
        }
        const int loaderIndex = m_targetLoader->findData(state.target.loaderKey);
        if (loaderIndex >= 0) m_targetLoader->setCurrentIndex(loaderIndex);
        if (!state.target.loaderVersion.isEmpty()) {
            const int versionIndex = m_targetLoaderVersion->findText(state.target.loaderVersion);
            if (versionIndex >= 0) m_targetLoaderVersion->setCurrentIndex(versionIndex);
        }
        m_instanceName->setText(state.target.instanceName);
    }
    for (QPushButton *check : m_checks) {
        const QSignalBlocker blocker(check);
        const QString key = check->property("categoryKey").toString();
        check->setChecked(state.selection.state.contains(key));
    }
    rebuildSummary();
}

void InstanceUpgradePage::setCheckPending(bool pending) {
    m_checkPending = pending;
    if (m_report != nullptr) {
        m_report->setEnabled(!pending);
        m_report->setToolTip(pending ? tr("正在检查兼容性，请稍候…") : QString());
    }
    rebuildSummary();
}

void InstanceUpgradePage::setSelectionVisible(bool visible) {
    for (QPushButton *check : m_checks) {
        check->setVisible(visible);
        if (auto *note = findChild<QLabel *>(categoryNoteName(check->property("categoryKey").toString()))) {
            note->setVisible(visible);
        }
    }
}

void InstanceUpgradePage::setHistoryVisible(bool visible) {
    if (m_history != nullptr) {
        m_history->setVisible(visible);
    }
}

void InstanceUpgradePage::setAmounts(int files, qint64 bytes) {
    m_amountFiles = files;
    m_amountBytes = bytes;
    rebuildSummary();
}

void InstanceUpgradePage::setCategoryLinks(const QHash<QString, int> &links) {
    m_categoryLinks = links;
    for (const UpgradeSelection::Category &category : UpgradeSelection::categories()) {
        auto *note = findChild<QLabel *>(categoryNoteName(category.key));
        if (note == nullptr) {
            continue;
        }
        QString text = note->text();
        // 去掉上一次追加的那一段，再按这次的数量接上。
        const QString suffix = QCoreApplication::translate("InstanceUpgradePage", " · 含 %1 个链接");
        const int at = text.indexOf(QCoreApplication::translate("InstanceUpgradePage", " · 含 "));
        if (at >= 0) {
            text = text.left(at);
        }
        const int count = links.value(category.key);
        if (count > 0) {
            text += suffix.arg(count) + QCoreApplication::translate("InstanceUpgradePage", "（不会跟随）");
        }
        note->setText(text);
    }
    rebuildSummary();
}

void InstanceUpgradePage::setCategoryAmounts(
    const QHash<QString, QPair<int, qint64>> &amounts) {
    m_categoryAmounts = amounts;
    for (const UpgradeSelection::Category &category : UpgradeSelection::categories()) {
        auto *note = findChild<QLabel *>(categoryNoteName(category.key));
        if (note == nullptr) {
            continue;
        }
        QString text = categoryDescription(category.key);
        if (amounts.contains(category.key)) {
            const auto amount = amounts.value(category.key);
            if (amount.first <= 0) {
                text += QCoreApplication::translate("InstanceUpgradePage", " · 没有");
            } else {
                text += QCoreApplication::translate("InstanceUpgradePage", " · %1 个 · %2")
                            .arg(amount.first)
                            .arg(humanBytes(amount.second));
            }
        }
        note->setText(text);
    }
    rebuildSummary();
}

void InstanceUpgradePage::rebuildSummary() {
    const int count = m_state.selection.state.ordered().size();
    QString summary = count > 0 ? QCoreApplication::translate("InstanceUpgradePage", "已选 %1 类").arg(count)
                                : QCoreApplication::translate("InstanceUpgradePage", "未选择迁移内容");
    if (!m_categoryAmounts.isEmpty()) {
        int files = 0;
        qint64 bytes = 0;
        for (const QString &key : m_state.selection.state.ordered()) {
            const auto amount = m_categoryAmounts.value(key);
            files += amount.first;
            bytes += amount.second;
        }
        if (files > 0) {
            summary += QCoreApplication::translate("InstanceUpgradePage", " · %1 个文件 · %2").arg(files).arg(humanBytes(bytes));
        }
    } else if (m_amountFiles > 0) {
        summary += QCoreApplication::translate("InstanceUpgradePage", " · %1 个文件 · %2")
                       .arg(m_amountFiles)
                       .arg(humanBytes(m_amountBytes));
    }
    if (m_checkPending) {
        summary += QCoreApplication::translate("InstanceUpgradePage", " · 正在检查兼容性…");
    }
    m_summary->setText(summary);
    if (m_preview != nullptr) {
        m_preview->setEnabled(!m_checkPending);
    }

    const UpgradePageState::Blocker blocker = UpgradePageState::firstBlocker(m_state);
    m_blocker->setText(blocker == UpgradePageState::Blocker::InstanceBusy
                           && !m_state.instanceBusyReason.isEmpty()
                       ? m_state.instanceBusyReason
                       : UpgradePageState::blockerText(blocker));
    m_preview->setEnabled(blocker == UpgradePageState::Blocker::None);
}






