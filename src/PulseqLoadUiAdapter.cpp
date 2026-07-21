#include "PulseqLoadUiAdapter.h"

#include "InteractionHandler.h"
#include "WaveformDrawer.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>

PulseqLoadUiAdapter::PulseqLoadUiAdapter(MainWindow* mainWindow)
    : m_mainWindow(mainWindow)
{
}

QWidget* PulseqLoadUiAdapter::dialogParent() const
{
    return m_mainWindow;
}

void PulseqLoadUiAdapter::setBusy(bool busy)
{
    if (m_mainWindow)
        m_mainWindow->setEnabled(!busy);
}

void PulseqLoadUiAdapter::clearLoadedState()
{
    if (!m_mainWindow)
        return;

    if (auto* ih = m_mainWindow->getInteractionHandler())
        ih->cancelPendingViewportRenders();
    m_mainWindow->clearLoadedFileTitle();
    if (auto* lbl = m_mainWindow->getVersionLabel()) {
        lbl->setText(QString());
        lbl->setVisible(false);
    }
    hideProgress();
    if (auto* drawer = m_mainWindow->getWaveformDrawer())
        drawer->clearAllWaveformData();
    if (m_mainWindow->ui && m_mainWindow->ui->customPlot)
        m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
}

void PulseqLoadUiAdapter::clearWindowFilePath()
{
    if (m_mainWindow)
        m_mainWindow->setWindowFilePath(QString());
}

void PulseqLoadUiAdapter::hideVersionLabel()
{
    if (m_mainWindow && m_mainWindow->getVersionLabel())
        m_mainWindow->getVersionLabel()->setVisible(false);
}

void PulseqLoadUiAdapter::showProgress()
{
    if (m_mainWindow && m_mainWindow->getProgressBar())
        m_mainWindow->getProgressBar()->show();
}

void PulseqLoadUiAdapter::setProgressValue(int value)
{
    if (m_mainWindow && m_mainWindow->getProgressBar())
        m_mainWindow->getProgressBar()->setValue(value);
}

void PulseqLoadUiAdapter::hideProgress()
{
    if (m_mainWindow && m_mainWindow->getProgressBar())
        m_mainWindow->getProgressBar()->hide();
}

void PulseqLoadUiAdapter::showWarning(const QString& title, const QString& message)
{
    QMessageBox::warning(m_mainWindow, title, message);
}

void PulseqLoadUiAdapter::showCritical(const QString& title, const QString& message)
{
    QMessageBox::critical(m_mainWindow, title, message);
}

void PulseqLoadUiAdapter::setLoadedTitle(const QString& path)
{
    if (m_mainWindow)
        m_mainWindow->setLoadedFileTitle(path);
}

void PulseqLoadUiAdapter::refreshTrajectoryPlot()
{
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
        m_mainWindow->refreshTrajectoryPlotData();
}
