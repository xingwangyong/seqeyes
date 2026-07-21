#ifndef IPULSEQLOADUI_H
#define IPULSEQLOADUI_H

#include <QString>

class QWidget;

/// Abstract interface for the UI adapter layer used by PulseqLoader and
/// PulseqOpenController during file-open / load operations.
///
/// Production code implements this via PulseqLoadUiAdapter (wraps MainWindow).
/// Tests inject a FakePulseqLoadUi to record calls without a real window.
class IPulseqLoadUi
{
public:
    virtual ~IPulseqLoadUi() = default;

    /// Parent widget for modal dialogs (QMessageBox, etc.).
    virtual QWidget* dialogParent() const = 0;

    /// Enable/disable the main window during long operations.
    virtual void setBusy(bool busy) = 0;

    /// Clear all visually loaded state (title, version label, progress, plots).
    virtual void clearLoadedState() = 0;

    /// Clear the native window file-path association.
    virtual void clearWindowFilePath() = 0;

    /// Hide the Pulseq version label in the status bar.
    virtual void hideVersionLabel() = 0;

    /// Show the progress bar.
    virtual void showProgress() = 0;

    /// Set the progress bar value (0-100).
    virtual void setProgressValue(int value) = 0;

    /// Hide the progress bar.
    virtual void hideProgress() = 0;

    /// Show a warning dialog.
    virtual void showWarning(const QString& title, const QString& message) = 0;

    /// Show a critical error dialog.
    virtual void showCritical(const QString& title, const QString& message) = 0;

    /// Set the window title to reflect the successfully loaded file path.
    virtual void setLoadedTitle(const QString& path) = 0;

    /// Refresh the trajectory plot if visible.
    virtual void refreshTrajectoryPlot() = 0;
};

#endif // IPULSEQLOADUI_H
