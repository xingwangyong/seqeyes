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
    , m_currentLevel(Settings::LogLevel::Info) // Default to Info level
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
    if (m_currentLevel != level) {
        m_currentLevel = level;
        emit logLevelChanged(level);
    }
}

Settings::LogLevel LogManager::getLogLevel() const
{
    return m_currentLevel;
}

bool LogManager::shouldLog(Settings::LogLevel messageLevel) const
{
    return static_cast<int>(messageLevel) <= static_cast<int>(m_currentLevel);
}

void LogManager::appendEntryInternal(QtMsgType type,
                                     const QString& source,
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
    if (!source.isEmpty())
    {
        prefix += QStringLiteral("[%1] ").arg(source);
    }

    QString line = prefix + message;
    if (!file.isEmpty())
    {
        line += QStringLiteral(" (%1)").arg(file);
    }

    m_lines.append(line);
    LogEntry e;
    e.timestamp = ts;
    e.level = levelStr;
    e.source = source;
    e.message = message;
    e.file = file;
    m_entries.append(e);
    if (m_lines.size() > m_maxLines)
    {
        m_lines.removeFirst();
        if (!m_entries.isEmpty())
            m_entries.removeFirst();
    }

    emit logLineAppended(line);
    emit logEntryAppended(e.timestamp, e.level, e.source, e.message, e.file);
}

void LogManager::appendFromQt(QtMsgType type,
                              const QMessageLogContext& context,
                              const QString& msg)
{
    QString source;
    if (context.category && *context.category)
    {
        source = QString::fromUtf8(context.category);
        if (source.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0)
            source.clear();
    }

    QString file;
    if (context.file && *context.file && context.line > 0)
    {
        const QString filePath = QString::fromUtf8(context.file);
        const QString baseName = QFileInfo(filePath).fileName();
        file = QStringLiteral("%1:%2").arg(baseName).arg(context.line);
    }
    appendEntryInternal(type, source, msg, file);
}

void LogManager::appendStructured(QtMsgType type,
                                  const QString& source,
                                  const QString& message,
                                  const QString& file)
{
    appendEntryInternal(type, source.trimmed(), message, file.trimmed());
}

void LogManager::appendDiagnostic(const QString& source,
                                  const QString& message,
                                  const QString& file)
{
    appendEntryInternal(QtWarningMsg,
                        source.trimmed(),
                        message,
                        file.trimmed(),
                        true);
}

void LogManager::fatal(const QString& message)
{
    // qFatal aborts the application after printing
    if (shouldLog(Settings::LogLevel::Fatal)) {
        qFatal("%s", qPrintable(message));
    } else {
        // Even if filtered out, fatal should still abort by definition
        qFatal("%s", qPrintable(message));
    }
}

void LogManager::error(const QString& message)
{
    if (shouldLog(Settings::LogLevel::Critical)) {
        qCritical().noquote() << message;
    }
}

void LogManager::warning(const QString& message)
{
    if (shouldLog(Settings::LogLevel::Warning)) {
        qWarning().noquote() << message;
    }
}

void LogManager::info(const QString& message)
{
    if (shouldLog(Settings::LogLevel::Info)) {
        qInfo().noquote() << message;
    }
}

void LogManager::debug(const QString& message)
{
    if (shouldLog(Settings::LogLevel::Debug)) {
        qDebug().noquote() << message;
    }
}

void LogManager::fatal(const QString& category, const QString& message)
{
    Q_UNUSED(category);
    // qFatal has no streaming API; prepend category manually
    QString line = category.isEmpty() ? message : (category + ": " + message);
    qFatal("%s", qPrintable(line));
}

void LogManager::error(const QString& category, const QString& message)
{
    if (shouldLog(Settings::LogLevel::Critical)) {
        qCritical().noquote() << category << ":" << message;
    }
}

void LogManager::warning(const QString& category, const QString& message)
{
    if (shouldLog(Settings::LogLevel::Warning)) {
        qWarning().noquote() << category << ":" << message;
    }
}

void LogManager::info(const QString& category, const QString& message)
{
    if (shouldLog(Settings::LogLevel::Info)) {
        qInfo().noquote() << category << ":" << message;
    }
}

void LogManager::debug(const QString& category, const QString& message)
{
    if (shouldLog(Settings::LogLevel::Debug)) {
        qDebug().noquote() << category << ":" << message;
    }
}
