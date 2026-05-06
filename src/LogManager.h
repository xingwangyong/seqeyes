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
        QString source;
        QString message;
        QString file; // e.g. "WaveformDrawer.cpp:464"
    };
    
    // Log level methods
    void setLogLevel(Settings::LogLevel level);
    Settings::LogLevel getLogLevel() const;
    
    // Logging methods
    void fatal(const QString& message);
    void error(const QString& message);
    void warning(const QString& message);
    void info(const QString& message);
    void debug(const QString& message);
    
    // Convenience methods for formatted output
    void fatal(const QString& category, const QString& message);
    void error(const QString& category, const QString& message);
    void warning(const QString& category, const QString& message);
    void info(const QString& category, const QString& message);
    void debug(const QString& category, const QString& message);

    // Central sink for Qt's global message handler.
    // Called from qtLogFilter (see main.cpp) to record all messages,
    // independent of whether a console window exists.
    void appendFromQt(QtMsgType type,
                      const QMessageLogContext& context,
                      const QString& msg);
    void appendStructured(QtMsgType type,
                          const QString& source,
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
                          const QString& source,
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
                             const QString& source,
                             const QString& message,
                             const QString& file);

    // In‑memory rolling buffer of formatted log lines (for Log window)
    QStringList m_lines;
    QVector<LogEntry> m_entries;
    int m_maxLines = 5000;
};

// Convenience macros for easier logging
#define LOG_FATAL(msg) LogManager::getInstance().fatal(msg)
#define LOG_ERROR(msg) LogManager::getInstance().error(msg)
#define LOG_WARNING(msg) LogManager::getInstance().warning(msg)
#define LOG_INFO(msg) LogManager::getInstance().info(msg)
#define LOG_DEBUG(msg) LogManager::getInstance().debug(msg)

#define LOG_FATAL_CAT(category, msg) LogManager::getInstance().fatal(category, msg)
#define LOG_ERROR_CAT(category, msg) LogManager::getInstance().error(category, msg)
#define LOG_WARNING_CAT(category, msg) LogManager::getInstance().warning(category, msg)
#define LOG_INFO_CAT(category, msg) LogManager::getInstance().info(category, msg)
#define LOG_DEBUG_CAT(category, msg) LogManager::getInstance().debug(category, msg)

#endif // LOGMANAGER_H
