#ifndef OPENRESULT_H
#define OPENRESULT_H

#include "LoadResult.h"

#include <QFileInfo>
#include <QString>

/// Structured result returned by PulseqOpenController::openPath() and reopen().
///
/// Replaces the previous bare `bool` return value, giving callers access to the
/// loaded path and structured error information in a single object.
struct OpenResult
{
    enum class Status
    {
        NoOp,
        Success,
        Warning,
        Error
    };

    bool ok = false;           ///< true if the file was loaded successfully
    QString loadedPath;        ///< Absolute path of the loaded file (empty on failure)
    QString errorTitle;        ///< Short error title (empty on success)
    QString errorMessage;      ///< Detailed error message (empty on success)
    Status status = Status::NoOp;

    static OpenResult success(const QString& loadedPath)
    {
        return {true, QFileInfo(loadedPath).absoluteFilePath(), QString(), QString(), Status::Success};
    }

    static OpenResult warning(const QString& title, const QString& message)
    {
        return {false, QString(), title, message, Status::Warning};
    }

    static OpenResult error(const QString& title, const QString& message)
    {
        return {false, QString(), title, message, Status::Error};
    }

    static OpenResult noOp()
    {
        return {};
    }

    static OpenResult fromLoadResult(const LoadResult& result, const QString& candidatePath)
    {
        if (result.ok)
            return success(candidatePath);
        if (!result.error.title.isEmpty() || !result.error.message.isEmpty())
            return error(result.error.title, result.error.message);
        return error(QStringLiteral("Load Error"),
                     QStringLiteral("Failed to load file: %1").arg(candidatePath));
    }

    /// Convenience: implicit bool conversion for backward compatibility.
    explicit operator bool() const { return ok; }
    bool isNoOp() const { return status == Status::NoOp; }
};

#endif // OPENRESULT_H
