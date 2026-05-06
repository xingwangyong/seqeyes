#pragma once

#include <QDialog>
#include <QVector>

#include "LogManager.h"

class QTableView;

/**
 * @brief Log viewer dialog with table layout (Excel-like).
 *
 * Comment (English): Uses QTableView so users can resize columns interactively.
 */
class LogTableDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LogTableDialog(QWidget* parent = nullptr);

    void setInitialContent(const QVector<LogManager::LogEntry>& entries);
    void appendEntry(const LogManager::LogEntry& entry);

private:
    void copySelectedRowsToClipboard() const;
    void copySelectedMessagesToClipboard() const;
    void showContextMenu(const QPoint& pos) const;
    bool isNearBottom() const;
    void scrollToBottomIfNeeded(bool followBottom);

private:
    class LogTableModel* m_model {nullptr};
    QTableView* m_view {nullptr};
};
