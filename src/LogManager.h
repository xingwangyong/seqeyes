#ifndef LOGMANAGER_H
#define LOGMANAGER_H

#include <QObject>
#include <QDebug>
#include <QStringList>
#include <QVector>
#include <QMutex>
#include <atomic>
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
    bool isLevelEnabled(Settings::LogLevel level) const;

    struct PerfLoadSummary {
        QString traceId;
        QString reason;
        qint64 totalMs = 0;
        qint64 parseMs = 0;
        qint64 decodeMs = 0;
        qint64 renderDataMs = 0;
        qint64 replotMs = 0;
        int visibleBlocks = 0;
        int pointsTotal = 0;
        int rfPoints = 0;
        int adcRectPoints = 0;
        int adcPhasePoints = 0;
        int gradPoints = 0;
        int trigPoints = 0;
        int edgePoints = 0;
        QString slowestStage;

        QString toLogString() const {
            return QStringLiteral("type=load traceId=%1 reason=%2 totalMs=%3 parseMs=%4 decodeMs=%5 renderDataMs=%6 replotMs=%7 visibleBlocks=%8 pointsTotal=%9 rfPoints=%10 adcRectPoints=%11 adcPhasePoints=%12 gradPoints=%13 trigPoints=%14 edgePoints=%15 slowestStage=%16")
                .arg(traceId, reason).arg(totalMs).arg(parseMs).arg(decodeMs).arg(renderDataMs).arg(replotMs).arg(visibleBlocks).arg(pointsTotal)
                .arg(rfPoints).arg(adcRectPoints).arg(adcPhasePoints).arg(gradPoints).arg(trigPoints).arg(edgePoints).arg(slowestStage);
        }
    };

    struct PerfInteractionSummary {
        QString traceId;
        QString reason;
        int frames = 0;
        qint64 totalMs = 0;
        qint64 maxFrameMs = 0;
        qint64 avgRenderMs = 0;
        qint64 thresholdMs = 33;
        int overThresholdFrames = 0;
        int visibleBlocksAvg = 0;
        int pointsTotalAvg = 0;
        QString slowestStage;

        QString toLogString() const {
            return QStringLiteral("type=zoom traceId=%1 reason=%2 frames=%3 totalMs=%4 maxFrameMs=%5 avgRenderMs=%6 thresholdMs=%7 overThresholdFrames=%8 visibleBlocksAvg=%9 pointsTotalAvg=%10 slowestStage=%11")
                .arg(traceId, reason).arg(frames).arg(totalMs).arg(maxFrameMs).arg(avgRenderMs).arg(thresholdMs).arg(overThresholdFrames).arg(visibleBlocksAvg).arg(pointsTotalAvg).arg(slowestStage);
        }
    };

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
    QStringList getBufferedLines() const;
    QVector<LogEntry> getBufferedEntries() const;

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
    
    std::atomic<Settings::LogLevel> m_currentLevel;
    
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
    
    mutable QMutex m_mutex;
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
