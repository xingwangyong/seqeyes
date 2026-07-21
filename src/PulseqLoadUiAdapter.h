#ifndef PULSEQLOADUIADAPTER_H
#define PULSEQLOADUIADAPTER_H

#include "IPulseqLoadUi.h"

#include <QString>

class MainWindow;
class QWidget;

/// Production implementation of IPulseqLoadUi that wraps MainWindow.
///
/// All UI interactions during the load/open flow go through this adapter,
/// keeping PulseqLoader and PulseqOpenController decoupled from MainWindow.
/// Tests use FakePulseqLoadUi (implements the same interface) instead.
class PulseqLoadUiAdapter : public IPulseqLoadUi
{
public:
    explicit PulseqLoadUiAdapter(MainWindow* mainWindow);

    QWidget* dialogParent() const override;
    void setBusy(bool busy) override;
    void clearLoadedState() override;
    void clearWindowFilePath() override;
    void hideVersionLabel() override;
    void showProgress() override;
    void setProgressValue(int value) override;
    void hideProgress() override;
    void showWarning(const QString& title, const QString& message) override;
    void showCritical(const QString& title, const QString& message) override;
    void setLoadedTitle(const QString& path) override;
    void refreshTrajectoryPlot() override;

private:
    MainWindow* m_mainWindow {nullptr};
};

#endif
