#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QObject>
#include <QDebug>
#include <QtGlobal>
#include <QStringList>
#include <QVector>
#include "Settings.h"

class LogManager : public QObject
{
    Q_OBJECT

public:
    static LogManager& getInstance();

    struct LogEntry
    {
        QString timestamp;
        QString level;
        QString category;
        QString message;
        QString file; // e.g. "WaveformDrawer.cpp:464"
    };
    
    // Log level methods
    void setLogLevel(Settings::LogLevel level);
    Settings::LogLevel getLogLevel() const;
    
    // Central sink for Qt's global message handler.
    // Called from qtLogFilter (see main.cpp) to record all messages,
    // independent of whether a console window exists.
    void appendFromQt(QtMsgType type,
                      const QMessageLogContext& context,
                      const QString& msg);
    void appendStructured(QtMsgType type,
                          const QString& category,
                          const QString& message,
                          const QString& file = QString());
    void appendDiagnostic(const QString& category,
                          const QString& message,
                          const QString& file = QString());

    // Return the in‑memory log buffer (oldest first).
    QStringList getBufferedLines() const { return m_lines; }
    QVector<LogEntry> getBufferedEntries() const { return m_entries; }

signals:
    void logLevelChanged(Settings::LogLevel level);
    // Emitted whenever a new formatted line is appended to the buffer.
    void logLineAppended(const QString& line);
    void logEntryAppended(const QString& timestamp,
                          const QString& level,
                          const QString& category,
                          const QString& message,
                          const QString& file);

private:
    explicit LogManager(QObject* parent = nullptr);
    ~LogManager() = default;
    
    // Disable copy constructor and assignment operator
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;
    
    Settings::LogLevel m_currentLevel;
    
    // Helper method to check if message should be logged
    bool shouldLog(Settings::LogLevel messageLevel) const;
    void appendEntryInternal(QtMsgType type,
                             const QString& category,
                             const QString& message,
                             const QString& file,
                             bool bypassFilter = false);

    // In‑memory rolling buffer of formatted log lines (for Log window)
    QStringList m_lines;
    QVector<LogEntry> m_entries;
    int m_maxLines = 5000;
};

// Convenience macros for easier logging
#define LOG_FATAL(msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO).fatal("%s", qPrintable(QString(msg)))
#define LOG_ERROR(msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO).critical("%s", qPrintable(QString(msg)))
#define LOG_WARNING(msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO).warning("%s", qPrintable(QString(msg)))
#define LOG_INFO(msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO).info("%s", qPrintable(QString(msg)))
#define LOG_DEBUG(msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO).debug("%s", qPrintable(QString(msg)))

#define LOG_FATAL_CAT(cat, msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, qPrintable(QString(cat))).fatal("%s", qPrintable(QString(msg)))
#define LOG_ERROR_CAT(cat, msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, qPrintable(QString(cat))).critical("%s", qPrintable(QString(msg)))
#define LOG_WARNING_CAT(cat, msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, qPrintable(QString(cat))).warning("%s", qPrintable(QString(msg)))
#define LOG_INFO_CAT(cat, msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, qPrintable(QString(cat))).info("%s", qPrintable(QString(msg)))
#define LOG_DEBUG_CAT(cat, msg) QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, qPrintable(QString(cat))).debug("%s", qPrintable(QString(msg)))

#endif // LOGMANAGER_H
