#include "LogTableDialog.h"

#include <QApplication>
#include <QAbstractTableModel>
#include <QClipboard>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QMenu>
#include <QScrollBar>
#include <QShortcut>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

class LogTableModel final : public QAbstractTableModel
{
public:
    enum Col
    {
        Time = 0,
        Level,
        Source,
        Message,
        File,
        ColCount
    };

    explicit LogTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        if (parent.isValid()) return 0;
        return m_entries.size();
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override
    {
        if (parent.isValid()) return 0;
        return ColCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Horizontal)
        {
            switch (section)
            {
                case Time:     return QStringLiteral("Time");
                case Level:    return QStringLiteral("Level");
                case Source:   return QStringLiteral("Source");
                case Message:  return QStringLiteral("Message");
                case File:     return QStringLiteral("File");
                default:       return {};
            }
        }
        return section + 1;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid()) return {};
        const int r = index.row();
        const int c = index.column();
        if (r < 0 || r >= m_entries.size()) return {};
        if (c < 0 || c >= ColCount) return {};

        const auto& e = m_entries[r];
        if (role == Qt::DisplayRole)
        {
            switch (c)
            {
                case Time:     return e.timestamp;
                case Level:    return e.level;
                case Source:   return e.source;
                case Message:  return e.message;
                case File:     return e.file;
                default:       return {};
            }
        }
        return {};
    }

    void setEntries(const QVector<LogManager::LogEntry>& entries)
    {
        beginResetModel();
        m_entries = entries;
        endResetModel();
    }

    void appendEntry(const LogManager::LogEntry& entry)
    {
        const int r = m_entries.size();
        beginInsertRows(QModelIndex(), r, r);
        m_entries.append(entry);
        endInsertRows();
    }

private:
    QVector<LogManager::LogEntry> m_entries;
};

LogTableDialog::LogTableDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Log");
    setWindowModality(Qt::NonModal);
    resize(1100, 520);
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

    m_model = new LogTableModel(this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);

    // Scrollbar styling (white track + gray handle) for better visibility.
    m_view->setStyleSheet(QStringLiteral(R"(
QTableView {
  background: white;
}
QScrollBar:vertical, QScrollBar:horizontal {
  background: #ffffff;
  border: 1px solid #d0d0d0;
}
QScrollBar:vertical {
  width: 14px;
  margin: 0px;
}
QScrollBar:horizontal {
  height: 14px;
  margin: 0px;
}
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
  background: #a9a9a9;
  border: 1px solid #8f8f8f;
  border-radius: 6px;
  min-height: 24px;
  min-width: 24px;
}
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
  background: #8f8f8f;
}
QScrollBar::add-line, QScrollBar::sub-line {
  background: transparent;
  border: none;
  width: 0px;
  height: 0px;
}
QScrollBar::add-page, QScrollBar::sub-page {
  background: #ffffff;
}
)"));

    // Monospace font for logs.
    {
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        f.setStyleHint(QFont::Monospace);
        m_view->setFont(f);
    }

    m_view->setWordWrap(false);
    m_view->setTextElideMode(Qt::ElideNone);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setAlternatingRowColors(true);
    m_view->setSortingEnabled(false);
    m_view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);

    // User-resizable columns (Excel-like).
    QHeaderView* hdr = m_view->horizontalHeader();
    hdr->setSectionsMovable(true);
    hdr->setStretchLastSection(true);
    hdr->setSectionResizeMode(QHeaderView::Interactive);
    hdr->setMinimumSectionSize(60);

    // Reasonable defaults.
    m_view->setColumnWidth(LogTableModel::Time, 180);
    m_view->setColumnWidth(LogTableModel::Level, 70);
    m_view->setColumnWidth(LogTableModel::Source, 140);
    m_view->setColumnWidth(LogTableModel::Message, 600);
    m_view->setColumnWidth(LogTableModel::File, 220);

    auto* copyShortcut = new QShortcut(QKeySequence::Copy, m_view);
    connect(copyShortcut, &QShortcut::activated, this, [this]() {
        copySelectedRowsToClipboard();
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showContextMenu(pos);
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(m_view);
}

void LogTableDialog::copySelectedRowsToClipboard() const
{
    if (!m_view || !m_view->model() || !m_view->selectionModel()) {
        return;
    }

    QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }

    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) {
        return a.row() < b.row();
    });

    QStringList lines;
    lines.reserve(rows.size());
    for (const QModelIndex& rowIndex : rows) {
        QStringList cols;
        cols.reserve(LogTableModel::ColCount);
        for (int c = 0; c < LogTableModel::ColCount; ++c) {
            const QModelIndex cell = m_view->model()->index(rowIndex.row(), c);
            cols.append(m_view->model()->data(cell, Qt::DisplayRole).toString());
        }
        lines.append(cols.join(QLatin1Char('\t')));
    }

    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(lines.join(QLatin1Char('\n')));
    }
}

void LogTableDialog::copySelectedMessagesToClipboard() const
{
    if (!m_view || !m_view->model() || !m_view->selectionModel()) {
        return;
    }

    QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }

    std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) {
        return a.row() < b.row();
    });

    QStringList lines;
    lines.reserve(rows.size());
    for (const QModelIndex& rowIndex : rows) {
        const QModelIndex cell = m_view->model()->index(rowIndex.row(), LogTableModel::Message);
        lines.append(m_view->model()->data(cell, Qt::DisplayRole).toString());
    }

    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(lines.join(QLatin1Char('\n')));
    }
}

void LogTableDialog::showContextMenu(const QPoint& pos) const
{
    if (!m_view) {
        return;
    }

    QMenu menu(m_view);
    QAction* copyRows = menu.addAction(QStringLiteral("Copy Rows"));
    QAction* copyMessages = menu.addAction(QStringLiteral("Copy Message"));
    if (!m_view->selectionModel() || m_view->selectionModel()->selectedRows().isEmpty()) {
        copyRows->setEnabled(false);
        copyMessages->setEnabled(false);
    }

    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == copyRows) {
        copySelectedRowsToClipboard();
    } else if (chosen == copyMessages) {
        copySelectedMessagesToClipboard();
    }
}

bool LogTableDialog::isNearBottom() const
{
    if (!m_view) return true;
    auto* v = m_view->verticalScrollBar();
    if (!v) return true;
    return v->value() >= v->maximum() - 2;
}

void LogTableDialog::scrollToBottomIfNeeded(bool followBottom)
{
    if (!followBottom || !m_view) return;
    if (auto* v = m_view->verticalScrollBar())
        v->setValue(v->maximum());
}

void LogTableDialog::setInitialContent(const QVector<LogManager::LogEntry>& entries)
{
    const bool followBottom = true;
    m_model->setEntries(entries);
    scrollToBottomIfNeeded(followBottom);
}

void LogTableDialog::appendEntry(const LogManager::LogEntry& entry)
{
    const bool followBottom = isNearBottom();
    m_model->appendEntry(entry);
    scrollToBottomIfNeeded(followBottom);
}
