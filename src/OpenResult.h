#ifndef OPENRESULT_H
#define OPENRESULT_H

#include <QString>

/// Structured result returned by PulseqOpenController::openPath() and reopen().
///
/// Replaces the previous bare `bool` return value, giving callers access to the
/// loaded path and structured error information in a single object.
struct OpenResult
{
    bool ok = false;           ///< true if the file was loaded successfully
    QString loadedPath;        ///< Absolute path of the loaded file (empty on failure)
    QString errorTitle;        ///< Short error title (empty on success)
    QString errorMessage;      ///< Detailed error message (empty on success)

    /// Convenience: implicit bool conversion for backward compatibility.
    explicit operator bool() const { return ok; }
};

#endif // OPENRESULT_H
