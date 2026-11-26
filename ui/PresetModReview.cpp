#include "PresetModReview.hpp"
#include "Theme.hpp"
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

PresetModReview::PresetModReview(const Presets::ModInstallPlan &plan, QWidget *parent)
    : ChromeDialog(parent), m_plan(plan) {
    setObjectName("presetModReview");
    setWindowTitle(tr("确认模组预设"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 540);
    auto *layout = new QVBoxLayout(body());
    auto *summary = new QLabel(Presets::modInstallSummaryText(plan), this);
    summary->setWordWrap(true); layout->addWidget(summary);
    auto *hint = new QLabel(tr("替代项目仅本次有效，需重新检查必要前置。"), this);
    hint->setWordWrap(true); layout->addWidget(hint);
    if (!plan.optionalDependencyKeys.isEmpty()) {
        auto *optional = new QLabel(tr("可选前置（不会自动加入）：%1").arg(plan.optionalDependencyKeys.join(tr("、"))), this);
        optional->setWordWrap(true); layout->addWidget(optional);
    }
    m_rows = new QTreeWidget(this);
    m_rows->setObjectName("presetModReviewRows");
    m_rows->setHeaderLabels({tr("模组"), tr("版本"), tr("状态"), tr("文件"), tr("大小"), tr("替代项目")});
    m_rows->setRootIsDecorated(false); m_rows->setUniformRowHeights(true);
    bool hasReplacementInput = false;
    for (const auto &row : plan.rows) {
        const QString title = (row.replacesKeys.isEmpty() ? QString() : row.replacesKeys.join(tr("、")) + QStringLiteral(" → "))
            + row.title + (row.fromDependency ? tr("（必要前置）") : QString());
        const QString release = row.version.versionNumber.isEmpty() ? row.version.id
            : row.version.versionNumber + QStringLiteral(" / ") + row.version.id;
        auto *item = new QTreeWidgetItem(m_rows, {title,
            release, Presets::modInstallStatusText(row.status), row.file.filename,
            row.file.size >= 0 ? QLocale().formattedDataSize(row.file.size, 1, QLocale::DataSizeSIFormat) : QString()});
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);  // 大小是数字列，表头跟着右对齐
        item->setToolTip(3, tr("下载后校验 SHA-1：%1").arg(row.file.sha1));
        item->setToolTip(0, row.key);
        item->setToolTip(2, row.problem + QLatin1Char('\n') + row.details.join(QLatin1Char('\n')));
        if (!row.problem.isEmpty()) item->setText(2, row.problem);
        if (row.status == Presets::ModInstallStatus::Missing || row.status == Presets::ModInstallStatus::AlternativeNeedsConfirm) {
            hasReplacementInput = true;
            auto *edit = new QLineEdit(m_rows); edit->setPlaceholderText("modrinth:sodium");
            edit->setToolTip(tr("格式：modrinth:项目ID 或 curseforge:数字ID"));
            edit->setProperty("originalKey", row.key); m_rows->setItemWidget(item, 5, edit);
        }
    }
    hint->setVisible(hasReplacementInput);
    m_rows->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_rows->header()->setStretchLastSection(true);
    const int widths[] = {130, 165, 200, 170, 70};  // 末列（替代项目）吃剩下的
    for (int column = 0; column < 5; ++column) m_rows->setColumnWidth(column, widths[column]);
    layout->addWidget(m_rows, 1);
    m_problem = new QLabel(this); m_problem->setWordWrap(true); layout->addWidget(m_problem);
    // 不用标准钮（QDialogButtonBox::Save/Cancel/…）：它的字在 LanguageChange 时会被 Qt 刷回自带的英文，
    // 装没装 Qt 中文翻译都一样不稳。自己 addButton(字, 角色)，accepted/rejected 照常发。
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *confirmButton = buttons->addButton(tr("确认安装"), QDialogButtonBox::AcceptRole);
    confirmButton->setObjectName(confirmButtonName());
    QPushButton *cancelButton = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
    const auto updateConfirmText = [this, confirmButton] {
        bool hasReplacement = false;
        for (int i = 0; i < m_rows->topLevelItemCount(); ++i) {
            auto *edit = qobject_cast<QLineEdit *>(m_rows->itemWidget(m_rows->topLevelItem(i), 5));
            if (edit && !edit->text().trimmed().isEmpty()) { hasReplacement = true; break; }
        }
        confirmButton->setText(hasReplacement || !m_plan.ok ? tr("检查替代") : tr("确认安装"));
    };
    for (int i = 0; i < m_rows->topLevelItemCount(); ++i) {
        if (auto *edit = qobject_cast<QLineEdit *>(m_rows->itemWidget(m_rows->topLevelItem(i), 5)))
            connect(edit, &QLineEdit::textChanged, this, updateConfirmText);
    }
    updateConfirmText();
    Theme::markButton(confirmButton, Theme::ButtonKind::Primary);
    Theme::markButton(cancelButton, Theme::ButtonKind::Neutral);
    connect(buttons, &QDialogButtonBox::accepted, this, &PresetModReview::confirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void PresetModReview::confirm() {
    Presets::PresetModInstaller::ReviewDecision decision;
    for (int i = 0; i < m_rows->topLevelItemCount(); ++i) {
        auto *edit = qobject_cast<QLineEdit *>(m_rows->itemWidget(m_rows->topLevelItem(i), 5));
        if (!edit || edit->text().trimmed().isEmpty()) continue;
        const QString text = edit->text().trimmed();
        Presets::ModReference reference;
        reference.platform = Presets::normalizeModPlatform(text.section(QLatin1Char(':'), 0, 0));
        reference.projectId = text.section(QLatin1Char(':'), 1);
        if (reference.key().isEmpty() || reference.projectId.contains(QLatin1Char(':')) ||
            reference.key() == edit->property("originalKey").toString()) {
            m_problem->setText(tr("替代项目格式无效，或与原项目相同。")); return;
        }
        decision.alternatives.insert(edit->property("originalKey").toString(), reference);
    }
    if (!m_plan.ok && decision.alternatives.isEmpty()) {
        m_problem->setText(tr("必要项尚未解决，请提供替代项目，或取消后修正预设 / 重试查询。")); return;
    }
    decision.accepted = true; m_decision = decision; accept();
}
