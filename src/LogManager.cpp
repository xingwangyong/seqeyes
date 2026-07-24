#include "LogManager.h"
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

namespace {
Settings::LogLevel qtMsgTypeToLogLevel(QtMsgType type)
{
    switch (type)
    {
    case QtDebugMsg:    return Settings::LogLevel::Debug;
    case QtInfoMsg:     return Settings::LogLevel::Info;
    case QtWarningMsg:  return Settings::LogLevel::Warning;
    case QtCriticalMsg: return Settings::LogLevel::Critical;
    case QtFatalMsg:    return Settings::LogLevel::Fatal;
    default:            return Settings::LogLevel::Info;
    }
}
}

LogManager& LogManager::getInstance()
{
    static LogManager instance;
    return instance;
}

LogManager::LogManager(QObject* parent)
    : QObject(parent)
    , m_currentLevel(Settings::LogLevel::Info)
{
    // Connect to settings changes
    Settings& settings = Settings::getInstance();
    connect(&settings, &Settings::settingsChanged, this, [this, &settings]() {
        setLogLevel(settings.getLogLevel());
    });
    
    // Initialize with current settings
    setLogLevel(settings.getLogLevel());
}

void LogManager::setLogLevel(Settings::LogLevel level)
{
    if (m_currentLevel.load(std::memory_order_relaxed) != level)
    {
        m_currentLevel.store(level, std::memory_order_relaxed);
        emit logLevelChanged(level);
    }
}

Settings::LogLevel LogManager::getLogLevel() const
{
    return m_currentLevel.load(std::memory_order_relaxed);
}

QStringList LogManager::getBufferedLines() const
{
    QMutexLocker locker(&m_mutex);
    return m_lines;
}

QVector<LogManager::LogEntry> LogManager::getBufferedEntries() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

bool LogManager::shouldLog(Settings::LogLevel messageLevel) const
{
    return static_cast<int>(messageLevel) <= static_cast<int>(m_currentLevel.load(std::memory_order_relaxed));
}

void LogManager::appendEntryInternal(QtMsgType type,
                                     const QString& category,
                                     const QString& message,
                                     const QString& file,
                                     bool bypassFilter)
{
    if (!bypassFilter && !shouldLog(qtMsgTypeToLogLevel(type))) {
        return;
    }

    const QString ts = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));

    QString levelStr;
    switch (type)
    {
    case QtDebugMsg:    levelStr = QStringLiteral("DEBUG");   break;
    case QtInfoMsg:     levelStr = QStringLiteral("INFO");    break;
    case QtWarningMsg:  levelStr = QStringLiteral("WARN");    break;
    case QtCriticalMsg: levelStr = QStringLiteral("ERROR");   break;
    case QtFatalMsg:    levelStr = QStringLiteral("FATAL");   break;
    default:            levelStr = QStringLiteral("LOG");     break;
    }

    QString prefix = QStringLiteral("%1 [%2] ").arg(ts, levelStr);
    if (!category.isEmpty())
    {
        prefix += QStringLiteral("[%1] ").arg(category);
    }

    QString line = prefix + message;
    if (!file.isEmpty())
    {
        line += QStringLiteral(" (%1)").arg(file);
    }

    LogEntry e;
    e.timestamp = ts;
    e.level = levelStr;
    e.category = category;
    e.message = message;
    e.file = file;

    {
        QMutexLocker locker(&m_mutex);
        m_entries.append(e);
        m_lines.append(line);

        if (m_lines.size() > m_maxLines)
        {
            m_lines.removeFirst();
            if (!m_entries.isEmpty())
                m_entries.removeFirst();
        }
    } // unlock before emitting signals

    emit logLineAppended(line);
    emit logEntryAppended(e.timestamp, e.level, e.category, e.message, e.file);
}

void LogManager::appendFromQt(QtMsgType type,
                              const QMessageLogContext& context,
                              const QString& msg)
{
    QString category;
    if (context.category && *context.category)
    {
        category = QString::fromUtf8(context.category);
        if (category.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
            category.clear();
    }

    QString file;
    if (context.file && *context.file && context.line > 0)
    {
        const QString filePath = QString::fromUtf8(context.file);
        const QString baseName = QFileInfo(filePath).fileName();
        file = QStringLiteral("%1:%2").arg(baseName).arg(context.line);
    }
    appendEntryInternal(type, category, msg, file);
}

void LogManager::appendStructured(QtMsgType type,
                                  const QString& category,
                                  const QString& message,
                                  const QString& file)
{
    appendEntryInternal(type, category.trimmed(), message, file.trimmed());
}

void LogManager::appendDiagnostic(const QString& category,
                                  const QString& message,
                                  const QString& file)
{
    appendEntryInternal(QtWarningMsg,
                        category.trimmed(),
                        message,
                        file.trimmed(),
                        true);
}
