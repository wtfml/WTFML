#pragma once
#include <QDialog>
#include <QList>
class QTableWidget;
class QTableWidgetItem;
class PresetOptionsEditor : public QDialog {
public:
    struct Entry { QString key, value; };
    enum class Mode { All, Options, Keys };
    explicit PresetOptionsEditor(const QList<Entry> &entries, QWidget *parent = nullptr, Mode mode = Mode::All);
    QList<Entry> entries() const;
private:
    QTableWidgetItem *append(const Entry &entry, int row, int column, bool included = true);
    const Mode m_mode;
    QTableWidget *m_table;
    QList<QTableWidgetItem *> m_options;
    QList<QTableWidgetItem *> m_groups;
};
