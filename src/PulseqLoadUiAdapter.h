#ifndef PULSEQLOADUIADAPTER_H
#define PULSEQLOADUIADAPTER_H

#include <QString>

class MainWindow;
class QWidget;

class PulseqLoadUiAdapter
{
public:
    explicit PulseqLoadUiAdapter(MainWindow* mainWindow);

    QWidget* dialogParent() const;
    void setBusy(bool busy);
    void clearLoadedState();
    void clearWindowFilePath();
    void hideVersionLabel();
    void showProgress();
    void setProgressValue(int value);
    void hideProgress();
    void showWarning(const QString& title, const QString& message);
    void showCritical(const QString& title, const QString& message);
    void setLoadedTitle(const QString& path);
    void refreshTrajectoryPlot();

private:
    MainWindow* m_mainWindow {nullptr};
};

#endif
