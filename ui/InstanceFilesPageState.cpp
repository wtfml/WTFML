/**
 * @file InstanceFilesPageState.cpp
 * @brief 这一页摆成哪一档：不可用 / 空 / 筛完一条不剩 / 有东西。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一页有四个"现在是哪一种"档位，而同一件事（比如"搜索那一栏露不露"）在四个档位下答案
 * 不同。`applyState()` 是这些显隐的唯一一处，改动它要一次看全四档：
 *
 * - 搜索 / 状态那两栏：这一页用不上、或者目录本来就空 ⇒ 整块收起来；**筛完一条不剩时
 *   留着** —— 用户正要靠它们把条件改回来；
 * - 下拉框与四颗按钮互相显隐（开了批量的页摆四颗，没开的摆下拉框）；
 * - 空/不可用那一段话：说清"是筛选把它筛没的"还是"目录本来就空"；「可更新」那一档还要
 *   分清"还没查过"与"查过了没有"，那两件事的下一步完全不同；
 * - 底下那条悬浮操作条跟着列表的可见性走 —— 判据要的是**上面刚摆好的** `m_list`，
 *   所以必须排在它后面。
 *
 * 【判据不在这一层】
 *
 * 哪一档摆得出来（`stateFilterVisible()`）在筛选那一份；哪些条算中（`entryMatchesFilter()`）
 * 也在那边；这一层只把已经算好的事实摆成界面。
 */

#include "InstanceFilesPage.hpp"

#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QWidget>

void InstanceFilesPage::applyState() {
    const bool unavailable = !m_unavailable.isEmpty();
    const bool empty = !unavailable && m_empty;
    // 第四档：目录里有东西、但这一趟筛完一条不剩。
    const bool filtered = !unavailable && !empty && m_filteredEmpty;

    // 搜索框 / 状态那一栏：**这一页用不上、或者目录本来就空**的时候整块收起来（没有
    // 可筛的东西，露着两排空控件等于摆两个坏键）。筛完一条不剩时**留着** —— 用户正要
    // 靠它们把条件改回来，这时候把它们收掉是最糟的一刻。
    const bool filtersUseful = !unavailable && !m_empty;
    if (m_searchRow != nullptr) {
        m_searchRow->setVisible(filtersUseful);
    }
    if (m_stateRow != nullptr) {
        const bool choice = stateFilterVisible(StateFilter::Enabled) ||
            stateFilterVisible(StateFilter::Disabled) || stateFilterVisible(StateFilter::Update);
        // 紧凑页头开了批量时，只要哪颗胶囊**可能**露出来就一直留着这一排（位置见
        // placeCompactStateRow）：按「此刻有没有可选的」显隐的话，查完更新「可更新」一冒出来列表就下跳。
        const bool reserved = m_batchMode && (m_canToggle || m_canFilterUpdates);
        placeCompactStateRow();
        m_stateRow->setVisible(filtersUseful && !m_worldLayout && (!m_compactHeader || choice || reserved));
    }
    // 那一排里**下拉框和四颗按钮互相显隐**（见 .hpp 里 m_stateRow 那段）：
    // 开了批量的页摆那四颗（连「显示」那个标签一起收掉 —— 那四颗自己就写着「全部(352)」），
    // 没开的页一丝不变。
    if (m_stateLabel != nullptr) {
        m_stateLabel->setVisible(!m_batchMode);
    }
    if (m_stateCombo != nullptr) {
        m_stateCombo->setVisible(!m_batchMode);
    }
    if (m_stateButtonsRow != nullptr) {
        m_stateButtonsRow->setVisible(m_batchMode);
    }

    m_pathRow->setVisible(!unavailable && !m_actionPanelEnabled);
    // 模组页的常用操作（下载、打开文件夹）在目录还空着时也要能用。
    if (m_actionPanelEnabled) m_selectionTools->setVisible(!unavailable);
    if (m_structureRows != nullptr) {
        m_structureRows->setVisible(!unavailable && !m_structureSources.isEmpty());
    }
    m_list->setVisible(!unavailable && !empty && !filtered);
    m_notice->setVisible(unavailable || empty || filtered);
    if (m_info != nullptr && (unavailable || empty || filtered)) {
        if (m_infoTimer != nullptr) {
            m_infoTimer->stop();
        }
        m_infoPath.clear();
        m_info->clear();
        m_info->setToolTip(QString());
        m_info->setVisible(false);
    }
    // 底下那条悬浮操作条跟着这一页的可见性走（它压在列表上，"列表都收起来了还浮着
    // 一条操作条"是最难解释的一种画面）。判据要的是**上面这一句刚摆好的** m_list 的
    // 可见性 ⇒ 必须排在这一句之后。
    updateSelectionBar();
    if (unavailable) {
        m_notice->setText(m_unavailable);
        return;
    }
    if (empty) {
        m_notice->setText(m_emptyText.isEmpty() ? emptyText() : m_emptyText);
        return;
    }
    // 筛完一条不剩：说清**是筛选把它筛没的**（不是目录空了），而且选着「可更新」时还要
    // 分清"还没查过"和"查过了没有"—— 那两件事的下一步完全不同。
    if (m_stateFilter == StateFilter::Update) {
        // NOTE: 开了批量那四页**够不着这儿**：（ 一整天里这一档被来回摆过两次，
        // 最后落在下面这条上）「可更新」那一颗在**没查过**的时候整颗收着
        // （见 stateFilterVisible() 里那段），而且 setStateFilter() 撞上一颗摆不出来
        // 的档会把条件退回「全部」⇒ 这一档在那一页上要么没被选中、要么已经查过了。
        // 所以下面这句在那儿仍是"够不着的兜底"，真正走得到它的是**摆下拉框的那几页**。
        //
        // NOTE: 查完之后走不到这儿 —— 「可更新(N)」在 N == 0 时**整颗收着**，而且它一旦
        // 收起来，选中的条件会被 ensureStateFilterVisible() 退回到「全部」。
        m_notice->setText(m_updateChecked ? updateNoneText() : updateNotCheckedText());
        return;
    }
    m_notice->setText(noMatchText());
}
