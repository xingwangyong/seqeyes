#include "InteractionHandler.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "TRManager.h"
#include "WaveformDrawer.h"
#include "doublerangeslider.h"
#include "ZoomManager.h"
#include "Settings.h"

#include <QDebug>

// RF use is now provided by PulseqLoader (post-classification cache). No local reclassification.

/*
InteractionHandler notes:
 - All wheel and drag operations are converted to viewport updates on the synchronized x-axes.
 - Boundary rules:
   * Never allow negative time (left bound >= 0 in internal units).
   * In time-based mode, allow zooming/panning within the full sequence range (clamped to data end).
   * In TR-Segmented mode, panning/zooming is effectively clamped by TRManager through WaveformDrawer,
     preventing accidental rendering of multi-TR spans that could degrade performance.
 - After each viewport change, WaveformDrawer::ensureRenderedForCurrentViewport() is invoked
   to re-render only the visible content at the appropriate detail level.
*/
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <QSet>

InteractionHandler::InteractionHandler(MainWindow* mainWindow)
    : QObject(mainWindow),
      m_mainWindow(mainWindow),
      m_pBlockInfoDialog(nullptr),
      m_bIsSelecting(false),
      m_pSelectionRect(nullptr),
      m_dDragStartRange(0.0),
      m_enablePanBoundaries(true), // Default to enabling pan boundaries
      m_tooltipVisible(false)
{
    m_pSelectionRect = new QCPItemRect(m_mainWindow->ui->customPlot);
    m_pSelectionRect->setVisible(false);

    // Initialize variables moved from MainWindow
    m_bIsSelecting = false;
    m_dDragStartRange = 0.;
    m_pBlockInfoDialog = nullptr;
    
    // Initialize tooltip timer
    m_tooltipTimer = new QTimer(this);
    m_tooltipTimer->setSingleShot(true);
    m_tooltipTimer->setInterval(2000); // Auto-hide after 2 seconds
    connect(m_tooltipTimer, &QTimer::timeout, this, [this]() {
        QToolTip::hideText();
        m_tooltipVisible = false;
    });

    // Wheel coalescing: process bursty wheel events in small batches to
    // improve "stickiness" and prevent excessive zooming after the wheel stops.
    // Rationale: Some devices emit many small wheel events rapidly. Processing
    // each event with a full zoom step causes continued zoom after the user
    // stops. We accumulate deltas and apply at ~60-80 Hz instead.
    m_wheelTimer = new QTimer(this);
    m_wheelTimer->setSingleShot(true);
    m_wheelTimer->setInterval(14); // ~70 Hz
    connect(m_wheelTimer, &QTimer::timeout, this, &InteractionHandler::processAccumulatedWheel);
}

InteractionHandler::~InteractionHandler()
{
    // The QCustomPlot takes ownership of m_pSelectionRect, so no need to delete.
    // m_pBlockInfoDialog is parented to MainWindow, so it will be deleted automatically.
}

void InteractionHandler::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void InteractionHandler::dropEvent(QDropEvent* event)
{
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();
        QString sPulseqFilePath = urlList.at(0).toLocalFile();

        // Delegate to PulseqLoader
        if (!m_mainWindow->getPulseqLoader()->LoadPulseqFile(sPulseqFilePath))
        {
            std::stringstream sLog;
            sLog << "Load " << sPulseqFilePath.toStdString() << " failed!";
            QMessageBox::critical(m_mainWindow, "File Error", sLog.str().c_str());
            return;
        }
        m_mainWindow->getPulseqLoader()->setPulseqFilePathCache(sPulseqFilePath);
    }
}

void InteractionHandler::wheelEvent(QWheelEvent* event)
{
    /*
     * DISABLED ZOOM IMPLEMENTATION:
     * 
     * This method is DISABLED to prevent conflicts with onMouseWheel() method.
     * 
     * PROBLEM: Both wheelEvent() and onMouseWheel() were being called for the same
     * wheel event, causing conflicts where zoom operations would become pan operations.
     * 
     * SOLUTION: Only use onMouseWheel() method which is connected to QCustomPlot's
     * mouseWheel signal. This method is kept for compatibility but does nothing.
     * 
     * The main zoom logic is now exclusively in onMouseWheel() method.
     */
    
    // Do nothing - all wheel event handling is done in onMouseWheel() method
    // This prevents the conflict that was causing zoom to become pan
}

void InteractionHandler::onMouseMove(QMouseEvent* event)
{
	// Axis drag reorder feedback
	if (m_axisDragging)
	{
		updateAxisDrag(event->pos());
		m_mainWindow->getWaveformDrawer()->updateAxisDragVisual(event->pos().y());
		return;
	}

	// If a title was pressed, start drag once movement passes threshold
	if (m_pendingAxisIndex >= 0)
	{
		if ((event->pos() - m_pressPos).manhattanLength() >= m_dragStartThresholdPx)
		{
			beginAxisDrag(m_pendingAxisIndex, m_pressPos);
			m_mainWindow->getWaveformDrawer()->startAxisDragVisual(m_pendingAxisIndex, m_pressPos);
			updateAxisDrag(event->pos());
			m_mainWindow->getWaveformDrawer()->updateAxisDragVisual(event->pos().y());
			return;
		}
	}

	// Measurement mode: live update of second marker and shaded region
	if (m_measureMode)
	{
		double xCoord = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(event->pos().x());
		WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
		if (drawer)
		{
			if (m_measureHasFirst)
			{
				m_measureT2 = xCoord;
			}
			QCustomPlot* plot = m_mainWindow->ui->customPlot;
            int rows = drawer->getRects().size();
			if (m_measureLines.size() != rows*2)
			{
				// create visuals lazily
				m_measureLines.clear();
				m_measureShades.clear();
				for (int r = 0; r < rows; ++r)
				{
                    QCPAxisRect* rect = drawer->getRects()[r];
                    if (!rect || !rect->visible()) { m_measureLines.append(nullptr); m_measureLines.append(nullptr); m_measureShades.append(nullptr); continue; }
                    auto l1 = new QCPItemStraightLine(plot); l1->setClipAxisRect(rect); l1->setVisible(false);
                    auto l2 = new QCPItemStraightLine(plot); l2->setClipAxisRect(rect); l2->setVisible(false);
                    // Bind item positions to this rect's axes; otherwise they default to plot->xAxis/yAxis and
                    // can disappear when the default axisRect is not the one currently visible (e.g. small windows / dynamic layouts).
                    l1->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                    l1->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                    l2->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                    l2->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
					QPen pen(Qt::darkGreen); pen.setStyle(Qt::DashLine); pen.setWidthF(1.0);
					l1->setPen(pen); l2->setPen(pen);
					m_measureLines.append(l1); m_measureLines.append(l2);
                    auto shade = new QCPItemRect(plot); shade->setClipAxisRect(rect); shade->setVisible(false);
                    shade->topLeft->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                    shade->bottomRight->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
					QBrush b(QColor(0,128,0,40)); shade->setBrush(b); shade->setPen(Qt::NoPen);
					m_measureShades.append(shade);
				}
			}
			double a = m_measureHasFirst ? std::min(m_measureT1, m_measureT2) : xCoord;
			double b = m_measureHasFirst ? std::max(m_measureT1, m_measureT2) : xCoord;
			for (int r = 0; r < drawer->getRects().size(); ++r)
			{
				QCPAxisRect* rect = drawer->getRects()[r];
                if (!rect || !rect->visible()) continue;
                double y0 = rect->axis(QCPAxis::atLeft)->range().lower;
                double y1 = rect->axis(QCPAxis::atLeft)->range().upper;
                QCPItemStraightLine* l1 = m_measureLines[r*2+0];
                QCPItemStraightLine* l2 = m_measureLines[r*2+1];
                if (!l1 || !l2) continue;
				l1->point1->setType(QCPItemPosition::ptPlotCoords); l1->point2->setType(QCPItemPosition::ptPlotCoords);
				l2->point1->setType(QCPItemPosition::ptPlotCoords); l2->point2->setType(QCPItemPosition::ptPlotCoords);
				l1->point1->setCoords(m_measureHasFirst?m_measureT1:xCoord, y0); l1->point2->setCoords(m_measureHasFirst?m_measureT1:xCoord, y1);
				l2->point1->setCoords(m_measureHasFirst?m_measureT2:xCoord, y0); l2->point2->setCoords(m_measureHasFirst?m_measureT2:xCoord, y1);
				l1->setVisible(true); l2->setVisible(true);
                QCPItemRect* shade = m_measureShades[r];
                if (!shade) continue;
				shade->topLeft->setType(QCPItemPosition::ptPlotCoords);
				shade->bottomRight->setType(QCPItemPosition::ptPlotCoords);
				shade->topLeft->setCoords(a, y1);
				shade->bottomRight->setCoords(b, y0);
				shade->setVisible(b>a);
			}
            // Hide default red guide lines while measuring
            for (auto* vline : drawer->getVerticalLines())
            {
                if (vline) vline->setVisible(false);
            }

            // Status text is appended in the normal hover path for consistency.

            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
        }
        // Continue to normal hover path so that status text appends Δt consistently
    }

	// Hover tooltips for ADC / Label / Trigger
	int axisIdx = -1;
	m_overAxisHandle = isOverAxisLabelArea(event->pos(), axisIdx);

	double xCoord = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(event->pos().x());
	PulseqLoader* loader = m_mainWindow->getPulseqLoader();
	if (!loader || loader->getDecodedSeqBlocks().empty()) return;

	const auto& edges = loader->getBlockEdges();
	int start = loader->getBlockRangeStart();
	int end = loader->getBlockRangeEnd();
	int blockIdx = -1;
	for (int i = start; i <= end && i + 1 < edges.size(); ++i)
	{
		if (xCoord >= edges[i] && xCoord < edges[i+1]) { blockIdx = i; break; }
	}
	// Whole-sequence mode can cover more than the active block window; fall back to
	// a global search so the status bar stays accurate instead of reporting -1.
	if (blockIdx < 0 && edges.size() > 1)
	{
		auto upper = std::upper_bound(edges.begin(), edges.end(), xCoord);
		int candidate = static_cast<int>(upper - edges.begin()) - 1;
		if (candidate >= 0 && candidate < loader->getDecodedSeqBlocks().size())
		{
			blockIdx = candidate;
		}
	}

	// Use mouse x position directly for the guide line to avoid snapping to sparse keys.
	double guideX = xCoord;
	WaveformDrawer* drawerForSearch = m_mainWindow->getWaveformDrawer();

	// Update vertical guide lines and bottom status label
	if (drawerForSearch)
	{
        // Build fixed-width, monospaced segments for status bar
        auto fixed = [](const QString& s, int width){ return s.leftJustified(width, ' ', true); };
        const int W_VER = 16, W_TIME = 16, W_BLOCK = 12, W_GRAD = 36, W_KSPACE = 32, W_RF = 36; // tunable widths

        // Version (from loader)
        QString ver = loader->getPulseqVersionString();
        if (ver.isEmpty()) ver = "v?.?.?";
        QString segVer = fixed(QString("File %1").arg(ver), W_VER);

        Settings& s = Settings::getInstance();
        const QString toUnit = s.getGradientUnitString();
        const bool useMicroseconds = (s.getTimeUnit() == Settings::TimeUnit::Microseconds);

        // Determine trajectory display scale and unit label
        Settings::TrajectoryUnit trajUnit = s.getTrajectoryUnit();
        QString trajUnitLabel = s.getTrajectoryUnitString();
        double trajScale = 1.0;
        if (trajUnit == Settings::TrajectoryUnit::RadPerM)
        {
            trajScale = 2.0 * M_PI;
        }
        else if (trajUnit == Settings::TrajectoryUnit::InvFov)
        {
            double fovMeters = 0.0;
            bool haveFov = false;
            if (loader)
            {
                auto seq = loader->getSequence();
                if (seq)
                {
                    std::vector<double> def = seq->GetDefinition("FOV");
                    if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0)
                    {
                        fovMeters = def[0];
                        haveFov = true;
                    }
                }
            }
            if (haveFov)
            {
                trajScale = fovMeters;
            }
            else
            {
                // Fallback: warn on console and revert to 1/m
                qWarning() << "Trajectory unit set to 1/FOV, but sequence lacks valid FOV definition; falling back to 1/m.";
                s.setTrajectoryUnit(Settings::TrajectoryUnit::PerM);
                trajUnit = s.getTrajectoryUnit();
                trajUnitLabel = s.getTrajectoryUnitString();
                trajScale = 1.0;
            }
        }
        auto formatTime = [&](double value, int decimalsForMs) {
            int decimals = useMicroseconds ? 0 : decimalsForMs;
            return QString::number(value, 'f', decimals);
        };

        // Time (2 decimals for ms, integer for us)
        QString segTime = fixed(QString("t=%1%2").arg(formatTime(guideX, 2)).arg(loader->getTimeUnits()), W_TIME);

        // Block index
        QString segBlock = fixed(QString("Block %1").arg(blockIdx >= 0 ? blockIdx : -1), W_BLOCK);

        // Gradients Gx,Gy,Gz (2 decimals) in selected unit
        auto fmt2 = [](double v){ return QString::number(v, 'f', 2); };
        QString gx = "--.--", gy = "--.--", gz = "--.--";
        if (blockIdx >= 0)
        {
            double val = 0.0;
            if (loader->sampleGradAtTime(0, guideX, blockIdx, val)) { if (toUnit != "Hz/m") val = s.convertGradient(val, "Hz/m", toUnit); gx = fmt2(val); }
            if (loader->sampleGradAtTime(1, guideX, blockIdx, val)) { if (toUnit != "Hz/m") val = s.convertGradient(val, "Hz/m", toUnit); gy = fmt2(val); }
            if (loader->sampleGradAtTime(2, guideX, blockIdx, val)) { if (toUnit != "Hz/m") val = s.convertGradient(val, "Hz/m", toUnit); gz = fmt2(val); }
        }
        QString segGrad = fixed(QString("Gxyz=%1,%2,%3 %4").arg(gx, gy, gz, toUnit), W_GRAD);
        QString segKSpace;
        {
            double kxVal = 0.0, kyVal = 0.0, kzVal = 0.0;
            auto fmt1 = [](double v){ return QString::number(v, 'f', 1); };
            QString kxStr = "--.-", kyStr = "--.-", kzStr = "--.-";
            if (m_mainWindow && m_mainWindow->sampleTrajectoryAtInternalTime(guideX, kxVal, kyVal, kzVal))
            {
                kxVal *= trajScale;
                kyVal *= trajScale;
                kzVal *= trajScale;
                kxStr = fmt1(kxVal);
                kyStr = fmt1(kyVal);
                kzStr = fmt1(kzVal);
            }
            segKSpace = fixed(QString("kxyz=%1,%2,%3 %4").arg(kxStr, kyStr, kzStr, trajUnitLabel), W_KSPACE);
        }

        // RF amplitude/phase (1 decimal)
        auto fmt1 = [](double v){ return QString::number(v, 'f', 1); };
        QString segRF;
        {
            double amp=0.0, ph=0.0;
            char rfUseChar = (blockIdx >= 0 ? loader->getRfUseForBlock(blockIdx) : 'u');
            if (blockIdx >= 0 && loader->sampleRFAtTime(guideX, blockIdx, amp, ph))
                segRF = fixed(QString("RF a=%1 Hz, ph=%2 rad, use=%3")
                              .arg(fmt1(amp), fmt1(ph), QString(rfUseChar)), W_RF);
            else
                segRF = fixed(QString("RF a=%1 Hz, ph=%2 rad, use=%3")
                              .arg("--.-", "--.-", QString(rfUseChar)), W_RF);
        }
        QString coordText = segVer + " | " + segTime + " | " + segBlock + " | " + segGrad + " | " + segKSpace + " | " + segRF;
        for (int i = 0; i < drawerForSearch->getVerticalLines().count(); i++)
        {
            QCPItemStraightLine* vline = drawerForSearch->getVerticalLines()[i];
            if (!vline) continue;
            vline->point1->setCoords(guideX, drawerForSearch->getRects()[i]->axis(QCPAxis::atBottom)->range().lower);
            vline->point2->setCoords(guideX, drawerForSearch->getRects()[i]->axis(QCPAxis::atBottom)->range().upper);
            vline->setVisible(!m_measureMode); // hide red guide while measuring
        }
        if (m_mainWindow)
        {
            m_mainWindow->updateTrajectoryCursorTime(guideX);
        }
        // Channel-specific inline appends removed; now summarized above
        // If measuring and at least one point exists, append Δt info
        if (m_measureMode)
        {
            if (m_measureHasFirst)
            {
                double dt = std::abs((m_measureHasFirst?m_measureT2:guideX) - m_measureT1);
                coordText += QString(" |  Δt=%1%2 (t1=%3, t2=%4)")
                    .arg(formatTime(dt, 3))
                    .arg(loader->getTimeUnits())
                    .arg(formatTime(m_measureT1, 3))
                    .arg(formatTime(m_measureHasFirst?m_measureT2:guideX, 3));
            }
            else
            {
                coordText += QString("  |  Pick first point...");
            }
        }
        m_mainWindow->getCoordLabel()->setText(coordText);
        // PERF NOTE: Must use rpQueuedReplot, not replot(). Synchronous replot() blocks
        // the UI thread for every mouse move event — when the plot contains many data points
        // (e.g. ADC phase), this causes the red guide line to lag seconds behind the cursor.
        // rpQueuedReplot coalesces rapid successive requests into a single actual repaint.
		m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
	}

	if (blockIdx < 0) return;

	const auto& blk = loader->getDecodedSeqBlocks()[blockIdx];
	double t0 = edges[blockIdx];
	double tFactor = loader->getTFactor();

	QString tip;
	// ADC tooltip disabled by request

	// Label tooltip: Show current active labels when hovering ADC axis
    // Controlled by Settings "Show extension tooltip" (Default: invalid/false)
    if (Settings::getInstance().getShowExtensionTooltip())
    {
        bool overAdc = false;
        if (m_mainWindow && m_mainWindow->getWaveformDrawer()) {
            auto rects = m_mainWindow->getWaveformDrawer()->getRects();
            if (!rects.isEmpty()) {
                QCPAxisRect* adcRect = rects[0];
                if (adcRect && adcRect->visible() && adcRect->rect().contains(event->pos())) {
                    overAdc = true;
                }
            }
        }

        if (overAdc)
        {
            // Use new helper to get all active labels for this block (even if not changing)
            auto activeLabels = m_mainWindow->getPulseqLoader()->getActiveLabels(blockIdx);
            for (const auto& pair : activeLabels) {
                if (!tip.isEmpty()) tip += "\n";
                tip += QString("%1=%2").arg(pair.first).arg(pair.second);
            }
        }
    }

	// Trigger tooltip
	if (blk->isTrigger())
	{
		const TriggerEvent& trg = blk->GetTriggerEvent();
		double tTrig = t0 + trg.delay * tFactor;
		double tol = (m_mainWindow->ui->customPlot->xAxis->range().size()) * 0.01;
		if (std::abs(xCoord - tTrig) <= tol)
		{
			if (!tip.isEmpty()) tip += "\n";
			// Tooltip should show trigger type only; time is not needed.
			tip += QString("Trigger type:%1").arg(trg.triggerType);
		}
	}

	if (!tip.isEmpty())
	{
		QToolTip::showText(QCursor::pos(), tip, m_mainWindow);
		m_tooltipVisible = true;
		m_tooltipTimer->start();
	}
	else if (m_tooltipVisible)
	{
		QToolTip::hideText();
		m_tooltipVisible = false;
	}
}

void InteractionHandler::onMousePress(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_measureMode)
    {
        double xCoord = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(event->pos().x());
        if (!m_measureHasFirst)
        {
            m_measureHasFirst = true;
            m_measureT1 = xCoord;
            m_measureT2 = xCoord;
        }
        else
        {
            m_measureT2 = xCoord;
            // After confirming second point, exit measure mode (click-to-finish)
            exitMeasureDtMode();
        }
        event->accept();
        return;
    }
    int axisIndex = -1;
    if (isOverAxisLabelArea(event->pos(), axisIndex))
    {
        // Defer: only start drag after threshold movement to prevent double-click triggering
        m_pendingAxisIndex = axisIndex;
        m_pressPos = event->pos();
        event->accept();
        return;
    }
}

void InteractionHandler::onMouseRelease(QMouseEvent* event)
{
    if (m_axisDragging)
    {
        endAxisDrag(event->pos());
        m_mainWindow->getWaveformDrawer()->finishAxisDragVisual();
        event->accept();
        return;
    }
}
void InteractionHandler::toggleMeasureDtMode()
{
    m_measureMode = !m_measureMode;
    if (!m_measureMode) { exitMeasureDtMode(); }

    // Sync UI action checked state so toolbar/button styling reflects active mode.
    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->actionMeasureDt)
        m_mainWindow->ui->actionMeasureDt->setChecked(m_measureMode);
}

void InteractionHandler::exitMeasureDtMode()
{
    // clear visuals
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    for (int i = 0; i < m_measureLines.size(); ++i) { auto* l = m_measureLines[i]; if (l) { plot->removeItem(l); } }
    for (int i = 0; i < m_measureShades.size(); ++i) { auto* r = m_measureShades[i]; if (r) { plot->removeItem(r); } }
    m_measureLines.clear();
    m_measureShades.clear();
    m_measureHasFirst = false;
    m_measureMode = false;
    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->actionMeasureDt)
        m_mainWindow->ui->actionMeasureDt->setChecked(false);
    plot->replot();
}


void InteractionHandler::onMouseWheel(QWheelEvent* event)
{
    int delta = event->angleDelta().y();
    if (delta == 0) return;
    // Accumulate and coalesce wheel events; process at ~70Hz to avoid zoom bursts
    m_accumulatedWheelDelta += delta;
    m_lastWheelPos = event->position();
    m_lastWheelModifiers = event->modifiers();
    if (!m_wheelTimer->isActive())
        m_wheelTimer->start();
}

void InteractionHandler::showContextMenu(const QPoint& pos)
{
    m_rightClickPos = pos;
    QMenu contextMenu(tr("Context menu"), m_mainWindow);

    QAction* actionInformation = new QAction("Information", m_mainWindow);
    connect(actionInformation, &QAction::triggered, this, &InteractionHandler::showBlockInformation);
    contextMenu.addAction(actionInformation);

    QAction* actionRaw = new QAction("Raw block data", m_mainWindow);
    connect(actionRaw, &QAction::triggered, this, &InteractionHandler::showRawBlockData);
    contextMenu.addAction(actionRaw);

    contextMenu.addSeparator();

    QAction* actionZoomIn = new QAction("Zoom In", m_mainWindow);
    connect(actionZoomIn, &QAction::triggered, this, &InteractionHandler::zoomIn);
    contextMenu.addAction(actionZoomIn);

    QAction* actionZoomOut = new QAction("Zoom Out", m_mainWindow);
    connect(actionZoomOut, &QAction::triggered, this, &InteractionHandler::zoomOut);
    contextMenu.addAction(actionZoomOut);

    contextMenu.exec(m_mainWindow->ui->customPlot->mapToGlobal(pos));
}

void InteractionHandler::showRawBlockData()
{
    double x = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(m_rightClickPos.x());
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader) return;

    int currentBlock = -1;
    const auto& edges = loader->getBlockEdges();
    if (edges.size() > 1)
    {
        auto upper = std::upper_bound(edges.begin(), edges.end(), x);
        int candidate = static_cast<int>(upper - edges.begin()) - 1;
        if (candidate >= 0 && candidate < static_cast<int>(loader->getDecodedSeqBlocks().size()))
            currentBlock = candidate;
    }
    if (currentBlock < 0) return;

    if (!m_pBlockInfoDialog)
        m_pBlockInfoDialog = new EventBlockInfoDialog(m_mainWindow);
    loader->setRawBlockInfoContent(m_pBlockInfoDialog, currentBlock);
    m_pBlockInfoDialog->show();
}

void InteractionHandler::zoomIn()
{
    /*
     * RIGHT-CLICK MENU ZOOM IN:
     * 
     * This method is called when user right-clicks and selects "Zoom In"
     * from the context menu. It uses the right-click position as zoom center.
     * 
     * Design:
     * - Uses right-click position as zoom center
     * - Applies range limits to prevent extreme zooming
     * - Applies boundary restrictions to prevent negative time values
     * - No tooltips for smooth user experience
     */
    
    // Get current range and calculate new range with limits
    double currentMin = m_mainWindow->ui->customPlot->xAxis->range().lower;
    double currentMax = m_mainWindow->ui->customPlot->xAxis->range().upper;
    double currentRange = currentMax - currentMin;
    
    double zoomFactor = 1.2;
    double newRange = currentRange / zoomFactor;  // Zoom in: reduce range
    
    // Apply range limits to prevent extreme zooming
    double minRange = currentRange * 0.001;   // Minimum 0.1% of current range
    double maxRange = currentRange * 1000.0;  // Maximum 1000x current range
    
    if (newRange < minRange) newRange = minRange;
    if (newRange > maxRange) newRange = maxRange;
    
    // CORRECT IMPLEMENTATION: Mouse-centered zoom like Adobe Illustrator
    // Step 1: Get right-click position in view coordinates
    QPointF posView = m_rightClickPos;
    
    // Step 2: Map to scene coordinates (data coordinates) BEFORE scaling
    double posSceneBefore = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(posView.x());
    
    // Step 3: Calculate new range based on zoom factor
    double newMin = currentMin + (currentRange - newRange) / 2.0;
    double newMax = currentMax - (currentRange - newRange) / 2.0;
    
    // Step 4: Apply boundary restrictions
    QCPRange validRange = getCurrentTimeRange();
    double minBoundary = validRange.lower;
    if (newMin < minBoundary)
    {
        double shift = minBoundary - newMin;
        newMin = minBoundary;
        newMax += shift;
    }
    
    // Step 5: Apply the new range (this is the scaling operation)
    m_mainWindow->ui->customPlot->xAxis->setRange(newMin, newMax);
    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(newMin, newMax);
    }
    
    // Step 6: Map to scene coordinates AFTER scaling
    double posSceneAfter = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(posView.x());
    
    // Step 7: Calculate offset and adjust translation to keep mouse point fixed
    double delta = posSceneAfter - posSceneBefore;
    double adjustedMin = newMin - delta;
    double adjustedMax = newMax - delta;
    
    // Step 8: Apply final range with translation compensation
    m_mainWindow->ui->customPlot->xAxis->setRange(adjustedMin, adjustedMax);
    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(adjustedMin, adjustedMax);
    }
    
    m_mainWindow->ui->customPlot->replot();
}

void InteractionHandler::zoomOut()
{
    /*
     * RIGHT-CLICK MENU ZOOM OUT:
     * 
     * This method is called when user right-clicks and selects "Zoom Out"
     * from the context menu. It uses the right-click position as zoom center.
     * 
     * Design:
     * - Uses right-click position as zoom center
     * - Applies range limits to prevent zooming out beyond initial time range
     * - Applies boundary restrictions to prevent negative time values
     * - No tooltips for smooth user experience
     */
    
    // Get current range and calculate new range with limits
    double currentMin = m_mainWindow->ui->customPlot->xAxis->range().lower;
    double currentMax = m_mainWindow->ui->customPlot->xAxis->range().upper;
    double currentRange = currentMax - currentMin;
    
    double zoomFactor = 1.2;
    double newRange = currentRange * zoomFactor;
    
    // Apply range limits to prevent extreme zooming out
    double minRange = currentRange * 0.001;   // Minimum 0.1% of current range
    
    // CRITICAL: Zoom out should be limited to the initial loaded time range
    QCPRange validRange = getCurrentTimeRange();
    double maxAllowedRange = validRange.upper - validRange.lower;  // Full time range
    double maxRange = qMin(currentRange * 1000.0, maxAllowedRange);  // Limit to full range
    
    if (newRange < minRange) newRange = minRange;
    if (newRange > maxRange) newRange = maxRange;
    
    // CORRECT IMPLEMENTATION: Mouse-centered zoom like Adobe Illustrator
    // Step 1: Get right-click position in view coordinates
    QPointF posView = m_rightClickPos;
    
    // Step 2: Map to scene coordinates (data coordinates) BEFORE scaling
    double posSceneBefore = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(posView.x());
    
    // Step 3: Calculate new range based on zoom factor
    double newMin = currentMin + (currentRange - newRange) / 2.0;
    double newMax = currentMax - (currentRange - newRange) / 2.0;
    
    // Step 4: Apply boundary restrictions
    if (newMin < validRange.lower)
    {
        double shift = validRange.lower - newMin;
        newMin = validRange.lower;
        newMax += shift;
    }
    
    // Step 5: Apply the new range (this is the scaling operation)
    m_mainWindow->ui->customPlot->xAxis->setRange(newMin, newMax);
    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(newMin, newMax);
    }
    
    // Step 6: Map to scene coordinates AFTER scaling
    double posSceneAfter = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(posView.x());
    
    // Step 7: Calculate offset and adjust translation to keep mouse point fixed
    double delta = posSceneAfter - posSceneBefore;
    double adjustedMin = newMin - delta;
    double adjustedMax = newMax - delta;
    
    // Step 8: Apply final range with translation compensation
    m_mainWindow->ui->customPlot->xAxis->setRange(adjustedMin, adjustedMax);
    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(adjustedMin, adjustedMax);
    }
    
    m_mainWindow->ui->customPlot->replot();
}

void InteractionHandler::showBlockInformation()
{
    double x = m_mainWindow->ui->customPlot->xAxis->pixelToCoord(m_rightClickPos.x());
    double minDist = std::numeric_limits<double>::max();
    double closestX = 0;
    bool found = false;

    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer) return;

    for (auto rect : drawer->getRects())
    {
        for (int i = 0; i < rect->graphs().size(); ++i)
        {
            QCPGraph* graph = rect->graphs().at(i);
            if (graph)
            {
                auto it = graph->data()->findBegin(x);
                if (it != graph->data()->end())
                {
                    double dist = std::abs(it->key - x);
                    if (dist < minDist)
                    {
                        minDist = dist;
                        closestX = it->key;
                        found = true;
                    }
                }
            }
        }
    }

    if (found)
    {
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        int currentBlock = -1;
        const auto& vecBlockEdges = loader->getBlockEdges();
        for (int i = 0; i < vecBlockEdges.size() - 1; ++i)
        {
            if (closestX >= vecBlockEdges[i] && closestX < vecBlockEdges[i + 1])
            {
                currentBlock = i;
                break;
            }
        }

        if (currentBlock != -1)
        {
            if (!m_pBlockInfoDialog)
            {
                m_pBlockInfoDialog = new EventBlockInfoDialog(m_mainWindow);
            }
            loader->setBlockInfoContent(m_pBlockInfoDialog, currentBlock);
            m_pBlockInfoDialog->show();
        }
    }
}

void InteractionHandler::synchronizeXAxes(const QCPRange& newRange)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer) return;

    // Reentrancy guard
    if (m_syncInProgress) return;
    QCPRange adjustedRange = newRange;
    
    if (m_enablePanBoundaries)
    {
        // Get the valid time range boundaries
        QCPRange validRange = getCurrentTimeRange();
        double minBoundary = validRange.lower;
        double maxBoundary = validRange.upper;
        
        bool hitLeftBoundary = false;
        bool hitRightBoundary = false;
        
        // Apply boundary restrictions
        if (adjustedRange.lower < minBoundary)
        {
            double adjustment = minBoundary - adjustedRange.lower;
            adjustedRange.lower = minBoundary;
            adjustedRange.upper += adjustment;
            hitLeftBoundary = true;
        }
        
        if (adjustedRange.upper > maxBoundary)
        {
            double adjustment = adjustedRange.upper - maxBoundary;
            adjustedRange.upper = maxBoundary;
            adjustedRange.lower -= adjustment;
            hitRightBoundary = true;
        }

        // Final clamp to ensure left boundary did not underflow after right-boundary correction
        // Preserve range width where possible; if width exceeds available span, clamp to [min,max]
        double width = adjustedRange.upper - adjustedRange.lower;
        if (width <= 0)
        {
            // Keep a minimal positive width to avoid invalid ranges
            width = 1.0;
            adjustedRange.lower = minBoundary;
            adjustedRange.upper = std::min(maxBoundary, adjustedRange.lower + width);
        }

        double available = maxBoundary - minBoundary;
        if (available <= 0)
        {
            // Degenerate boundary; fall back to a safe non-negative window
            adjustedRange.lower = std::max(0.0, minBoundary);
            adjustedRange.upper = adjustedRange.lower + width;
        }
        else if (width >= available)
        {
            // Requested width larger than allowed span: show the full allowed span
            adjustedRange.lower = minBoundary;
            adjustedRange.upper = maxBoundary;
        }
        else if (adjustedRange.lower < minBoundary)
        {
            adjustedRange.lower = minBoundary;
            adjustedRange.upper = adjustedRange.lower + width;
        }
        else if (adjustedRange.upper > maxBoundary)
        {
            adjustedRange.upper = maxBoundary;
            adjustedRange.lower = adjustedRange.upper - width;
            if (adjustedRange.lower < minBoundary)
            {
                adjustedRange.lower = minBoundary;
            }
        }
        
        // Show tooltip if boundary was hit
        if (hitLeftBoundary || hitRightBoundary)
        {
            QString message;
            if (hitLeftBoundary && hitRightBoundary) {
                message = "Reached time range boundary, cannot continue dragging";
            } else if (hitLeftBoundary) {
                message = "Reached left boundary, cannot continue dragging left";
            } else {
                message = "Reached right boundary, cannot continue dragging right";
            }
            showBoundaryTooltip(message);
        }
        
        // Ensure the range is still valid after adjustments
        if (adjustedRange.lower >= adjustedRange.upper)
        {
            // If the range becomes invalid, keep the current range
            adjustedRange = newRange;
        }
    }

    m_syncInProgress = true;
    for (int i = 0; i < drawer->getRects().count(); i++)
    {
        drawer->getRects()[i]->axis(QCPAxis::atBottom)->blockSignals(true);
    }
    for (int i = 0; i < drawer->getRects().count(); i++)
    {
        drawer->getRects()[i]->axis(QCPAxis::atBottom)->setRange(adjustedRange);
    }
    for (int i = 0; i < drawer->getRects().count(); i++)
    {
        drawer->getRects()[i]->axis(QCPAxis::atBottom)->blockSignals(false);
    }

    // Trigger viewport-aware re-render based on viewport and zoom level
    if (WaveformDrawer* d = m_mainWindow->getWaveformDrawer())
    {
        d->ensureRenderedForCurrentViewport();
    }
    // Reflect the new viewport to time controls silently
    if (m_mainWindow->getTRManager())
    {
        m_mainWindow->getTRManager()->syncTimeControlsToAxisRange(adjustedRange);
    }
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
    {
        m_mainWindow->refreshTrajectoryPlotData();
    }
    // No extra replot here; WaveformDrawer::ensureRenderedForCurrentViewport() already replots
    m_syncInProgress = false;
}

void InteractionHandler::processAccumulatedWheel()
{
    int delta = m_accumulatedWheelDelta;
    if (delta == 0) return;
    m_accumulatedWheelDelta = 0;

    bool ctrl = m_lastWheelModifiers & Qt::ControlModifier;
    Settings& appSettings = Settings::getInstance();
    Settings::ZoomInputMode zoomMode = appSettings.getZoomInputMode();
    bool panWheelEnabled = appSettings.getPanWheelEnabled();
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    QCPRange cur = plot->xAxis->range();
    QCPRange valid = getCurrentTimeRange();
    double minBoundary = valid.lower, maxBoundary = valid.upper;
    double currentRange = cur.size();

    auto doZoom = [&](int wheelDelta){
        // Zoom: anchor under mouse remains fixed. Use exponential factor per wheel tick.
        double ticks = wheelDelta / 120.0; // typical mouse wheel notch = 120
        double base = 0.9;                 // 10% per tick in
        double factor = std::pow(base, ticks); // ticks<0 -> >1 (zoom out)

        double anchor = plot->xAxis->pixelToCoord(m_lastWheelPos.x());
        if (currentRange <= 0) currentRange = (maxBoundary - minBoundary) * 0.1;
        double newRange = currentRange * factor;

        // Clamp newRange between 0.1% of full allowed span and full span
        double fullSpan = std::max(1e-9, maxBoundary - minBoundary);
        double relativeMinRange = currentRange * 0.001;
        if (relativeMinRange < 1e-9) relativeMinRange = 1e-9;
        if (newRange < relativeMinRange) newRange = relativeMinRange;
        if (newRange > fullSpan) newRange = fullSpan;

        double s = (anchor - cur.lower) / (currentRange == 0 ? 1.0 : currentRange);
        double newMin = anchor - s * newRange;
        double newMax = newMin + newRange;

        // Clamp to allowed boundaries
        if (newMin < minBoundary) { newMin = minBoundary; newMax = newMin + newRange; }
        if (newMax > maxBoundary) { newMax = maxBoundary; newMin = newMax - newRange; }
        if (newMin < minBoundary) newMin = minBoundary;
        if (newMax <= newMin) newMax = newMin + 1.0;

        synchronizeXAxes(QCPRange(newMin, newMax));
    };

    auto doPan = [&](int wheelDelta){
        // Pan: move by 10% of current range per wheel notch.
        double ticks = wheelDelta / 120.0;
        double step = currentRange * 0.1 * (ticks < 0 ? 1.0 : -1.0);
        double newMin = cur.lower + step;
        double newMax = cur.upper + step;

        // Clamp to boundaries
        if (newMin < minBoundary) { double d = minBoundary - newMin; newMin += d; newMax += d; }
        if (newMax > maxBoundary) { double d = newMax - maxBoundary; newMin -= d; newMax -= d; }
        if (newMax <= newMin) newMax = newMin + 1.0;

        synchronizeXAxes(QCPRange(newMin, newMax));
    };

    // Behavior matrix based on settings:
    // - Zoom mode = Wheel: always zoom with wheel; pan by wheel is disabled
    // - Zoom mode = CtrlWheel:
    //     * Ctrl pressed => zoom
    //     * Ctrl not pressed => if panWheelEnabled then pan, else ignore
    if (zoomMode == Settings::ZoomInputMode::Wheel)
    {
        doZoom(delta);
    }
    else // CtrlWheel
    {
        if (ctrl)
        {
            doZoom(delta);
        }
        else if (panWheelEnabled)
        {
            doPan(delta);
        }
        else
        {
            // Ignore wheel for pan if disabled
            return;
        }
    }
}

bool InteractionHandler::eventFilter(QObject* obj, QEvent* event)
{
    TRManager* trManager = m_mainWindow->getTRManager();
    if (!trManager) return false;

    // Axis drag begin/end (use label area to start) — only when interacting with the main plot.
    if (obj == m_mainWindow->ui->customPlot)
    {
        if (event->type() == QEvent::MouseButtonPress)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                int axisIndex = -1;
                // Coordinates are already in plot widget space
                QPoint plotPos = me->pos();
                if (isOverAxisLabelArea(plotPos, axisIndex) && axisIndex >= 0)
                {
                    // Don't start drag immediately; wait until threshold surpassed
                    m_pendingAxisIndex = axisIndex;
                    m_pressPos = plotPos;
                    return true;
                }
            }
        }
        if (event->type() == QEvent::MouseMove && !m_axisDragging && m_pendingAxisIndex >= 0)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            QPoint plotPos = me->pos();
            if ((plotPos - m_pressPos).manhattanLength() >= m_dragStartThresholdPx)
            {
                beginAxisDrag(m_pendingAxisIndex, m_pressPos);
                m_mainWindow->getWaveformDrawer()->startAxisDragVisual(m_pendingAxisIndex, m_pressPos);
            }
        }
        if (event->type() == QEvent::MouseMove && m_axisDragging)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            QPoint plotPos = me->pos();
            updateAxisDrag(plotPos);
            m_mainWindow->getWaveformDrawer()->updateAxisDragVisual(plotPos.y());
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            Q_UNUSED(me);
            if (m_axisDragging)
            {
                QPoint plotPos = m_mainWindow->ui->customPlot->mapFromGlobal(QCursor::pos());
                endAxisDrag(plotPos);
                m_mainWindow->getWaveformDrawer()->finishAxisDragVisual();
            }
            // Reset pending state to avoid treating click/double-click as drag
            m_pendingAxisIndex = -1;
            return false;
        }
    }

    if (event->type() == QEvent::Wheel)
    {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
        if (obj == trManager->getTrStartInput() || obj == trManager->getTrEndInput())
        {
            handleTrInputWheelEvent(wheelEvent, static_cast<QLineEdit*>(obj));
            return true;
        }
        else if (obj == trManager->getTimeStartInput() || obj == trManager->getTimeEndInput())
        {
            handleTimeInputWheelEvent(wheelEvent, static_cast<QLineEdit*>(obj));
            return true;
        }
    }
    // Key handling:
    //  - Arrow up/down for TR/time inputs (handled above)
    //  - Global shortcuts for pan and TR stepping (arrows, A/D, Alt+1/Alt+2)
    //  - ESC to exit measure mode
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        // Up/Down to change TR start/end values
        if (obj == trManager->getTrStartInput() || obj == trManager->getTrEndInput())
        {
            if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
            {
                QLineEdit* input = static_cast<QLineEdit*>(obj);
                int currentValue = input->text().toInt();
                int delta = (ke->key() == Qt::Key_Up) ? 1 : -1;
                int minTr = 1;
                int maxTr = trManager->getTrRangeSlider()->maximum() + 1;
                int newValue = currentValue + delta;
                if (newValue < minTr) newValue = minTr;
                if (newValue > maxTr) newValue = maxTr;
                if (newValue != currentValue)
                {
                    input->setText(QString::number(newValue));
                    if (input == trManager->getTrStartInput())
                        trManager->onTrStartInputChanged();
                    else
                        trManager->onTrEndInputChanged();
                }
                return true; // consume Up/Down for TR inputs
            }
        }
        // Up/Down to change Time start/end values (ms), step derived from slider span
        if (obj == trManager->getTimeStartInput() || obj == trManager->getTimeEndInput())
        {
            if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
            {
                QLineEdit* input = static_cast<QLineEdit*>(obj);
                double currentValue = input->text().toDouble();
                int minTime = trManager->getTimeRangeSlider()->minimum();
                int maxTime = trManager->getTimeRangeSlider()->maximum();
                double rangeSize = static_cast<double>(maxTime - minTime);
                double step = rangeSize / 10.0; if (step < 1.0) step = 1.0;
                double delta = (ke->key() == Qt::Key_Up) ? step : -step;
                double newValue = currentValue + delta;
                if (newValue < minTime) newValue = minTime;
                if (newValue > maxTime) newValue = maxTime;
                if (std::round(newValue) != std::round(currentValue))
                {
                    input->setText(QString::number(static_cast<int>(std::round(newValue))));
                    if (input == trManager->getTimeStartInput())
                        trManager->onTimeStartInputChanged();
                    else
                        trManager->onTimeEndInputChanged();
                }
                return true; // consume Up/Down for Time inputs
            }
        }
        // ESC to exit measurement mode
        if (ke->key() == Qt::Key_Escape && m_measureMode)
        {
            exitMeasureDtMode();
            return true;
        }

        // Normalize modifiers (ignore Shift for simplicity here)
        const Qt::KeyboardModifiers mods = ke->modifiers();
        // Debug hotkey trace for Alt+1 / Alt+2 investigation
        if (mods & Qt::AltModifier)
        {
            qInfo().noquote() << "[Hotkey] KeyPress key=" << ke->key()
                              << " mods=" << static_cast<int>(mods);
        }

        // 1) TR stepping (global, independent of focus)
        //    Alt+Q: decrease TR start/end by |Inc| (default 1 if Inc empty or 0)
        //    Alt+W: increase TR start/end by |Inc| (default 1 if Inc empty or 0)
        bool trStepShortcut = false;
        int  trStepSign = 0; // +1 or -1

        if ((mods & Qt::AltModifier) && (ke->key() == Qt::Key_Q || ke->key() == Qt::Key_W))
        {
            trStepShortcut = true;
            trStepSign = (ke->key() == Qt::Key_Q) ? -1 : +1;
        }

        if (trStepShortcut && trStepSign != 0)
        {
            TRManager* trm = m_mainWindow->getTRManager();
            if (!trm)
                return false;

            QLineEdit* trStart = trm->getTrStartInput();
            QLineEdit* trEnd   = trm->getTrEndInput();
            QLineEdit* trInc   = trm->getTrIncInput();
            if (!trStart || !trEnd || !trInc)
                return false;

            bool okInc = false;
            int incVal = trInc->text().toInt(&okInc);
            int step = okInc ? std::abs(incVal) : 1;
            if (step <= 0)
                step = 1;

            step *= trStepSign;

            bool okS = false, okE = false;
            int startTr = trStart->text().toInt(&okS);
            int endTr   = trEnd->text().toInt(&okE);
            if (!okS || !okE)
                return false;

            startTr += step;
            endTr   += step;

            PulseqLoader* loader = m_mainWindow->getPulseqLoader();
            if (!loader)
                return false;
            int trCount = loader->getTrCount();
            if (trCount <= 0)
                return false;

            // Clamp to [1, trCount]
            startTr = std::clamp(startTr, 1, trCount);
            endTr   = std::clamp(endTr,   1, trCount);
            if (startTr > endTr)
                startTr = endTr;

            {
                const QSignalBlocker b1(trStart);
                const QSignalBlocker b2(trEnd);
                trStart->setText(QString::number(startTr));
                trEnd->setText(QString::number(endTr));
            }
            {
                const QSignalBlocker blocker(trm->getTrRangeSlider());
                trm->getTrRangeSlider()->setValues(startTr - 1, endTr - 1);
            }
            trm->updateTrRangeDisplay();
            return true;
        }

        // 2) Time window stepping (global, independent of focus)
        //    Alt+E: decrease Time start/end by |Inc| (ms). If Inc empty/0, use ~10% of current window.
        //    Alt+R: increase Time start/end by |Inc|.
        //    Ctrl+Left:  decrease Time start/end by |Inc| (same as Alt+E, fallback)
        //    Ctrl+Right: increase Time start/end by |Inc| (same as Alt+R)
        bool timeStepShortcut = false;
        double timeStepSign = 0.0; // +1.0 or -1.0

        if ((mods & Qt::AltModifier) && (ke->key() == Qt::Key_E || ke->key() == Qt::Key_R))
        {
            timeStepShortcut = true;
            timeStepSign = (ke->key() == Qt::Key_E) ? -1.0 : +1.0;
        }
        else if ((mods & Qt::ControlModifier) && (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right))
        {
            timeStepShortcut = true;
            timeStepSign = (ke->key() == Qt::Key_Left) ? -1.0 : +1.0;
        }

        if (timeStepShortcut && timeStepSign != 0.0)
        {
            TRManager* trm = m_mainWindow->getTRManager();
            if (!trm)
                return false;

            QLineEdit* timeStart = trm->getTimeStartInput();
            QLineEdit* timeEnd   = trm->getTimeEndInput();
            QLineEdit* timeInc   = trm->getTimeIncInput();
            DoubleRangeSlider* timeSlider = trm->getTimeRangeSlider();
            if (!timeStart || !timeEnd || !timeInc || !timeSlider)
                return false;

            // Determine magnitude from Inc field if available
            bool okInc = false;
            double incVal = timeInc->text().toDouble(&okInc);
            double mag = (okInc && std::abs(incVal) > 0.0) ? std::abs(incVal) : 0.0;

            // Fallback: ~10% of current time window (or at least 1 ms)
            if (mag <= 0.0)
            {
                bool okS = false, okE = false;
                double s = timeStart->text().toDouble(&okS);
                double e = timeEnd->text().toDouble(&okE);
                if (!okS || !okE)
                    return false;
                double len = std::abs(e - s);
                if (len <= 0.0) len = 1.0;
                mag = len / 10.0;
                if (mag < 1.0) mag = 1.0;
            }

            double inc = mag * timeStepSign;

            // Read current window
            bool okS = false, okE = false;
            double startMs = timeStart->text().toDouble(&okS);
            double endMs   = timeEnd->text().toDouble(&okE);
            if (!okS || !okE)
                return false;
            if (endMs < startMs)
                std::swap(startMs, endMs);
            double windowSize = endMs - startMs;
            if (windowSize < 0.0) windowSize = 0.0;

            double minAllowed = std::max(0.0, static_cast<double>(timeSlider->minimum()));
            double maxAllowed = static_cast<double>(timeSlider->maximum());

            // Propose shifted window
            double newStart = startMs + inc;
            double newEnd   = endMs   + inc;

            // Clamp while preserving window size as much as possible
            if (newStart < minAllowed)
            {
                newStart = minAllowed;
                newEnd = newStart + windowSize;
            }
            if (newEnd > maxAllowed)
            {
                newEnd = maxAllowed;
                newStart = newEnd - windowSize;
                if (newStart < minAllowed)
                    newStart = minAllowed;
            }
            if (newEnd < newStart)
                newEnd = newStart;

            trm->setTimeRange(newStart, newEnd);

            // Apply to axes (mirroring onTimeIncrementEditingFinished semantics)
            PulseqLoader* loader = m_mainWindow->getPulseqLoader();
            if (loader)
            {
                bool trMode = trm->isTrBasedMode() && loader->hasRepetitionTime();
                double absStart_ms, absEnd_ms;
                if (trMode)
                {
                    int startTr = trm->getTrStartInput()->text().toInt();
                    double trStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
                    absStart_ms = trStart_ms + newStart;
                    absEnd_ms   = trStart_ms + newEnd;
                }
                else
                {
                    absStart_ms = newStart;
                    absEnd_ms   = newEnd;
                }
                double tFactor = loader->getTFactor();
                double rangeStart = absStart_ms * tFactor * 1000.0;
                double rangeEnd   = absEnd_ms   * tFactor * 1000.0;
                if (auto ih = m_mainWindow->getInteractionHandler())
                {
                    ih->synchronizeXAxes(QCPRange(rangeStart, rangeEnd));
                }
            }
            return true;
        }

        // 3) Global pan shortcuts (only when event target is the main window or plot)
        QObject* target = obj;
        bool overMainOrPlot = (target == m_mainWindow ||
                               target == m_mainWindow->ui->customPlot);
        if (overMainOrPlot)
        {

            // Pan left / right
            //    - Left arrow / Right arrow
            //    - 'A' / 'a'  (left)
            //    - 'D' / 'd'  (right)
            bool handled = false;
            if (ke->key() == Qt::Key_Left)
            {
                m_mainWindow->getTRManager()->onPanLeftClicked();
                handled = true;
            }
            else if (ke->key() == Qt::Key_Right)
            {
                m_mainWindow->getTRManager()->onPanRightClicked();
                handled = true;
            }
            else if (ke->key() == Qt::Key_A || ke->key() == Qt::Key_D)
            {
                if (ke->key() == Qt::Key_A)
                    m_mainWindow->getTRManager()->onPanLeftClicked();
                else
                    m_mainWindow->getTRManager()->onPanRightClicked();
                handled = true;
            }

            if (handled)
                return true;
        }
    }
    return false; // Return false to let MainWindow continue processing
}
bool InteractionHandler::isOverAxisLabelArea(const QPoint& pos, int& axisIndex) const
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer) return false;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    // Hit test: allow drag only from the axis title area (left label & tick area), not plotting area
    int rowCount = plot->plotLayout()->rowCount();
    for (int row = 0; row < rowCount; ++row)
    {
        QCPLayoutElement* el = plot->plotLayout()->element(row, 0);
        QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
        if (!rect) continue;
        QRect plotRect = rect->rect();
        // Approximate title/labels area: extend left by 90 px and shrink right by 10 px to avoid plot area
        QRect titleArea(plotRect.left() - 90, plotRect.top(), 90, plotRect.height());
        if (titleArea.contains(pos)) { axisIndex = row; return true; }
    }
    return false;
}

void InteractionHandler::beginAxisDrag(int axisIndex, const QPoint& pos)
{
    m_axisDragging = true;
    m_dragSourceIndex = axisIndex;
    m_dragStartPos = pos;
    // Temporarily disable interactions to avoid conflict while dragging order
    m_prevInteractions = m_mainWindow->ui->customPlot->interactions();
    m_mainWindow->ui->customPlot->setInteractions(QCP::iNone);
    m_mainWindow->getWaveformDrawer()->showDropIndicatorAt(axisIndex);
}

void InteractionHandler::updateAxisDrag(const QPoint& pos)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer) return;
    // Compute target strictly within title band under mouse; if not over title, clear highlight
    int targetIndex = -1;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    if (plot && plot->plotLayout())
    {
        int rowCount = plot->plotLayout()->rowCount();
        for (int row = 0; row < rowCount; ++row)
        {
            QCPLayoutElement* el = plot->plotLayout()->element(row, 0);
            QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
            if (!rect) continue;
            QRect outer = rect->outerRect(); QRect plotRect = rect->rect();
            int leftBandWidth = plotRect.left() - outer.left(); if (leftBandWidth < 1) leftBandWidth = 1;
            QRect titleBand(outer.left(), outer.top(), leftBandWidth, outer.height());
            if (titleBand.contains(pos)) { targetIndex = row; break; }
        }
    }
    if (targetIndex >= 0)
        drawer->showDropIndicatorAt(targetIndex);
    else
        drawer->clearDropIndicator(); // Clear highlight when mouse leaves title area
}

void InteractionHandler::endAxisDrag(const QPoint& pos)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer) { m_axisDragging = false; m_pendingAxisIndex = -1; return; }
    int targetIndex = drawer->axisIndexAtPositionY(pos.y());
    if (targetIndex >= 0 && m_dragSourceIndex >= 0)
    {
        drawer->moveAxis(m_dragSourceIndex, targetIndex);
        drawer->saveUiConfig();
    }
    drawer->clearDropIndicator();
    m_axisDragging = false;
    m_dragSourceIndex = -1;
    m_pendingAxisIndex = -1; // Reset pending axis index to prevent issues
    // Restore interactions
    m_mainWindow->ui->customPlot->setInteractions(static_cast<QCP::Interactions>(m_prevInteractions));
}

void InteractionHandler::setPanBoundaryMode(bool enableBoundaries)
{
    m_enablePanBoundaries = enableBoundaries;
}

bool InteractionHandler::getPanBoundaryMode() const
{
    return m_enablePanBoundaries;
}

void InteractionHandler::showBoundaryTooltip(const QString& message)
{
    // Hide any existing tooltip
    if (m_tooltipVisible) {
        QToolTip::hideText();
        m_tooltipTimer->stop();
    }
    
    // Show new tooltip at mouse position
    QPoint globalPos = QCursor::pos();
    QToolTip::showText(globalPos, message, m_mainWindow);
    
    // Start auto-hide timer
    m_tooltipVisible = true;
    m_tooltipTimer->start();
}

QCPRange InteractionHandler::getCurrentTimeRange() const
{
    // Return the allowed pan/zoom extent, not the current visible range.
    // TR mode: [TR absolute start, TR absolute end]
    // Whole-sequence: [0, data end]
    TRManager* trManager = m_mainWindow->getTRManager();
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!trManager || !loader)
        return QCPRange(0, 1000);

    const double tFactor = loader->getTFactor();
    bool trMode = trManager->isTrBasedMode();

    if (trMode && loader->hasRepetitionTime())
    {
        int startTr = trManager->getTrStartInput()->text().toInt();
        int endTr = trManager->getTrEndInput()->text().toInt();
        double absoluteTrStartTime_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
        double absoluteTrEndTime_ms = endTr * loader->getRepetitionTime_us() / 1000.0;
        double rangeStart = absoluteTrStartTime_ms * tFactor * 1000.0;
        double rangeEnd = absoluteTrEndTime_ms * tFactor * 1000.0;
        if (rangeStart < 0) rangeStart = 0;
        if (rangeEnd <= rangeStart) rangeEnd = rangeStart + 1.0;
        return QCPRange(rangeStart, rangeEnd);
    }
    else
    {
        // Whole sequence extent: from 0 to last block edge
        double rangeStart = 0.0;
        double rangeEnd = 0.0;
        const auto& edges = loader->getBlockEdges();
        if (!edges.isEmpty())
            rangeEnd = edges.last();
        else
            rangeEnd = loader->getTotalDuration_us() * tFactor;
        if (rangeEnd <= rangeStart) rangeEnd = rangeStart + 1.0;
        return QCPRange(rangeStart, rangeEnd);
    }
}

void InteractionHandler::applyBoundaryRestrictionsToAllAxes()
{
    /*
     * BOUNDARY RESTRICTIONS FOR ZOOM OPERATIONS:
     * 
     * This method applies boundary restrictions to all axis rects after zoom operations.
     * It's designed to prevent showing negative time values while allowing
     * zooming beyond the data range for better user experience.
     * 
     * Key principles:
     * - Only restrict left boundary to prevent negative time values
     * - Allow zooming beyond right boundary (user might want to see beyond data)
     * - No tooltips for zoom operations (smooth experience)
     * - Ensure range validity after adjustments
     */
    
    // ALWAYS apply boundary restrictions to prevent negative time values
    // This is CRITICAL - never allow negative time display
    
    // Get the valid time range boundaries
    QCPRange validRange = getCurrentTimeRange();
    double minBoundary = validRange.lower;
    double maxBoundary = validRange.upper;
    
    bool hitLeftBoundary = false;
    bool hitRightBoundary = false;
    
    // Apply boundary restrictions to all axis rects
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        for (auto& rect : drawer->getRects())
        {
            QCPRange currentRange = rect->axis(QCPAxis::atBottom)->range();
            QCPRange adjustedRange = currentRange;
            
            // Apply boundary restrictions
            if (adjustedRange.lower < minBoundary)
            {
                adjustedRange.lower = minBoundary;
                hitLeftBoundary = true;
            }
            
            // Note: We don't restrict the right boundary for zoom operations
            // This allows users to zoom out beyond the data range if needed
            
            // Ensure the range is still valid after adjustments
            if (adjustedRange.lower >= adjustedRange.upper)
            {
                // If the range becomes invalid, keep the current range
                adjustedRange = currentRange;
            }
            
            // Set the adjusted range
            rect->axis(QCPAxis::atBottom)->setRange(adjustedRange);
        }
    }
    
    // Note: No tooltip for zoom operations - zoom should be smooth and unrestricted
}

void InteractionHandler::handleTrInputWheelEvent(QWheelEvent* event, QLineEdit* input)
{
    TRManager* trManager = m_mainWindow->getTRManager();
    if (!trManager) return;

    int currentValue = input->text().toInt();
    int delta = event->angleDelta().y() > 0 ? 1 : -1;
    int newValue = currentValue + delta;

    int minTr = 1;
    int maxTr = trManager->getTrRangeSlider()->maximum() + 1;

    if (newValue < minTr) newValue = minTr;
    if (newValue > maxTr) newValue = maxTr;

    input->setText(QString::number(newValue));

    if (input == trManager->getTrStartInput()) {
        trManager->onTrStartInputChanged();
    }
    else if (input == trManager->getTrEndInput()) {
        trManager->onTrEndInputChanged();
    }
}

void InteractionHandler::handleTimeInputWheelEvent(QWheelEvent* event, QLineEdit* input)
{
    TRManager* trManager = m_mainWindow->getTRManager();
    if (!trManager) return;

    double currentValue = input->text().toDouble();
    double minTime = trManager->getTimeRangeSlider()->minimum();
    double maxTime = trManager->getTimeRangeSlider()->maximum();
    double rangeSize = maxTime - minTime;
    double step = rangeSize / 10.0;
    if (step < 1.0) step = 1.0;

    int delta = event->angleDelta().y() > 0 ? 1 : -1;
    double newValue = currentValue + delta * step;

    if (newValue < minTime) newValue = minTime;
    if (newValue > maxTime) newValue = maxTime;

    input->setText(QString::number(static_cast<int>(std::round(newValue))));

    if (input == trManager->getTimeStartInput()) {
        trManager->onTimeStartInputChanged();
    }
    else if (input == trManager->getTimeEndInput()) {
        trManager->onTimeEndInputChanged();
    }
}
