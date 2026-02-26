#include "WaveformDrawer.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "InteractionHandler.h"
#include "Settings.h"
#include "ZoomManager.h"
#include "TRManager.h"
#include "PulseqLabelAnalyzer.h"
#include "ExtensionPlotter.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFont>
#include <QHash>
#include <QTimer>
#include <QPen>
#include <QtGlobal>
#include <chrono>

// Debug control for LTTB algorithm
static const bool DEBUG_LTTB = false; // Set to true to enable LTTB debug output

// Hash function for LODLevel enum
inline uint qHash(WaveformDrawer::LODLevel level, uint seed = 0)
{
    return qHash(static_cast<int>(level), seed);
}

/*
Overview of rendering strategy (performance-aware):

1) Viewport-based culling
   - We never draw data outside the current x-axis viewport (visibleStart..visibleEnd).
   - We map the viewport to a contiguous block index range using precomputed block edges,
     and only iterate those blocks.

2) Complexity-based LOD (Level of Detail) system
   - LOD levels are determined by the number of complex curves in the current viewport,
     not by viewport duration. This provides more intelligent performance optimization.
   - LODLevel::FULL_DETAIL: No downsampling for maximum accuracy
   - LODLevel::DOWNSAMPLED: LTTB downsampling for balanced performance
   - Thresholds are configurable via ZoomManager settings

3) Intelligent downsampling
   - Uses adaptive sampling that preserves peaks and valleys in scientific data
   - Maintains waveform characteristics while reducing data points
   - Cache system stores multiple LOD levels for efficient switching

4) TR-Segmented vs Whole-Sequence rendering
   - Whole-Sequence: viewport is the absolute time range; we clamp the upper bound to the
     end of data to avoid drawing beyond sequence end.
   - TR-Segmented: viewport is clamped to the current TR range selection [startTr, endTr]
     to prevent accidental rendering across very large multi-TR spans.

5) Incremental redraws
   - When viewport or LOD level changes, ensureRenderedForCurrentViewport() clears graphs
     and re-renders only the visible content at the appropriate complexity level.
*/
static void applyZoomSettingsToManager(ZoomManager* zm)
{
    if (!zm) return;
    Settings& s = Settings::getInstance();
    // We cannot set fields directly on ZoomManager (read-only API), so we rely on
    // loading from a transient JSON is not ideal. Instead, we compute level by thresholds from Settings
    // by wrapping getZoomLevel usage sites to read from Settings. For now, we only keep thresholds in Settings
    // by reading them at call sites.
}

#include <QPen>
#include <QDebug>
#include <chrono>
#include <complex>

// Debug logging removed for cleaner output in tests and runtime

WaveformDrawer::WaveformDrawer(MainWindow* mainWindow)
    : QObject(mainWindow),
      m_mainWindow(mainWindow),
      m_pADCLabelsRect(nullptr), m_pRfMagRect(nullptr), m_pRfADCPhaseRect(nullptr),
      m_pGxRect(nullptr), m_pGyRect(nullptr), m_pGzRect(nullptr),
      bShowBlocksEdges(false), m_nCurrentMaxPoints(MAX_POINTS_PER_GRAPH)
{
    // Initialize curve visibility - all curves visible by default
    m_curveVisibility.resize(6);
    for (int i = 0; i < 6; i++)
    {
        m_curveVisibility[i] = true;
    }
    
    // Initialize auto expand mode - default to true (auto expand behavior)
    m_autoExpandMode = true;

    // Default axes order (UI config default)
    m_axesOrder = QStringList() << "RF mag" << "GZ" << "GY" << "GX" << "RF/ADC ph" << "ADC/labels";
    // Initialize fixed Y ranges container
    m_fixedYRanges.resize(6);
}

WaveformDrawer::~WaveformDrawer()
{
    // All QCustomPlot items are owned by the plot itself.
}

void WaveformDrawer::InitSequenceFigure()
{
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    customPlot->clearGraphs();
    customPlot->plotLayout()->clear();
    // Prefer fast polylines and cache labels; keep AA during still, disable while dragging for responsiveness
    customPlot->setNoAntialiasingOnDrag(true);
    customPlot->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
    customPlot->setInteractions(QCP::iRangeDrag | QCP::iSelectPlottables);

    m_pADCLabelsRect = new QCPAxisRect(customPlot);
    m_pRfMagRect = new QCPAxisRect(customPlot);
    m_pRfADCPhaseRect = new QCPAxisRect(customPlot);
    m_pGxRect = new QCPAxisRect(customPlot);
    m_pGyRect = new QCPAxisRect(customPlot);
    m_pGzRect = new QCPAxisRect(customPlot);

    m_vecRects.append(m_pADCLabelsRect);
    m_vecRects.append(m_pRfMagRect);
    m_vecRects.append(m_pRfADCPhaseRect);
    m_vecRects.append(m_pGxRect);
    m_vecRects.append(m_pGyRect);
    m_vecRects.append(m_pGzRect);

    auto m_pMarginGroup = new QCPMarginGroup(customPlot);
    // Single-column grid: only plot rects; drag will target axis label area directly
    for (int i = 0; i < m_vecRects.count(); i++)
    {
        customPlot->plotLayout()->addElement(i, 0, m_vecRects[i]);
        m_vecRects[i]->axis(QCPAxis::atLeft)->setRange(-1, 1);
        m_vecRects[i]->axis(QCPAxis::atBottom)->setRange(0, 100);
        m_vecRects[i]->setMarginGroup(QCP::msLeft, m_pMarginGroup);
        m_vecRects[i]->setMargins(QMargins(0, 0, 0, 2));
        m_vecRects[i]->setMinimumMargins(QMargins(0, 0, 0, 0));
        m_vecRects[i]->setAutoMargins(QCP::msLeft);

        // Keep a non-zero height even in small windows (e.g. tiled --layout 211).
        // Without a minimum height, the last subplot can get collapsed to 0px by the layout system.
        m_vecRects[i]->setMinimumSize(QSize(0, 70));
    }
    // Remove outer margins on the root grid to eliminate unused vertical space
    customPlot->plotLayout()->setAutoMargins(QCP::msNone);
    customPlot->plotLayout()->setMargins(QMargins(0, 0, 0, 0));
    customPlot->plotLayout()->setRowSpacing(0);
    // Single column layout: no column spacing

    // Initial stretch factors will be set by updateCurveVisibility()
    // Don't set them here as they will be overridden

    // Load saved UI configuration (axes order)
    loadUiConfig();
    
    // Configure x-axis labels based on axes order
    // First, hide x-axis labels on all axes
    for (int i = 0; i < m_vecRects.count(); i++)
    {
        m_vecRects[i]->axis(QCPAxis::atBottom)->setTickLabels(false);
        m_vecRects[i]->axis(QCPAxis::atBottom)->setLabel("");
    }
    
    // Then, show x-axis labels only on the bottom-most axis (last in m_axesOrder)
    if (!m_axesOrder.isEmpty()) {
        QString lastAxisName = m_axesOrder.last();
        qDebug() << "[X-AXIS] Last axis in order:" << lastAxisName;
        
        // Find the rect corresponding to the last axis name
        QCPAxisRect* targetRect = nullptr;
        if (lastAxisName == "ADC/labels") targetRect = m_pADCLabelsRect;
        else if (lastAxisName == "RF mag") targetRect = m_pRfMagRect;
        else if (lastAxisName == "RF/ADC ph") targetRect = m_pRfADCPhaseRect;
        else if (lastAxisName == "GX") targetRect = m_pGxRect;
        else if (lastAxisName == "GY") targetRect = m_pGyRect;
        else if (lastAxisName == "GZ") targetRect = m_pGzRect;
        
        if (targetRect) {
            targetRect->axis(QCPAxis::atBottom)->setTickLabels(true);
            targetRect->axis(QCPAxis::atBottom)->setLabel(currentTimeAxisLabel());
            applyTimeAxisFormatting(targetRect->axis(QCPAxis::atBottom));
            
            // Set label color and font to ensure visibility
            targetRect->axis(QCPAxis::atBottom)->setLabelColor(Qt::black);
            QFont labelFont = targetRect->axis(QCPAxis::atBottom)->labelFont();
            labelFont.setPointSize(10);
            labelFont.setBold(true);
            targetRect->axis(QCPAxis::atBottom)->setLabelFont(labelFont);
            
        // Increase bottom margin for the bottom axis to ensure x-axis labels are visible
        targetRect->setMargins(QMargins(0, 0, 0, 50)); // Increased to 50 for better visibility
            qDebug() << "[X-AXIS] Setting x-axis label on:" << lastAxisName;
            qDebug() << "[X-AXIS] Initial - Tick labels enabled:" << targetRect->axis(QCPAxis::atBottom)->tickLabels();
            qDebug() << "[X-AXIS] Initial - Label text:" << targetRect->axis(QCPAxis::atBottom)->label();
            qDebug() << "[X-AXIS] Initial - Label color:" << targetRect->axis(QCPAxis::atBottom)->labelColor();
            qDebug() << "[X-AXIS] Initial - Margins:" << targetRect->margins();
        }
    }

    // Set drag and zoom for all axis rects
    for (int i = 0; i < m_vecRects.count(); i++)
    {
        m_vecRects[i]->setRangeDrag(Qt::Horizontal);
        m_vecRects[i]->setRangeZoom(Qt::Horizontal);
    }

    m_pADCLabelsRect->axis(QCPAxis::atLeft)->setLabel("ADC/labels");
    m_pRfMagRect->axis(QCPAxis::atLeft)->setLabel("RF mag(Hz)");
    m_pRfADCPhaseRect->axis(QCPAxis::atLeft)->setLabel("RF/ADC ph(rad)");
    // Set gradient axis labels with current unit from settings
    Settings& settings = Settings::getInstance();
    QString gradientUnit = settings.getGradientUnitString();
    m_pGxRect->axis(QCPAxis::atLeft)->setLabel("GX (" + gradientUnit + ")");
    m_pGyRect->axis(QCPAxis::atLeft)->setLabel("GY (" + gradientUnit + ")");
    m_pGzRect->axis(QCPAxis::atLeft)->setLabel("GZ (" + gradientUnit + ")");

    for (int i = 0; i < m_vecRects.count(); ++i)
    {
        QCPAxis* axis = m_vecRects[i]->axis(QCPAxis::atBottom);
        InteractionHandler* handler = m_mainWindow->getInteractionHandler();
        if (axis && handler)
        {
            QObject::connect(axis,
                             QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
                             handler,
                             &InteractionHandler::synchronizeXAxes);
        }
    }

    // Try load zoom config if available
    if (!m_zoomManager)
    {
        m_zoomManager = new ZoomManager(this);
        m_zoomManager->loadConfig("zoom_config.json");
        // React to settings changes (thresholds and points)
        connect(&Settings::getInstance(), &Settings::settingsChanged, this, [this]() {
            // Recompute Y-axis ranges with the (potentially new) gradient unit conversion,
            // then redraw. Without this, changing e.g. mT/m → Hz/m would keep old Y-ranges.
            computeAndLockYAxisRanges();
            DrawRFWaveform();
            DrawADCWaveform();
            DrawGWaveform();
            if (getShowBlockEdges()) DrawBlockEdges();
            m_mainWindow->ui->customPlot->replot();
        });
    }

    colors = QVector<QColor>({
        QColor::fromRgbF(0,0.447,0.741), QColor::fromRgbF(0.85,0.325,0.098),
        QColor::fromRgbF(0.929,0.694,0.125), QColor::fromRgbF(0.494,0.184,0.556),
        QColor::fromRgbF(0.466,0.674,0.188), QColor::fromRgbF(0.301,0.745,0.933),
        QColor::fromRgbF(0.635,0.078,0.184)
    });
    
    // Set initial layout based on curve visibility
    updateCurveVisibility();

    // Create persistent graphs per axis rect to avoid runtime add/remove and flicker
    // ADC (rect 0)
    m_graphADC = customPlot->addGraph(m_pADCLabelsRect->axis(QCPAxis::atBottom), m_pADCLabelsRect->axis(QCPAxis::atLeft));
    if (m_graphADC)
    {
        QPen pen(QColor(231, 76, 60, 220));
        pen.setWidthF(1.5);
        pen.setCapStyle(Qt::FlatCap);
        m_graphADC->setPen(pen);
        // Use step style to ensure rectangular ADC envelopes (no diagonal edges on some platforms)
        m_graphADC->setLineStyle(QCPGraph::lsStepLeft);
        m_graphADC->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphADC->setAdaptiveSampling(false);
        // Disable AA to avoid slanted artifacts on low zoom levels (Linux-specific rendering differences)
        m_graphADC->setAntialiased(false);
        m_graphADC->setVisible(m_curveVisibility.value(0, true));
    }
    // RF Mag (rect 1)
    m_graphRFMag = customPlot->addGraph(m_pRfMagRect->axis(QCPAxis::atBottom), m_pRfMagRect->axis(QCPAxis::atLeft));
    if (m_graphRFMag)
    {
        QPen pen(colors.isEmpty() ? Qt::blue : colors[0 % colors.size()]);
        pen.setWidthF(1.5);
        m_graphRFMag->setPen(pen);
        m_graphRFMag->setLineStyle(QCPGraph::lsLine);
        m_graphRFMag->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphRFMag->setAdaptiveSampling(false);
        m_graphRFMag->setAntialiased(true); // RF needs smoothness to avoid jagged Gaussians
        m_graphRFMag->setVisible(m_curveVisibility.value(1, true));
    }
    // RF Phase (rect 2)
    m_graphRFPh = customPlot->addGraph(m_pRfADCPhaseRect->axis(QCPAxis::atBottom), m_pRfADCPhaseRect->axis(QCPAxis::atLeft));
    if (m_graphRFPh)
    {
        QPen pen(colors.isEmpty() ? Qt::darkGreen : colors[1 % colors.size()]);
        pen.setWidthF(1.5);
        m_graphRFPh->setPen(pen);
        m_graphRFPh->setLineStyle(QCPGraph::lsLine);
        m_graphRFPh->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphRFPh->setAdaptiveSampling(false);
        m_graphRFPh->setAntialiased(true);
        m_graphRFPh->setVisible(m_curveVisibility.value(2, true));
    }
    // ADC Phase (same rect as RF Phase: m_pRfADCPhaseRect)
    // PERF NOTE: Must use lsLine (not scatter ssDisc). QCustomPlot renders scatter dots
    // individually (per-point QPainter::drawEllipse), while line segments are batched into
    // a single QPainterPath — the difference is ~10x. Scatter caused severe UI lag on
    // mouse move because every replot() had to re-render thousands of individual circles.
    // NaN breaks in the data (inserted by getAdcPhaseViewport) prevent lines from connecting
    // separate ADC blocks. MATLAB SeqPlot.m uses 'b.' MarkerSize=1 but that is acceptable
    // in MATLAB's retained-mode renderer; QCustomPlot is immediate-mode and much slower.
    m_graphADCPh = customPlot->addGraph(m_pRfADCPhaseRect->axis(QCPAxis::atBottom), m_pRfADCPhaseRect->axis(QCPAxis::atLeft));
    if (m_graphADCPh)
    {
        QPen adcPhPen(Qt::blue);
        adcPhPen.setWidthF(1.0);
        m_graphADCPh->setPen(adcPhPen);
        m_graphADCPh->setLineStyle(QCPGraph::lsLine);
        m_graphADCPh->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphADCPh->setAntialiased(false);
        m_graphADCPh->setAdaptiveSampling(true);
        m_graphADCPh->setVisible(m_curveVisibility.value(2, true));
    }
    // Gradients Gx/Gy/Gz (rects 3..5)
    m_graphGx = customPlot->addGraph(m_pGxRect->axis(QCPAxis::atBottom), m_pGxRect->axis(QCPAxis::atLeft));
    if (m_graphGx)
    {
        QPen pen(colors.isEmpty() ? Qt::red : colors[2 % colors.size()]);
        pen.setWidthF(1.5);
        m_graphGx->setPen(pen);
        m_graphGx->setLineStyle(QCPGraph::lsLine);
        m_graphGx->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphGx->setAdaptiveSampling(false);
        m_graphGx->setAntialiased(true);
        m_graphGx->setVisible(m_curveVisibility.value(3, true));
    }
    m_graphGy = customPlot->addGraph(m_pGyRect->axis(QCPAxis::atBottom), m_pGyRect->axis(QCPAxis::atLeft));
    if (m_graphGy)
    {
        QPen pen(colors.isEmpty() ? Qt::darkYellow : colors[3 % colors.size()]);
        pen.setWidthF(1.5);
        m_graphGy->setPen(pen);
        m_graphGy->setLineStyle(QCPGraph::lsLine);
        m_graphGy->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphGy->setAdaptiveSampling(false);
        m_graphGy->setAntialiased(true);
        m_graphGy->setVisible(m_curveVisibility.value(4, true));
    }
    m_graphGz = customPlot->addGraph(m_pGzRect->axis(QCPAxis::atBottom), m_pGzRect->axis(QCPAxis::atLeft));
    if (m_graphGz)
    {
        QPen pen(colors.isEmpty() ? Qt::darkCyan : colors[4 % colors.size()]);
        pen.setWidthF(1.5);
        m_graphGz->setPen(pen);
        m_graphGz->setLineStyle(QCPGraph::lsLine);
        m_graphGz->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphGz->setAdaptiveSampling(false);
        m_graphGz->setAntialiased(true);
        m_graphGz->setVisible(m_curveVisibility.value(5, true));
    }

    // Persistent block-edge graphs for each rect
    m_blockEdgeGraphs.resize(m_vecRects.size());
    for (int i = 0; i < m_vecRects.size(); ++i)
    {
        m_blockEdgeGraphs[i] = customPlot->addGraph(m_vecRects[i]->axis(QCPAxis::atBottom), m_vecRects[i]->axis(QCPAxis::atLeft));
        QPen pen(Qt::black);
        pen.setWidthF(1.0);
        pen.setStyle(Qt::DashLine);
        m_blockEdgeGraphs[i]->setPen(pen);
        m_blockEdgeGraphs[i]->setLineStyle(QCPGraph::lsLine);
        m_blockEdgeGraphs[i]->setScatterStyle(QCPScatterStyle::ssNone);
        m_blockEdgeGraphs[i]->setAdaptiveSampling(false);
        m_blockEdgeGraphs[i]->setAntialiased(false);
        m_blockEdgeGraphs[i]->setVisible(bShowBlocksEdges);
    }

    // Trigger overlay on ADC/labels rect
    m_graphTrigMarkers = customPlot->addGraph(m_pADCLabelsRect->axis(QCPAxis::atBottom), m_pADCLabelsRect->axis(QCPAxis::atLeft));
    if (m_graphTrigMarkers)
    {
        QPen pen(QColor(0, 90, 200, 220)); // blue
        pen.setWidthF(1.2);
        m_graphTrigMarkers->setPen(pen);
        m_graphTrigMarkers->setLineStyle(QCPGraph::lsNone);
        m_graphTrigMarkers->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssTriangle, 7));
        m_graphTrigMarkers->setAdaptiveSampling(false);
        m_graphTrigMarkers->setAntialiased(false);
        m_graphTrigMarkers->setVisible(true);
    }
    m_graphTrigDurations = customPlot->addGraph(m_pADCLabelsRect->axis(QCPAxis::atBottom), m_pADCLabelsRect->axis(QCPAxis::atLeft));
    if (m_graphTrigDurations)
    {
        QPen pen(QColor(0, 150, 0, 220)); // green
        pen.setWidthF(1.2);
        m_graphTrigDurations->setPen(pen);
        m_graphTrigDurations->setLineStyle(QCPGraph::lsLine);
        m_graphTrigDurations->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
        m_graphTrigDurations->setAdaptiveSampling(false);
        m_graphTrigDurations->setAntialiased(false);
        m_graphTrigDurations->setVisible(true);
    }

    // Extension label overlay (ADC/labels rect)
    if (!m_extensionPlotter)
        m_extensionPlotter = std::make_unique<ExtensionPlotter>(customPlot, m_pADCLabelsRect);
    else
    {
        // Graphs were cleared above; drop cached graph pointers and recreate lazily.
        m_extensionPlotter->setTarget(customPlot, m_pADCLabelsRect);
        m_extensionPlotter->reset();
    }
}

void WaveformDrawer::setAxesOrder(const QStringList& order)
{
    if (order.size() != m_vecRects.size()) return;
    m_axesOrder = order;
    // Rebuild grid according to new order
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    // Remove existing elements and re-add in order
    // Take rects and re-add in the new order
    for (int i = 0; i < m_vecRects.size(); ++i)
    {
        customPlot->plotLayout()->take(m_vecRects[i]);
    }
    // Map labels to rects
    QMap<QString, QCPAxisRect*> labelToRect;
    labelToRect["ADC/labels"] = m_pADCLabelsRect;
    labelToRect["RF mag"] = m_pRfMagRect;
    labelToRect["RF/ADC ph"] = m_pRfADCPhaseRect;
    labelToRect["GX"] = m_pGxRect;
    labelToRect["GY"] = m_pGyRect;
    labelToRect["GZ"] = m_pGzRect;
    for (int row = 0; row < m_axesOrder.size(); ++row)
    {
        // single column rect grid
        QCPAxisRect* rect = labelToRect.value(m_axesOrder[row], nullptr);
        if (rect) customPlot->plotLayout()->addElement(row, 0, rect);
    }
    updateCurveVisibility();
    // Reconfigure x-axis labels after reordering
    configureXAxisLabelsAfterReorder();
}

void WaveformDrawer::configureXAxisLabels()
{
    // Configure x-axis labels based on current layout order
    // Only show x-axis labels on the bottom-most *visible* axis (last visible row).
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    int rowCount = customPlot->plotLayout()->rowCount();
    
    qDebug() << "[X-AXIS] configureXAxisLabels called, rowCount:" << rowCount;
    
    if (rowCount == 0) {
        qDebug() << "[X-AXIS] ERROR: No rows in layout!";
        return;
    }
    
    // Find bottom-most visible row. If all are hidden, fall back to last row.
    int bottomVisibleRow = -1;
    for (int row = rowCount - 1; row >= 0; --row)
    {
        QCPLayoutElement* el = customPlot->plotLayout()->element(row, 0);
        QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
        if (rect && rect->visible())
        {
            bottomVisibleRow = row;
            break;
        }
    }
    if (bottomVisibleRow < 0)
        bottomVisibleRow = rowCount - 1;

    for (int row = 0; row < rowCount; ++row)
    {
        QCPLayoutElement* el = customPlot->plotLayout()->element(row, 0);
        QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
        if (!rect) {
            qDebug() << "[X-AXIS] ERROR: No rect at row" << row;
            continue;
        }
        
        // Reset margins to a compact default first; bottom visible row will override.
        rect->setMargins(QMargins(0, 0, 0, 2));

        if (row != bottomVisibleRow) {
            // Hide x-axis labels for all subplots except the last one
            rect->axis(QCPAxis::atBottom)->setTickLabels(false);
            rect->axis(QCPAxis::atBottom)->setLabel("");
            qDebug() << "[X-AXIS] Hiding x-axis labels at row:" << row;
        } else {
            // Only show x-axis label on the bottom subplot (last in layout)
            rect->axis(QCPAxis::atBottom)->setTickLabels(true);
            rect->axis(QCPAxis::atBottom)->setLabel(currentTimeAxisLabel());
            applyTimeAxisFormatting(rect->axis(QCPAxis::atBottom));
            // Increase bottom margin for the bottom axis to ensure x-axis labels are visible
            rect->setMargins(QMargins(0, 0, 0, 50));
            qDebug() << "[X-AXIS] Setting x-axis label on bottom axis at row:" << row;
        }
    }
}

void WaveformDrawer::configureXAxisLabelsAfterReorder()
{
    // Keep a single source of truth: behavior is the same as configureXAxisLabels()
    configureXAxisLabels();
}

QString WaveformDrawer::currentTimeAxisLabel() const
{
    QString unit = Settings::getInstance().getTimeUnitString();
    if (unit.isEmpty() && m_mainWindow) {
        if (PulseqLoader* loader = m_mainWindow->getPulseqLoader()) {
            unit = loader->getTimeUnits();
        }
    }
    if (unit.isEmpty()) unit = "ms";
    return QStringLiteral("Time (%1)").arg(unit);
}

void WaveformDrawer::applyTimeAxisFormatting(QCPAxis* axis) const
{
    if (!axis) return;
    const bool useMicro = Settings::getInstance().getTimeUnit() == Settings::TimeUnit::Microseconds;
    axis->setNumberFormat("f"); // force fixed notation, no scientific
    axis->setNumberPrecision(useMicro ? 0 : 3);
}

int WaveformDrawer::axisIndexAtPositionY(int yInPlot) const
{
    // Map Y in plot widget coordinates to rect layout row index
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    int rowCount = customPlot->plotLayout()->rowCount();
    for (int row = 0; row < rowCount; ++row)
    {
        QCPLayoutElement* el = customPlot->plotLayout()->element(row, 0);
        QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
        if (!rect) continue;
        const QRect rOuter = rect->outerRect();
        if (yInPlot >= rOuter.top() && yInPlot <= rOuter.bottom()) return row;
    }
    return -1;
}

int WaveformDrawer::axisCenterY(int index) const
{
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    int rowCount = customPlot->plotLayout()->rowCount();
    if (index < 0 || index >= rowCount) return -1;
    QCPLayoutElement* el = customPlot->plotLayout()->element(index, 0);
    QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
    if (!rect) return -1;
    QRect rOuter = rect->outerRect();
    return rOuter.center().y();
}

void WaveformDrawer::swapAxes(int i, int j)
{
    if (i < 0 || j < 0 || i >= m_axesOrder.size() || j >= m_axesOrder.size() || i == j) return;
    m_axesOrder.swapItemsAt(i, j);
    setAxesOrder(m_axesOrder);
}

void WaveformDrawer::moveAxis(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || toIndex < 0 || fromIndex >= m_axesOrder.size() || toIndex >= m_axesOrder.size()) return;
    if (fromIndex == toIndex) return;
    QString item = m_axesOrder.takeAt(fromIndex);
    // Insert-before semantics: when dragging to a target row, insert at that row index
    m_axesOrder.insert(toIndex, item);
    setAxesOrder(m_axesOrder);
    saveUiConfig();
}

void WaveformDrawer::showDropIndicatorAt(int index)
{
    m_dropIndicatorIndex = index;
    // Highlight by layout row index to avoid mismatch after reordering
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    if (!plot || !plot->plotLayout()) return;
    int rows = plot->plotLayout()->rowCount();
    if (index < 0 || index >= rows) return;
    for (int r = 0; r < rows; ++r)
    {
        QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(plot->plotLayout()->element(r, 0));
        if (!rect) continue;
        QBrush bg = (r == index) ? QBrush(QColor(235, 242, 255)) : QBrush(Qt::NoBrush);
        rect->setBackground(bg);
    }
    m_mainWindow->ui->customPlot->replot();
}

void WaveformDrawer::clearDropIndicator()
{
    m_dropIndicatorIndex = -1;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    if (plot && plot->plotLayout())
    {
        int rows = plot->plotLayout()->rowCount();
        for (int r = 0; r < rows; ++r)
        {
            QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(plot->plotLayout()->element(r, 0));
            if (rect) rect->setBackground(QBrush(Qt::NoBrush));
        }
    }
    m_mainWindow->ui->customPlot->replot();
}

QString WaveformDrawer::defaultLabelForRect(int layoutRowIndex) const
{
    if (layoutRowIndex >= 0 && layoutRowIndex < m_axesOrder.size())
    {
        return m_axesOrder[layoutRowIndex];
    }
    return "";
}

void WaveformDrawer::startAxisDragVisual(int sourceIndex, const QPoint& startPos)
{
    if (m_dragGhost)
    {
        finishAxisDragVisual();
    }
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    m_dragGhost = new QCPItemText(plot);
    m_dragGhost->position->setType(QCPItemPosition::ptAbsolute);
    m_dragGhost->position->setCoords(10, startPos.y());
    m_dragGhost->setColor(QColor(50, 50, 50));
    QFont f = plot->font(); f.setBold(true);
    m_dragGhost->setFont(f);
    m_dragGhost->setBrush(QBrush(QColor(255, 255, 255, 220)));
    m_dragGhost->setPen(QPen(QColor(100, 100, 255)));
    m_dragGhost->setText(defaultLabelForRect(sourceIndex));
    m_dragGhost->setVisible(true);
    plot->replot(QCustomPlot::rpQueuedReplot);
}

void WaveformDrawer::updateAxisDragVisual(int yInPlot)
{
    if (!m_dragGhost) return;
    m_dragGhost->position->setCoords(10, yInPlot);
    m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
}

void WaveformDrawer::finishAxisDragVisual()
{
    if (!m_dragGhost) return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    plot->removeItem(m_dragGhost);
    m_dragGhost = nullptr;
    plot->replot(QCustomPlot::rpQueuedReplot);
}

void WaveformDrawer::rescaleTimeCachedState(double ratio)
{
    // Rescale initial viewport bounds
    m_initialViewportLower *= ratio;
    m_initialViewportUpper *= ratio;

    // Rescale cached viewport state used by debounce / viewport-change detection
    m_lastViewportLower *= ratio;
    m_lastViewportUpper *= ratio;
    m_pendingViewportStart *= ratio;
    m_pendingViewportEnd *= ratio;

    // Rescale all x-axis ranges so the same physical time span stays visible
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    if (!customPlot) return;
    for (auto& rect : m_vecRects)
    {
        if (!rect) continue;
        QCPRange r = rect->axis(QCPAxis::atBottom)->range();
        rect->axis(QCPAxis::atBottom)->setRange(r.lower * ratio, r.upper * ratio);
    }
}

void WaveformDrawer::loadUiConfig()
{
    QString path = Settings::getInstance().getConfigDirPath() + "/ui_config.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        // Initialize with default order and persist a default ui_config.json
        setAxesOrder(QStringList() << "RF mag" << "GZ" << "GY" << "GX" << "RF/ADC ph" << "ADC/labels");
        saveUiConfig();
        return;
    }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    auto obj = doc.object();
    if (obj.contains("axes_order") && obj.value("axes_order").isArray())
    {
        QStringList order;
        for (auto v : obj.value("axes_order").toArray()) order << v.toString();
        if (order.size() == m_vecRects.size()) setAxesOrder(order);
    }
}

void WaveformDrawer::saveUiConfig() const
{
    QString path = Settings::getInstance().getConfigDirPath() + "/ui_config.json";
    QJsonObject obj;
    QJsonArray arr;
    for (const auto& s : m_axesOrder) arr.append(s);
    obj["axes_order"] = arr;
    QJsonDocument doc(obj);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
    {
        f.write(doc.toJson(QJsonDocument::Indented));
        f.close();
    }
}

void WaveformDrawer::InitTracers()
{
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    m_vecVerticalLine.resize(6);
    for (int i = 0; i < 6; i++)
    {
        auto verticalLine = new QCPItemStraightLine(customPlot);
        verticalLine->setVisible(false);
        QPen pen(Qt::red);
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(0.5);
        verticalLine->setPen(pen);
        verticalLine->point1->setType(QCPItemPosition::ptPlotCoords);
        verticalLine->point2->setType(QCPItemPosition::ptPlotCoords);
        verticalLine->setClipToAxisRect(true);
        m_vecVerticalLine[i] = verticalLine;
    }

    // Re-assign vertical lines to current layout order
    if (customPlot && customPlot->plotLayout())
    {
        int rowCount = customPlot->plotLayout()->rowCount();
        for (int row = 0; row < rowCount && row < m_vecVerticalLine.size(); ++row)
        {
            QCPLayoutElement* el = customPlot->plotLayout()->element(row, 0);
            QCPAxisRect* rect = qobject_cast<QCPAxisRect*>(el);
            if (rect)
            {
                m_vecVerticalLine[row]->setClipAxisRect(rect);
                m_vecVerticalLine[row]->point1->setAxisRect(rect);
                m_vecVerticalLine[row]->point2->setAxisRect(rect);
                m_vecVerticalLine[row]->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                m_vecVerticalLine[row]->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            }
        }
    }
    ensureTeGuideCapacity();
    ensureKxKyZeroGuideCapacity();
}


void WaveformDrawer::ResetView()
{
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    if (!customPlot) return;

    // Determine initial view range based on current render mode
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    TRManager* trManager = m_mainWindow->getTRManager();

    double initialStartTime, initialEndTime;

    if (trManager && loader->hasRepetitionTime() && trManager->isTrBasedMode())
    {
        // TR-Segmented mode: show first TR
        double trDuration_us = loader->getRepetitionTime_us();
        initialStartTime = 0;
        initialEndTime = trDuration_us * loader->getTFactor();

        // Ensure valid range for TR-based mode
        if (initialStartTime < 0) initialStartTime = 0;
        if (initialEndTime <= initialStartTime) initialEndTime = initialStartTime + 1.0;
    }
    else
    {
        // Whole-Sequence mode: show entire sequence (ensure non-negative)
        if (m_initialViewSaved)
        {
            // Use saved initial range (already validated to be non-negative)
            initialStartTime = m_initialViewportLower;
            initialEndTime = m_initialViewportUpper;
        }
        else
        {
            // Calculate entire sequence range - ensure non-negative start
            const auto& edges = loader->getBlockEdges();
            if (!edges.isEmpty())
            {
                initialStartTime = std::max(edges[0], 0.0);  // Never negative
                initialEndTime = edges.last();  // Show entire sequence
            }
            else
            {
                initialStartTime = 0;
                initialEndTime = loader->getTotalDuration_us() * loader->getTFactor();
            }
        }

        // Ensure valid range
        if (initialEndTime <= initialStartTime) initialEndTime = initialStartTime + 1.0;
        double totalDuration = loader->getTotalDuration_us() * loader->getTFactor();
        if (totalDuration > 0)
        {
            // Ensure end time doesn't exceed total duration
            if (initialEndTime > totalDuration) initialEndTime = totalDuration;
            // Ensure start time doesn't exceed total duration
            if (initialStartTime > totalDuration) initialStartTime = totalDuration;
        }
    }

    // Final validation before setting ranges
    if (initialEndTime <= initialStartTime) initialEndTime = initialStartTime + 1.0;

    // Ensure the range doesn't exceed the total sequence duration
    double totalDuration = loader->getTotalDuration_us() * loader->getTFactor();
    if (totalDuration > 0 && initialEndTime > totalDuration)
    {
        initialEndTime = totalDuration;
    }

    // Set X-axis range for all rects
    for (auto& rect : m_vecRects)
    {
        rect->axis(QCPAxis::atBottom)->setRange(initialStartTime, initialEndTime);
    }

    // Redraw all waveforms with the validated range
    DrawRFWaveform(initialStartTime, initialEndTime);
    DrawADCWaveform(initialStartTime, initialEndTime);
    DrawGWaveform(initialStartTime, initialEndTime);

    // Update curve visibility to ensure proper layout
    updateCurveVisibility();

    // Replot to apply changes
    customPlot->replot();
}

void WaveformDrawer::DrawRFWaveform(const double& dStartTime, double dEndTime)
{
    static int callCount = 0;
    callCount++;
    // Debug logging removed
    
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getDecodedSeqBlocks().empty()) return;

    // Validate input parameters - ensure non-negative time coordinates
    double safeStartTime = dStartTime;
    double safeEndTime = dEndTime;

    // Ensure non-negative time coordinates regardless of mode
    if (safeStartTime < 0) safeStartTime = 0;
    if (safeEndTime <= safeStartTime) safeEndTime = safeStartTime + 1.0;

    // Determine visible viewport in internal time units
    if (m_vecRects.isEmpty() || !m_vecRects[0]) {
        qDebug() << "ERROR: m_vecRects not initialized!";
        return;
    }
    QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
    double visibleStart = viewport.lower;
    double visibleEnd = viewport.upper;
    
    // Debug logging removed

    // Ensure visible range is also valid
    if (visibleStart < 0) visibleStart = 0;
    if (visibleEnd <= visibleStart) visibleEnd = visibleStart + 1.0;

    // Ensure visible range doesn't exceed sequence bounds
    double totalDuration = loader->getTotalDuration_us() * loader->getTFactor();
    if (totalDuration > 0)
    {
        if (visibleEnd > totalDuration) visibleEnd = totalDuration;
        if (visibleStart > totalDuration) visibleStart = totalDuration;
    }

    // Clamp viewport to TR bounds if TR-Segmented mode is active
    TRManager* trm = m_mainWindow->getTRManager();
    if (trm && m_mainWindow->getPulseqLoader()->hasRepetitionTime() && trm->isTrBasedMode())
    {
        int startTr = trm->getTrStartInput()->text().toInt();
        int endTr = trm->getTrEndInput()->text().toInt();
        double trStart = (startTr - 1) * loader->getRepetitionTime_us() * loader->getTFactor();
        double trEnd = endTr * loader->getRepetitionTime_us() * loader->getTFactor();
        // In TR range mode, intersect with TR bounds instead of expanding to full TR.
        // This preserves deep zoom behavior for accurate rendering/detail.
        visibleStart = std::max(visibleStart, trStart);
        visibleEnd = std::min(visibleEnd, trEnd);
        if (visibleEnd <= visibleStart) {
            // Fallback to a minimal positive window within TR to avoid empty draw
            visibleStart = trStart;
            visibleEnd = std::max(trStart + 1e-6, trEnd);
        }
    }
    updateTeGuides(visibleStart, visibleEnd);
    updateKxKyZeroGuides(visibleStart, visibleEnd);

    // Use simple LOD system
    LODLevel currentLODLevel = getCurrentLODLevel();

    // Fast path: RF on-demand viewport rendering via shape cache
    {
        int pxRF = 0;
        if (m_vecRects.size() > 1 && m_vecRects[1])
            pxRF = qMax(1, static_cast<int>(qRound(m_vecRects[1]->width() * m_mainWindow->devicePixelRatioF())));
        // If LOD is FULL_DETAIL, force an effectively huge pixel width to disable decimation in loader
        int pxRFEffective = (currentLODLevel == LODLevel::DOWNSAMPLED ? pxRF : qMax(pxRF, 100000));

        QVector<double> tAmp, vAmp, tPh, vPh;
        loader->getRfViewportDecimated(visibleStart, visibleEnd, pxRFEffective, tAmp, vAmp, tPh, vPh);
        if (m_graphRFMag) { m_graphRFMag->setData(tAmp, vAmp); m_graphRFMag->setVisible(m_curveVisibility.value(1, true)); }
        if (m_graphRFPh)  { m_graphRFPh->setData(tPh, vPh);   m_graphRFPh->setVisible(m_curveVisibility.value(2, true)); }

        // Added: ADC Phase (pixel-aware decimation like RF)
        QVector<double> tAdcPh, vAdcPh;
        int pxADCPh = pxRFEffective; // reuse RF effective pixel width for phase rect
        if (m_vecRects.size() > 2 && m_vecRects[2])
            pxADCPh = qMax(1, static_cast<int>(qRound(m_vecRects[2]->width() * m_mainWindow->devicePixelRatioF())));
        if (currentLODLevel != LODLevel::DOWNSAMPLED)
            pxADCPh = qMax(pxADCPh, 100000); // full detail mode
        loader->getAdcPhaseViewport(visibleStart, visibleEnd, pxADCPh, tAdcPh, vAdcPh);
        if (m_graphADCPh) {
             m_graphADCPh->setData(tAdcPh, vAdcPh);
             m_graphADCPh->setVisible(m_curveVisibility.value(2, true)); // controlled by RF Phase visibility checkbox
        }

        if (!m_lockYAxisRanges)
        {
            auto upd = [](const QVector<double>& arr, double& mn, double& mx){ for (double v: arr){ if (std::isnan(v)) continue; if (v<mn) mn=v; if (v>mx) mx=v; } };
            double minMag = std::numeric_limits<double>::max();
            double maxMag = -std::numeric_limits<double>::infinity();
            double minPh  = std::numeric_limits<double>::max();
            double maxPh  = -std::numeric_limits<double>::infinity();
            upd(vAmp, minMag, maxMag); upd(vPh, minPh, maxPh);
            upd(vAdcPh, minPh, maxPh); // Include ADC phase in range computation

            if (maxMag >= minMag && m_vecRects.size() > 1 && m_vecRects[1]){
                double pad = (maxMag - minMag) * 0.05; if (pad == 0) pad = 1.0;
                m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(minMag - pad, maxMag + pad);
            }
            if (maxPh >= minPh && m_vecRects.size() > 2 && m_vecRects[2]){
                // Force full [-pi, pi] range coverage to ensure negative values are visible
                double forceMin = -3.2; // slightly more than -pi
                double forceMax = 3.2;  // slightly more than pi
                if (minPh > forceMin) minPh = forceMin;
                if (maxPh < forceMax) maxPh = forceMax;
                
                double pad = (maxPh - minPh) * 0.05; if (pad == 0) pad = 1.0;
                m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(minPh - pad, maxPh + pad);
            }
        } else {
            if (m_vecRects.size() > 1 && m_vecRects[1]) m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[1].first, m_fixedYRanges[1].second);
            if (m_vecRects.size() > 2 && m_vecRects[2]) m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[2].first, m_fixedYRanges[2].second);
        }
        
        // DEBUG: Unconditionally force phase Y-axis range to [-3.5, 3.5] to reveal negative values
        if (m_vecRects.size() > 2 && m_vecRects[2]) {
             m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(-3.5, 3.5);
        }
        return;
    }

    // Visible block range
    const auto& edges = loader->getBlockEdges();
    int startBlock = loader->getBlockRangeStart();
    int endBlock = loader->getBlockRangeEnd();
    for (int i = startBlock; i <= endBlock; ++i)
    {
        if (i + 1 < edges.size() && edges[i + 1] > visibleStart) { startBlock = i; break; }
    }
    for (int i = endBlock; i >= startBlock; --i)
    {
        if (edges[i] < visibleEnd) { endBlock = i; break; }
    }
    if (startBlock > endBlock) return;

    if (startBlock >= edges.size() || startBlock < 0) {
        qDebug() << "ERROR: startBlock out of bounds - startBlock:" << startBlock << "edges.size():" << edges.size();
        return;
    }
    double t0 = edges[startBlock];
    int cnt = 0;
    double min1 = std::numeric_limits<double>::max(), max1 = std::numeric_limits<double>::min();
    double min2 = std::numeric_limits<double>::max(), max2 = std::numeric_limits<double>::min();
    double tFactor = loader->getTFactor();

    // Prefer merged RF series built at load-time
    const QVector<double>& mergedTimeAmp = loader->getRfTimeAmp();
    const QVector<double>& mergedAmp = loader->getRfAmp();

    // Debug output for RF data
    // Debug logging removed
    if (!mergedTimeAmp.isEmpty()) {
        // Debug logging removed
    }
    // Debug logging removed

    if (!mergedTimeAmp.isEmpty() && !mergedAmp.isEmpty()) {
        // Slice by viewport with small margin
        const double margin = (visibleEnd - visibleStart) * 0.02;
        const double x0 = visibleStart - margin;
        const double x1 = visibleEnd + margin;

        // Compute viewport-aware target points for RF Mag
        int pxRF = 0;
        if (m_vecRects.size() > 1 && m_vecRects[1]) {
            pxRF = qMax(1, static_cast<int>(qRound(m_vecRects[1]->width() * m_mainWindow->devicePixelRatioF())));
        }
        const double noDecimThresholdRF = 1.2; // pts/px -> draw 1:1 if below
        const int smallSegThresholdRF = 64;     // small segments -> pass-through
        const double lttbFactorRF = 2.0;        // ~2 samples per pixel
        const int capPointsRF = 10000;          // safety cap

        auto sliceAndProcess = [&](const QVector<double>& tIn, const QVector<double>& vIn, QVector<double>& tOut, QVector<double>& vOut) {
            tOut.clear(); vOut.clear();
            QVector<double> segT, segV;
            segT.reserve(1024); segV.reserve(1024);
            double minDt = std::numeric_limits<double>::infinity();
            auto flushSegment = [&]() {
                if (segT.isEmpty()) return;
                if (currentLODLevel == LODLevel::DOWNSAMPLED) {
                    QVector<double> dsT, dsV;
                    int n = segT.size();
                    if (pxRF <= 0) { dsT = segT; dsV = segV; }
                    else {
                        double ppp = static_cast<double>(n) / static_cast<double>(pxRF);
                        if (n <= smallSegThresholdRF || ppp <= noDecimThresholdRF) { dsT = segT; dsV = segV; }
                        else {
                            // Hybrid: very large ppp uses fast min-max; otherwise LTTB
                            if (ppp >= 8.0) {
                                applyMinMaxDownsampling(segT, segV, pxRF, dsT, dsV);
                            } else {
                                int target = qMin(static_cast<int>(qRound(pxRF * lttbFactorRF)), capPointsRF);
                                target = qMin(target, n);
                                applyLTTBDownsampling(segT, segV, target, dsT, dsV);
                            }
                        }
                    }
                    // Debug logging removed
                    tOut += dsT; vOut += dsV;
                } else {
                    tOut += segT; vOut += segV;
                }
                segT.clear(); segV.clear();
                minDt = std::numeric_limits<double>::infinity();
                // keep NaN separator between segments (values only)
                if (!tOut.isEmpty()) {
                    // duplicate last valid time to maintain monotonic keys, mark value as NaN to break line
                    tOut.append(tOut.last());
                    vOut.append(std::numeric_limits<double>::quiet_NaN());
                }
            };

            // Restrict to indices within [x0, x1] using binary search to avoid O(n_total) scans
            int iStart = 0;
            int iEnd = tIn.size();
            if (!tIn.isEmpty()) {
                auto itL = std::lower_bound(tIn.begin(), tIn.end(), x0);
                auto itU = std::upper_bound(tIn.begin(), tIn.end(), x1);
                iStart = std::distance(tIn.begin(), itL);
                iEnd = std::distance(tIn.begin(), itU);
                if (iStart > 0) --iStart; // include margin just before
                if (iEnd < tIn.size()) ++iEnd; // include margin just after
            }
            for (int i = iStart; i < iEnd; ++i) {
                const double tx = tIn[i];
                const double vy = (i < vIn.size() ? vIn[i] : std::numeric_limits<double>::quiet_NaN());
                const bool isBreak = std::isnan(vy);
                if (isBreak) { flushSegment(); continue; }
                if (!segT.isEmpty()) {
                    double dt = tx - segT.last();
                    if (dt > 0) {
                        if (!std::isfinite(minDt)) minDt = dt; else minDt = std::min(minDt, dt);
                        // Heuristic: if current gap is much larger than local sampling, start a new segment
                        if (minDt < std::numeric_limits<double>::infinity() && dt > 10.0 * minDt) {
                            flushSegment();
                        }
                    }
                }
                segT.append(tx); segV.append(vy);
            }
            flushSegment();
            // remove trailing NaN in values (time stays finite)
            if (!vOut.isEmpty() && std::isnan(vOut.last())) { tOut.removeLast(); vOut.removeLast(); }

            // Fallback: if nothing in slice, pick nearest segment around viewport center
            if (tOut.isEmpty()) {
                double xc = 0.5 * (visibleStart + visibleEnd);
                // find nearest index by linear scan (acceptable once at draw time)
                int best = -1; double bestDist = std::numeric_limits<double>::infinity();
                for (int i = 0; i < tIn.size(); ++i) {
                    const double tx = tIn[i];
                    if (i >= vIn.size() || std::isnan(vIn[i])) continue;
                    double d = std::abs(tx - xc);
                    if (d < bestDist) { bestDist = d; best = i; }
                }
                if (best != -1) {
                    // expand to contiguous non-NaN segment around best
                    int L = best, R = best;
                    while (L > 0 && !std::isnan(vIn[L-1])) L--;
                    while (R+1 < tIn.size() && !std::isnan(vIn[R+1])) R++;
                    segT.clear(); segV.clear();
                    for (int i = L; i <= R; ++i) { segT.append(tIn[i]); segV.append(vIn[i]); }
                    if (!segT.isEmpty()) {
                        if (currentLODLevel == LODLevel::DOWNSAMPLED) {
                            QVector<double> dsT, dsV; applyLTTBDownsampling(segT, segV, 1000, dsT, dsV); tOut = dsT; vOut = dsV;
    } else {
                            tOut = segT; vOut = segV;
                        }
                    }
                }
            }
        };

        QVector<double> tAmp, vAmp;
        sliceAndProcess(mergedTimeAmp, mergedAmp, tAmp, vAmp);
        
        // Prepare pen for RF Mag if needed (kept once at graph creation)

        if (m_graphRFMag)
        {
            m_graphRFMag->setData(tAmp, vAmp);
            m_graphRFMag->setVisible(m_curveVisibility.value(1, true));
        }
        
        // Draw RF Phase if visible
        if (m_curveVisibility[2] && m_vecRects.size() > 2 && m_vecRects[2]) {
            const QVector<double>& mergedTimePh = loader->getRfTimePh();
            const QVector<double>& mergedPh = loader->getRfPh();
            
            if (!mergedTimePh.isEmpty() && !mergedPh.isEmpty()) {
                QVector<double> tPh, vPh;
                
                // LTTB viewport-aware target for RF Phase
                int pxPh = 0; if (m_vecRects.size()>2 && m_vecRects[2]) { pxPh = qMax(1, static_cast<int>(qRound(m_vecRects[2]->width() * m_mainWindow->devicePixelRatioF()))); }
                const double noDecimThresholdPh = 1.2; const int smallSegThresholdPh = 64; const double lttbFactorPh = 2.0; const int capPointsPh = 10000;
                // Slice merged RF Phase data based on viewport
                auto itLowerPh = std::lower_bound(mergedTimePh.begin(), mergedTimePh.end(), visibleStart);
                auto itUpperPh = std::upper_bound(mergedTimePh.begin(), mergedTimePh.end(), visibleEnd);
                
                int idxStartPh = std::distance(mergedTimePh.begin(), itLowerPh);
                int idxEndPh = std::distance(mergedTimePh.begin(), itUpperPh);
                
                // Add margin for continuity
                if (idxStartPh > 0) idxStartPh--;
                if (idxEndPh < mergedTimePh.size()) idxEndPh++;
                
                if (idxStartPh < idxEndPh) {
                    // Extract visible data
                    for (int k = idxStartPh; k < idxEndPh; ++k) {
                        tPh.append(mergedTimePh[k]);
                        vPh.append(mergedPh[k]);
                    }
                    
                    // Process segments separated by NaN
                    QVector<double> processedTimePh, processedValuesPh;
                    int segmentStartIdx = 0;
                    for (int k = 0; k < vPh.size(); ++k) {
                        if (std::isnan(vPh[k])) {
                            if (k > segmentStartIdx) {
                                QVector<double> currentSegmentTime;
                                QVector<double> currentSegmentValues;
                                for (int m = segmentStartIdx; m < k; ++m) {
                                    currentSegmentTime.append(tPh[m]);
                                    currentSegmentValues.append(vPh[m]);
                                }
                                
                                if (currentLODLevel == LODLevel::DOWNSAMPLED) {
                                    QVector<double> downsampledSegmentTime, downsampledSegmentValues;
                                    int n=currentSegmentTime.size();
                            if (pxPh <= 0) { downsampledSegmentTime=currentSegmentTime; downsampledSegmentValues=currentSegmentValues; }
                            else {
                                double ppp = static_cast<double>(n) / static_cast<double>(pxPh);
                                if (n <= smallSegThresholdPh || ppp <= noDecimThresholdPh) { downsampledSegmentTime=currentSegmentTime; downsampledSegmentValues=currentSegmentValues; }
                                else {
                                    if (ppp >= 8.0) { applyMinMaxDownsampling(currentSegmentTime, currentSegmentValues, pxPh, downsampledSegmentTime, downsampledSegmentValues); }
                                    else { int target = qMin(static_cast<int>(qRound(pxPh * lttbFactorPh)), capPointsPh); target = qMin(target, n); applyLTTBDownsampling(currentSegmentTime, currentSegmentValues, target, downsampledSegmentTime, downsampledSegmentValues); }
                                }
                            }
                                    processedTimePh.append(downsampledSegmentTime);
                                    processedValuesPh.append(downsampledSegmentValues);
                                } else {
                                    processedTimePh.append(currentSegmentTime);
                                    processedValuesPh.append(currentSegmentValues);
                                }
                                // Add NaN to separate segments
                                processedTimePh.append(tPh[k]);
                                processedValuesPh.append(std::numeric_limits<double>::quiet_NaN());
                            }
                            segmentStartIdx = k + 1;
                        }
                    }
                    // Process the last segment
                    if (vPh.size() > segmentStartIdx) {
                        QVector<double> currentSegmentTime;
                        QVector<double> currentSegmentValues;
                        for (int m = segmentStartIdx; m < vPh.size(); ++m) {
                            currentSegmentTime.append(tPh[m]);
                            currentSegmentValues.append(vPh[m]);
                        }
                        if (currentLODLevel == LODLevel::DOWNSAMPLED) {
                            QVector<double> downsampledSegmentTime, downsampledSegmentValues;
                            int n=currentSegmentTime.size();
                            if (pxPh <= 0) { downsampledSegmentTime=currentSegmentTime; downsampledSegmentValues=currentSegmentValues; }
                            else {
                                double ppp = static_cast<double>(n) / static_cast<double>(pxPh);
                                if (n <= smallSegThresholdPh || ppp <= noDecimThresholdPh) { downsampledSegmentTime=currentSegmentTime; downsampledSegmentValues=currentSegmentValues; }
                                else {
                                    if (ppp >= 8.0) { applyMinMaxDownsampling(currentSegmentTime, currentSegmentValues, pxPh, downsampledSegmentTime, downsampledSegmentValues); }
                                    else { int target = qMin(static_cast<int>(qRound(pxPh * lttbFactorPh)), capPointsPh); target = qMin(target, n); applyLTTBDownsampling(currentSegmentTime, currentSegmentValues, target, downsampledSegmentTime, downsampledSegmentValues); }
                                }
                            }
                            processedTimePh.append(downsampledSegmentTime);
                            processedValuesPh.append(downsampledSegmentValues);
                        } else {
                            processedTimePh.append(currentSegmentTime);
                            processedValuesPh.append(currentSegmentValues);
                        }
                    }
                    
                    // Draw RF Phase via persistent graph
                    if (!processedTimePh.isEmpty() && m_graphRFPh) {
                        m_graphRFPh->setData(processedTimePh, processedValuesPh);
                        m_graphRFPh->setVisible(m_curveVisibility.value(2, true));
                        
                        // Update Y-axis range for RF Phase
                        double minPh = std::numeric_limits<double>::max();
                        double maxPh = std::numeric_limits<double>::min();
                        for (const auto& val : processedValuesPh) {
                            if (!std::isnan(val)) {
                                minPh = std::min(minPh, val);
                                maxPh = std::max(maxPh, val);
                            }
                        }
                        if (maxPh >= minPh) {
                            double pad = (maxPh - minPh) * 0.05;
                            pad = (pad == 0) ? 1.0 : pad;
                            m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(minPh - pad, maxPh + pad);
                        }
                    }
                }
            }
        }

        auto updateMinMax = [&](const QVector<double>& arr, double& mn, double& mx) {
            for (int i = 0; i < arr.size(); ++i) {
                const double v = arr[i];
                if (std::isnan(v)) continue;
                if (v < mn) mn = v; if (v > mx) mx = v;
            }
        };
        min1 = std::min(min1, std::numeric_limits<double>::max());
        max1 = std::max(max1, std::numeric_limits<double>::min());
        min2 = std::min(min2, std::numeric_limits<double>::max());
        max2 = std::max(max2, std::numeric_limits<double>::min());
        updateMinMax(vAmp, min1, max1);
    } else {
        // Fallback: keep previous per-block path if merged series unavailable (should not happen)
        // No-op; axes range update below will keep previous values
    }

    if (!m_lockYAxisRanges) {
        if (max1 >= min1 && m_vecRects.size() > 1 && m_vecRects[1]) { double pad = (max1 - min1) * 0.05; pad = (pad == 0) ? 1.0 : pad; m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(min1 - pad, max1 + pad); }
        if (max2 >= min2 && m_vecRects.size() > 2 && m_vecRects[2]) { double pad = (max2 - min2) * 0.05; pad = (pad == 0) ? 1.0 : pad; m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(min2 - pad, max2 + pad); }
    } else {
        if (m_vecRects.size() > 1 && m_vecRects[1]) m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[1].first, m_fixedYRanges[1].second);
        if (m_vecRects.size() > 2 && m_vecRects[2]) m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[2].first, m_fixedYRanges[2].second);
    }
}

void WaveformDrawer::DrawADCWaveform(const double& dStartTime, double dEndTime)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getDecodedSeqBlocks().empty()) return;
    
    // ʹ��PulseqLabelAnalyzer����ȷ������ǩ״̬
    static PulseqLabelAnalyzer* labelAnalyzer = nullptr;
    if (!labelAnalyzer) {
        auto seq = loader->getSequence();
        if (seq) {
            labelAnalyzer = new PulseqLabelAnalyzer(*seq);
            if (DEBUG_LABEL_EVENTS) {
                qDebug().noquote() << "PulseqLabelAnalyzer initialized with" << seq->GetNumberOfBlocks() << "blocks";
            }
        }
    }

    // Determine visible viewport in internal time units
    if (m_vecRects.isEmpty() || !m_vecRects[0]) {
        qDebug() << "ERROR: m_vecRects not initialized!";
        return;
    }
    QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
    double visibleStart = viewport.lower;
    double visibleEnd = viewport.upper;

    // Clamp viewport to TR bounds if TR-Segmented mode is active
    TRManager* trm = m_mainWindow->getTRManager();
    if (trm && m_mainWindow->getPulseqLoader()->hasRepetitionTime() && trm->isTrBasedMode())
    {
        int startTr = trm->getTrStartInput()->text().toInt();
        int endTr = trm->getTrEndInput()->text().toInt();
        double trStart = (startTr - 1) * loader->getRepetitionTime_us() * loader->getTFactor();
        double trEnd = endTr * loader->getRepetitionTime_us() * loader->getTFactor();
        // Intersect with TR bounds to preserve user's zoom window
        visibleStart = std::max(visibleStart, trStart);
        visibleEnd = std::min(visibleEnd, trEnd);
        if (visibleEnd <= visibleStart) {
            visibleStart = trStart;
            visibleEnd = std::max(trStart + 1e-6, trEnd);
        }
    }

    // Use simple LOD system
    LODLevel currentLODLevel = getCurrentLODLevel();

    // Compute unified ADC rectangle height based on label range across the sequence
    double adcHeight = 1.0; // default when no label exists
    {
        double maxAbsLabel = 0.0;
        const auto& blocks = loader->getDecodedSeqBlocks();
        for (auto* blk : blocks)
        {
            if (!blk) continue;
            if (blk->isLabel())
            {
                const auto& sets = blk->GetLabelSetEvents();
                const auto& incs = blk->GetLabelIncEvents();
                for (const auto& e : sets) { maxAbsLabel = std::max(maxAbsLabel, std::abs((double)e.numVal.second)); }
                for (const auto& e : incs) { maxAbsLabel = std::max(maxAbsLabel, std::abs((double)e.numVal.second)); }
            }
        }
        // Include precomputed max accumulated counter (e.g. LIN=63 from INC events)
        maxAbsLabel = std::max(maxAbsLabel, (double)loader->getMaxAccumulatedCounter());
        if (maxAbsLabel <= 0.0) maxAbsLabel = 1.0;
        adcHeight = maxAbsLabel * 1.2;
    }

    // Use merged ADC series for drawing
    const QVector<double>& mergedAdcTime = loader->getAdcTime();
    const QVector<double>& mergedAdcValues = loader->getAdcValues();
    
    if (mergedAdcTime.isEmpty() || mergedAdcValues.isEmpty()) return;
    
    // Slice merged data based on viewport
    QVector<double> tAdc, vAdc;
    
    // Find visible range indices
    auto itLower = std::lower_bound(mergedAdcTime.begin(), mergedAdcTime.end(), visibleStart);
    auto itUpper = std::upper_bound(mergedAdcTime.begin(), mergedAdcTime.end(), visibleEnd);
    
    int idxStart = std::distance(mergedAdcTime.begin(), itLower);
    int idxEnd = std::distance(mergedAdcTime.begin(), itUpper);
    
    // Add margin for continuity
    if (idxStart > 0) idxStart--;
    if (idxEnd < mergedAdcTime.size()) idxEnd++;
    
    if (idxStart >= idxEnd) return;
    
    // Extract visible data
    for (int k = idxStart; k < idxEnd; ++k) {
        tAdc.append(mergedAdcTime[k]);
        vAdc.append(mergedAdcValues[k] * adcHeight); // Scale to proper height
    }
    
    // Process segments separated by NaN
    QVector<double> processedTime, processedValues;
    int segmentStartIdx = 0;
    for (int k = 0; k < vAdc.size(); ++k) {
        if (std::isnan(vAdc[k])) {
            if (k > segmentStartIdx) {
                QVector<double> currentSegmentTime;
                QVector<double> currentSegmentValues;
                for (int m = segmentStartIdx; m < k; ++m) {
                    currentSegmentTime.append(tAdc[m]);
                    currentSegmentValues.append(vAdc[m]);
                }
                
                // For ADC rectangles, we don't need LTTB downsampling as they are simple shapes
                processedTime.append(currentSegmentTime);
                processedValues.append(currentSegmentValues);
                
                // Add NaN to separate segments
                processedTime.append(tAdc[k]);
                processedValues.append(std::numeric_limits<double>::quiet_NaN());
            }
            segmentStartIdx = k + 1;
        }
    }
    // Process the last segment
    if (vAdc.size() > segmentStartIdx) {
        QVector<double> currentSegmentTime;
        QVector<double> currentSegmentValues;
        for (int m = segmentStartIdx; m < vAdc.size(); ++m) {
            currentSegmentTime.append(tAdc[m]);
            currentSegmentValues.append(vAdc[m]);
        }
        processedTime.append(currentSegmentTime);
        processedValues.append(currentSegmentValues);
    }
    
    // Draw ADC rectangles using persistent graph
    if (m_graphADC) {
        m_graphADC->setData(processedTime, processedValues);
        m_graphADC->setVisible(m_curveVisibility.value(0, true) && !processedTime.isEmpty());
        
        // Set ADC y-axis range to show the full rectangle height
        if (!m_lockYAxisRanges) {
            if (m_vecRects.size() > 0 && m_vecRects[0]) {
                double minVal = 0.0;
                double maxVal = adcHeight;
                double pad = maxVal * 0.1; // 10% padding
                m_vecRects[0]->axis(QCPAxis::atLeft)->setRange(minVal - pad, maxVal + pad);
            }
        } else {
            if (m_vecRects.size() > 0 && m_vecRects[0]) m_vecRects[0]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[0].first, m_fixedYRanges[0].second);
        }
    }

    // Extension labels overlay (SLC/REP/AVG...); controlled by Settings checkboxes.
    if (m_extensionPlotter)
    {
        m_extensionPlotter->setHostVisible(m_curveVisibility.value(0, true));
        m_extensionPlotter->updateForViewport(loader, visibleStart, visibleEnd);

        // Ensure extension values are within the visible Y-range of the ADC/labels panel.
        // ADC y-range was originally sized for ADC rectangles only; extension counters like LIN can grow large (e.g. 63),
        // which would make the line appear "missing" even though tooltip shows the correct value.
        if (m_vecRects.size() > 0 && m_vecRects[0] && m_curveVisibility.value(0, true))
        {
            int maxExt = 0;
            Settings& st = Settings::getInstance();

            struct ExtSpec { QString name; bool isFlag; int id; };
            const QVector<ExtSpec> specs = {
                {"SLC", false, SLC}, {"SEG", false, SEG}, {"REP", false, REP}, {"AVG", false, AVG},
                {"SET", false, SET}, {"ECO", false, ECO}, {"PHS", false, PHS}, {"LIN", false, LIN},
                {"PAR", false, PAR}, {"ACQ", false, ACQ}, {"ONCE", false, ONCE},
                {"NAV", true, NAV}, {"REV", true, REV}, {"SMS", true, SMS}, {"REF", true, REF},
                {"IMA", true, IMA}, {"OFF", true, OFF}, {"NOISE", true, NOISE},
                {"PMC", true, PMC}, {"NOROT", true, NOROT}, {"NOPOS", true, NOPOS}, {"NOSCL", true, NOSCL},
            };

            // Find block indices intersecting the viewport
            const auto& edges = loader->getBlockEdges();
            if (edges.size() > 1)
            {
                auto itL = std::upper_bound(edges.begin(), edges.end(), visibleStart);
                int b0 = std::max(0, int(std::distance(edges.begin(), itL)) - 1);
                auto itU = std::upper_bound(edges.begin(), edges.end(), visibleEnd);
                int b1 = std::min(int(loader->getDecodedSeqBlocks().size()) - 1,
                                  std::max(b0, int(std::distance(edges.begin(), itU)) - 1));
                for (int b = b0; b <= b1; ++b)
                {
                    for (const auto& s : specs)
                    {
                        if (!st.isExtensionLabelEnabled(s.name))
                            continue;
                        if (s.isFlag)
                        {
                            bool v = false;
                            if (loader->getFlagValueAfterBlock(b, s.id, v))
                                maxExt = std::max(maxExt, v ? 1 : 0);
                        }
                        else
                        {
                            int v = 0;
                            if (loader->getCounterValueAfterBlock(b, s.id, v))
                                maxExt = std::max(maxExt, v);
                        }
                    }
                }
            }

            QCPRange yr = m_vecRects[0]->axis(QCPAxis::atLeft)->range();
            const double upperNeeded = std::max<double>(yr.upper, maxExt);
            if (upperNeeded > yr.upper)
            {
                double pad = std::max(1.0, upperNeeded * 0.05);
                QCPRange expanded(std::min(yr.lower, 0.0), upperNeeded + pad);
                m_vecRects[0]->axis(QCPAxis::atLeft)->setRange(expanded);
                // Persist expanded range so subsequent locked-mode draws keep it.
                // computeAndLockYAxisRanges only sees raw SET/INC values (e.g. 1),
                // but accumulated counters (e.g. LIN=63) need a larger range.
                if (m_fixedYRanges.size() > 0)
                    m_fixedYRanges[0] = qMakePair(expanded.lower, expanded.upper);
            }
        }
    }

    // Trigger overlay is now drawn by DrawTriggerOverlay(), called independently
    // from ensureRenderedForCurrentViewport() so it works even when ADC data is empty.
}

void WaveformDrawer::DrawTriggerOverlay()
{
    // Draw trigger markers (and duration segments if duration>0) in ADC/labels panel,
    // aligned with SeqPlot.m.  This function is intentionally independent of ADC data
    // so that sequences containing only trigger blocks (no ADC) still show markers.
    if (!m_graphTrigMarkers && !m_graphTrigDurations)
        return;

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader || loader->getDecodedSeqBlocks().empty())
        return;

    if (m_vecRects.isEmpty() || !m_vecRects[0])
        return;

    // Determine visible viewport
    QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
    double visibleStart = viewport.lower;
    double visibleEnd   = viewport.upper;

    // Clamp viewport to TR bounds if TR-Segmented mode is active
    TRManager* trm = m_mainWindow->getTRManager();
    if (trm && m_mainWindow->getPulseqLoader()->hasRepetitionTime() && trm->isTrBasedMode())
    {
        int startTr = trm->getTrStartInput()->text().toInt();
        int endTr   = trm->getTrEndInput()->text().toInt();
        double trStart = (startTr - 1) * loader->getRepetitionTime_us() * loader->getTFactor();
        double trEnd   = endTr * loader->getRepetitionTime_us() * loader->getTFactor();
        visibleStart = std::max(visibleStart, trStart);
        visibleEnd   = std::min(visibleEnd,   trEnd);
        if (visibleEnd <= visibleStart) {
            visibleStart = trStart;
            visibleEnd   = std::max(trStart + 1e-6, trEnd);
        }
    }

    QVector<double> xMarks, yMarks;
    QVector<double> xSeg,   ySeg;
    xMarks.reserve(64);
    yMarks.reserve(64);
    xSeg.reserve(128);
    ySeg.reserve(128);

    const auto& edges = loader->getBlockEdges();
    if (edges.size() > 1)
    {
        auto itL = std::upper_bound(edges.begin(), edges.end(), visibleStart);
        int b0   = std::max(0, int(std::distance(edges.begin(), itL)) - 1);
        auto itU = std::upper_bound(edges.begin(), edges.end(), visibleEnd);
        int b1   = std::min(int(loader->getDecodedSeqBlocks().size()) - 1,
                            std::max(b0, int(std::distance(edges.begin(), itU)) - 1));

        for (int b = b0; b <= b1; ++b)
        {
            SeqBlock* blk = loader->getDecodedSeqBlocks()[b];
            if (!blk || !blk->isTrigger())
                continue;
            const TriggerEvent& trg = blk->GetTriggerEvent();
            const double t0  = edges[b] + trg.delay    * loader->getTFactor();
            const double dur  =            trg.duration * loader->getTFactor();

            // Marker at trigger start
            xMarks.append(t0);
            yMarks.append(0.0);

            // Duration segment if any
            if (dur > 0)
            {
                const double t1 = t0 + dur;
                if (t1 >= visibleStart && t0 <= visibleEnd)
                {
                    xSeg.append(t0); ySeg.append(0.0);
                    xSeg.append(t1); ySeg.append(0.0);
                    xSeg.append(t1); ySeg.append(std::numeric_limits<double>::quiet_NaN()); // break
                }
            }
        }
    }

    const bool show = m_curveVisibility.value(0, true);
    if (m_graphTrigMarkers)
    {
        m_graphTrigMarkers->setData(xMarks, yMarks);
        m_graphTrigMarkers->setVisible(show && !xMarks.isEmpty());
    }
    if (m_graphTrigDurations)
    {
        // Remove trailing NaN for cleanliness
        if (!ySeg.isEmpty() && std::isnan(ySeg.last())) { xSeg.removeLast(); ySeg.removeLast(); }
        m_graphTrigDurations->setData(xSeg, ySeg);
        m_graphTrigDurations->setVisible(show && !xSeg.isEmpty());
    }
}


void WaveformDrawer::DrawGWaveform(const double& dStartTime, double dEndTime)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getDecodedSeqBlocks().empty()) return;

    // Determine visible viewport in internal time units
    if (m_vecRects.isEmpty() || !m_vecRects[0]) {
        qDebug() << "ERROR: m_vecRects not initialized!";
        return;
    }
    QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
    double visibleStart = viewport.lower;
    double visibleEnd = viewport.upper;

    // Clamp viewport to TR bounds if TR-Segmented mode is active
    TRManager* trm2 = m_mainWindow->getTRManager();
    if (trm2 && m_mainWindow->getPulseqLoader()->hasRepetitionTime() && trm2->isTrBasedMode())
    {
        int startTr = trm2->getTrStartInput()->text().toInt();
        int endTr = trm2->getTrEndInput()->text().toInt();
        double trStart = (startTr - 1) * loader->getRepetitionTime_us() * loader->getTFactor();
        double trEnd = endTr * loader->getRepetitionTime_us() * loader->getTFactor();
        // Intersect with TR bounds to keep zoom fidelity
        visibleStart = std::max(visibleStart, trStart);
        visibleEnd = std::min(visibleEnd, trEnd);
        if (visibleEnd <= visibleStart) {
            visibleStart = trStart;
            visibleEnd = std::max(trStart + 1e-6, trEnd);
        }
    }

    // Use simple LOD system
    LODLevel currentLODLevel = getCurrentLODLevel();
    double tFactor = loader->getTFactor();

    // Phase 2: On-demand gradients per channel using loader cache
    for (int channel = 0; channel < 3; ++channel) {
        int curveIndex = channel + 3;
        int px = 0;
        if (m_vecRects.size() > curveIndex && m_vecRects[curveIndex])
            px = qMax(1, static_cast<int>(qRound(m_vecRects[curveIndex]->width() * m_mainWindow->devicePixelRatioF())));
        // Respect LOD: in FULL_DETAIL mode, disable decimation by faking a huge pixel width
        int pxEffective = (currentLODLevel == LODLevel::DOWNSAMPLED ? px : qMax(px, 100000));

        QVector<double> tG, vG;
        loader->getGradViewportDecimated(channel, visibleStart, visibleEnd, pxEffective, tG, vG);

        // Apply unit conversion from internal standard (Hz/m) to selected display unit
        {
            Settings& s = Settings::getInstance();
            const QString toUnit = s.getGradientUnitString();
            if (toUnit != "Hz/m") {
                for (int i = 0; i < vG.size(); ++i) {
                    double v = vG[i];
                    if (!std::isnan(v)) vG[i] = s.convertGradient(v, "Hz/m", toUnit);
                }
            }
        }

        QCPGraph* target = (channel == 0 ? m_graphGx : (channel == 1 ? m_graphGy : m_graphGz));
        if (target) {
            target->setData(tG, vG);
            target->setVisible(m_curveVisibility.value(curveIndex, true) && !tG.isEmpty());

            if (!m_lockYAxisRanges) {
                double mn = std::numeric_limits<double>::max();
                double mx = -std::numeric_limits<double>::infinity();
                for (double v : vG) { if (!std::isnan(v)) { if (v < mn) mn = v; if (v > mx) mx = v; } }
                if (mx >= mn) {
                    double pad = (mx - mn) * 0.05; if (pad == 0) pad = 0.1;
                    m_vecRects[curveIndex]->axis(QCPAxis::atLeft)->setRange(mn - pad, mx + pad);
                }
            } else {
                if (m_vecRects.size() > curveIndex && m_vecRects[curveIndex])
                    m_vecRects[curveIndex]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[curveIndex].first, m_fixedYRanges[curveIndex].second);
            }
        }
    }
}

void WaveformDrawer::computeAndLockYAxisRanges()
{
    // Compute global min/max for each channel from merged series and set fixed ranges.
    // This prevents per-TR/window autoscale jitter and keeps visual comparison stable.
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader) return;

    auto computeRange = [](const QVector<double>& vals){
        double mn = std::numeric_limits<double>::max();
        double mx = -std::numeric_limits<double>::infinity();
        for (double v : vals) { if (std::isnan(v)) continue; mn = std::min(mn, v); mx = std::max(mx, v);} 
        if (mx < mn) { mn = -1.0; mx = 1.0; }
        double pad = (mx - mn) * 0.05; if (pad == 0) pad = 1.0; return qMakePair(mn - pad, mx + pad);
    };

    // 0: ADC/labels -> use computed adcHeight similar to DrawADCWaveform
    double maxAbsLabel = 0.0;
    for (auto* blk : loader->getDecodedSeqBlocks()) {
        if (!blk) continue; if (blk->isLabel()) {
            const auto& sets = blk->GetLabelSetEvents(); const auto& incs = blk->GetLabelIncEvents();
            for (const auto& e : sets) maxAbsLabel = std::max(maxAbsLabel, std::abs((double)e.numVal.second));
            for (const auto& e : incs) maxAbsLabel = std::max(maxAbsLabel, std::abs((double)e.numVal.second));
        }
    }
    // Include precomputed max accumulated counter (e.g. LIN=63 from INC events)
    maxAbsLabel = std::max(maxAbsLabel, (double)loader->getMaxAccumulatedCounter());
    if (maxAbsLabel <= 0.0) maxAbsLabel = 1.0;
    double adcHeight = maxAbsLabel * 1.2;
    double adcPad = adcHeight * 0.1;
    m_fixedYRanges[0] = qMakePair(0.0 - adcPad, adcHeight + adcPad);

    // 1: RF mag, 2: RF/ADC phase (use on-demand global ranges without needing merged arrays)
    {
        auto rAmp = loader->getRfGlobalRangeAmp();
        auto rPh  = loader->getRfGlobalRangePh();
        double padA = (rAmp.second - rAmp.first) * 0.05; if (padA == 0) padA = 1.0;
        double padP = (rPh.second  - rPh.first)  * 0.05; if (padP == 0) padP = 1.0;
        m_fixedYRanges[1] = qMakePair(rAmp.first - padA, rAmp.second + padA);
        m_fixedYRanges[2] = qMakePair(rPh.first  - padP, rPh.second  + padP);
    }

    // 3: Gx, 4: Gy, 5: Gz (convert from internal standard Hz/m to display unit)
    {
        Settings& s = Settings::getInstance();
        const QString toUnit = s.getGradientUnitString();
        auto convertRange = [&](QPair<double,double> r) {
            if (toUnit == "Hz/m") return r;
            double a = r.first, b = r.second;
            if (std::isfinite(a)) a = s.convertGradient(a, "Hz/m", toUnit);
            if (std::isfinite(b)) b = s.convertGradient(b, "Hz/m", toUnit);
            return qMakePair(a, b);
        };
        m_fixedYRanges[3] = convertRange(loader->getGradGlobalRange(0));
        m_fixedYRanges[4] = convertRange(loader->getGradGlobalRange(1));
        m_fixedYRanges[5] = convertRange(loader->getGradGlobalRange(2));
    }

    // Apply ranges now
    for (int i = 0; i < m_vecRects.size() && i < m_fixedYRanges.size(); ++i) {
        if (m_vecRects[i]) {
            m_vecRects[i]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[i].first, m_fixedYRanges[i].second);
        }
    }
    m_lockYAxisRanges = true;
}

void WaveformDrawer::DrawBlockEdges()
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getDecodedSeqBlocks().empty()) return;

    const auto& edges = loader->getBlockEdges();
    if (edges.isEmpty()) return;

    // Determine visible viewport
    if (m_vecRects.isEmpty() || !m_vecRects[0]) return;
    QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
    double visibleStart = viewport.lower;
    double visibleEnd = viewport.upper;

    // Build per-rect vertical line segments with NaN breaks
    for (int r = 0; r < m_vecRects.size(); ++r)
    {
        if (!m_blockEdgeGraphs.value(r)) continue;
        QVector<double> xs; xs.reserve(edges.size()*3);
        QVector<double> ys; ys.reserve(edges.size()*3);

        // Use current y-range of the rect to span full height
        QCPRange yr = m_vecRects[r]->axis(QCPAxis::atLeft)->range();
        double yMin = yr.lower;
        double yMax = yr.upper;

        for (int i = 0; i < edges.size(); ++i)
        {
            double t = edges[i];
            if (t < visibleStart || t > visibleEnd) continue;
            xs.append(t); ys.append(yMin);
            xs.append(t); ys.append(yMax);
            xs.append(t); ys.append(std::numeric_limits<double>::quiet_NaN()); // break
        }
        // Remove trailing NaN to avoid spurious points
        if (!ys.isEmpty() && std::isnan(ys.last())) { xs.removeLast(); ys.removeLast(); }
        m_blockEdgeGraphs[r]->setData(xs, ys);
        m_blockEdgeGraphs[r]->setVisible(bShowBlocksEdges && !xs.isEmpty());
    }
}

void WaveformDrawer::setShowCurve(int curveIndex, bool show)
{
    if (curveIndex >= 0 && curveIndex < m_curveVisibility.size())
    {
        m_curveVisibility[curveIndex] = show;
    }
}

void WaveformDrawer::updateCurveVisibility()
{
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    if (!customPlot || !customPlot->plotLayout())
        return;
    // NOTE: Do not rely on plotLayout()->rowCount() staying in sync when taking/adding elements.
    // We will call plotLayout()->simplify() after rebuild to remove empty rows/cols.

    // Map axis rect pointer -> curve index in m_curveVisibility.
    // NOTE: Layout row order can change due to axis reordering, so we must not assume row==index.
    auto curveIndexForRect = [&](QCPAxisRect* rect) -> int {
        if (!rect) return -1;
        if (rect == m_pADCLabelsRect) return 0;
        if (rect == m_pRfMagRect) return 1;
        if (rect == m_pRfADCPhaseRect) return 2;
        if (rect == m_pGxRect) return 3;
        if (rect == m_pGyRect) return 4;
        if (rect == m_pGzRect) return 5;
        return -1;
    };

    // Map label -> rect pointer. Used to rebuild layout with/without hidden rects.
    QMap<QString, QCPAxisRect*> labelToRect;
    labelToRect["ADC/labels"] = m_pADCLabelsRect;
    labelToRect["RF mag"] = m_pRfMagRect;
    labelToRect["RF/ADC ph"] = m_pRfADCPhaseRect;
    labelToRect["GX"] = m_pGxRect;
    labelToRect["GY"] = m_pGyRect;
    labelToRect["GZ"] = m_pGzRect;
    
    // Handle auto-expand mode vs fixed layout mode
    if (m_autoExpandMode) {
        const int kMinAxisRectHeightPx = 50;
        // Auto-expand: physically remove hidden axis rects from the layout.
        // Rationale: QCustomPlot axis rects can have a non-zero implicit minimum size
        // (e.g. due to axes/margins). Even with rowStretchFactors=0, this may leave
        // residual blank space. Rebuilding the layout eliminates that.

        // 1) Detach all known rects from the layout (non-destructive).
        for (QCPAxisRect* rect : m_vecRects)
        {
            if (rect)
                customPlot->plotLayout()->take(rect);
        }

        // 2) Re-add only visible rects in the current UI order.
        int visibleCount = 0;
        for (const QString& label : m_axesOrder)
        {
            QCPAxisRect* rect = labelToRect.value(label, nullptr);
            if (!rect) continue;
            const int curveIdx = curveIndexForRect(rect);
            const bool visible = (curveIdx >= 0) ? m_curveVisibility.value(curveIdx, true) : true;
            if (!visible)
            {
                rect->setVisible(false);
                rect->setMaximumSize(QWIDGETSIZE_MAX, 0);
                rect->setMinimumSize(0, 0);
                continue;
            }
            rect->setVisible(true);
            rect->setMinimumSize(QSize(0, kMinAxisRectHeightPx));
            rect->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            customPlot->plotLayout()->addElement(visibleCount, 0, rect);
            ++visibleCount;
        }

        // 3) Remove empty rows/cols, then equally distribute height among visible rows.
        // simplify() is required because taking elements can leave empty rows which still consume space.
        customPlot->plotLayout()->simplify();
        const int newRowCount = customPlot->plotLayout()->rowCount();
        if (newRowCount > 0)
            customPlot->plotLayout()->setRowStretchFactors(QList<double>(newRowCount, 1.0));
    } else {
        const int kMinAxisRectHeightPx = 50;
        // Fixed layout: ensure all rectangles are present (even if curve is hidden),
        // and distribute height equally.
        for (QCPAxisRect* rect : m_vecRects)
        {
            if (rect)
                customPlot->plotLayout()->take(rect);
        }

        int r = 0;
        for (const QString& label : m_axesOrder)
        {
            QCPAxisRect* rect = labelToRect.value(label, nullptr);
            if (!rect) continue;
            rect->setVisible(true);
            rect->setMinimumSize(QSize(0, kMinAxisRectHeightPx));
            rect->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            customPlot->plotLayout()->addElement(r, 0, rect);
            ++r;
        }
        customPlot->plotLayout()->simplify();
        const int newRowCount = customPlot->plotLayout()->rowCount();
        if (newRowCount > 0)
            customPlot->plotLayout()->setRowStretchFactors(QList<double>(newRowCount, 1.0));
    }

    // Synchronize graph visibility with curveVisibility so that hiding all
    // checkboxes truly hides all waveforms, even before the next redraw.
    if (m_graphADC)
        m_graphADC->setVisible(m_curveVisibility.value(0, false));
    if (m_graphRFMag)
        m_graphRFMag->setVisible(m_curveVisibility.value(1, false));
    if (m_graphRFPh)
        m_graphRFPh->setVisible(m_curveVisibility.value(2, false));
    if (m_graphGx)
        m_graphGx->setVisible(m_curveVisibility.value(3, false));
    if (m_graphGy)
        m_graphGy->setVisible(m_curveVisibility.value(4, false));
    if (m_graphGz)
        m_graphGz->setVisible(m_curveVisibility.value(5, false));
    if (m_graphTrigMarkers)
        m_graphTrigMarkers->setVisible(m_curveVisibility.value(0, false));
    if (m_graphTrigDurations)
        m_graphTrigDurations->setVisible(m_curveVisibility.value(0, false));

    // Keep extension overlay in sync with ADC axis visibility.
    if (m_extensionPlotter)
        m_extensionPlotter->setHostVisible(m_curveVisibility.value(0, true));

    // Ensure x-axis labels are on the bottom-most visible rect after layout/visibility changes.
    configureXAxisLabels();

    // Replot to apply both layout and visibility changes
    customPlot->replot();
}

void WaveformDrawer::setAutoExpandMode(bool autoExpand)
{
    m_autoExpandMode = autoExpand;
    // Update layout immediately when mode changes
    updateCurveVisibility();
}

bool WaveformDrawer::getAutoExpandMode() const
{
    return m_autoExpandMode;
}

void WaveformDrawer::applySubplotLayout(int rows, int cols, int index)
{
    // Only support single column for now to match request: rows fill full height; each row equal height
    if (rows <= 0 || cols <= 0 || cols != 1) return;
    
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    
    // Set equal height for all rows
    QList<double> stretchFactors;
    for (int i = 0; i < rows; ++i) {
        stretchFactors.append(1.0);
    }
    
    customPlot->plotLayout()->setRowStretchFactors(stretchFactors);

    customPlot->replot();
}

void WaveformDrawer::ensureRenderedForCurrentViewport()
{
    try {
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        if (!loader) {
            if (DEBUG_LOD_SYSTEM) {
                qDebug().noquote() << "[LOD] No loader available";
            }
            return;
        }
        // Redraw visible content for all channels based on the current viewport
        DrawRFWaveform();
        DrawADCWaveform();
        DrawGWaveform();
        DrawTriggerOverlay();
        if (getShowBlockEdges()) DrawBlockEdges();
        m_mainWindow->ui->customPlot->replot();
    } catch (const std::exception& e) {
        if (DEBUG_LOD_SYSTEM) {
            qDebug().noquote() << "[LOD] Exception in ensureRenderedForCurrentViewport:" << e.what();
        }
    } catch (...) {
        if (DEBUG_LOD_SYSTEM) {
            qDebug().noquote() << "[LOD] Unknown exception in ensureRenderedForCurrentViewport";
        }
    }
}

void WaveformDrawer::updateAxisLabels()
{
    // Update Y-axis labels using each rect's fixed identity (matching InitSequenceFigure).
    // Do NOT use m_axesOrder indices — m_axesOrder is the visual order which differs
    // from m_vecRects order when the user has reordered subplots.
    Settings& settings = Settings::getInstance();
    QString gradientUnit = settings.getGradientUnitString();

    if (m_pADCLabelsRect)   m_pADCLabelsRect->axis(QCPAxis::atLeft)->setLabel("ADC/labels");
    if (m_pRfMagRect)       m_pRfMagRect->axis(QCPAxis::atLeft)->setLabel("RF mag(Hz)");
    if (m_pRfADCPhaseRect)  m_pRfADCPhaseRect->axis(QCPAxis::atLeft)->setLabel("RF/ADC ph(rad)");
    if (m_pGxRect)          m_pGxRect->axis(QCPAxis::atLeft)->setLabel("GX (" + gradientUnit + ")");
    if (m_pGyRect)          m_pGyRect->axis(QCPAxis::atLeft)->setLabel("GY (" + gradientUnit + ")");
    if (m_pGzRect)          m_pGzRect->axis(QCPAxis::atLeft)->setLabel("GZ (" + gradientUnit + ")");

    // Update x-axis label on the bottom-most visible rect
    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot) {
        QCustomPlot* plot = m_mainWindow->ui->customPlot;
        int rowCount = plot->plotLayout()->rowCount();
        if (rowCount > 0) {
            QCPAxisRect* lastRect = qobject_cast<QCPAxisRect*>(plot->plotLayout()->element(rowCount - 1, 0));
            if (lastRect) {
                lastRect->axis(QCPAxis::atBottom)->setLabel(currentTimeAxisLabel());
                applyTimeAxisFormatting(lastRect->axis(QCPAxis::atBottom));
            }
        }
    }
    
    // Trigger replot to update labels and data
    m_mainWindow->ui->customPlot->replot();
}

// Simple LOD system - no complex decision logic needed
void WaveformDrawer::setUseDownsampling(bool useDownsampling)
{
    m_useDownsampling = useDownsampling;
    
    // Trigger replot to apply new LOD level
    m_mainWindow->ui->customPlot->replot();
}

WaveformDrawer::LODLevel WaveformDrawer::getCurrentLODLevel() const
{
    return m_useDownsampling ? LODLevel::DOWNSAMPLED : LODLevel::FULL_DETAIL;
}

void WaveformDrawer::setShowTeGuides(bool show)
{
    if (m_showTeGuides == show)
        return;

    m_showTeGuides = show;
    if (!show)
    {
        hideTeGuideItems();
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
        return;
    }

    if (!m_vecRects.isEmpty() && m_vecRects[0])
    {
        const QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
        updateTeGuides(viewport.lower, viewport.upper);
        updateKxKyZeroGuides(viewport.lower, viewport.upper);
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void WaveformDrawer::setShowKxKyZeroGuides(bool show)
{
    if (m_showKxKyZeroGuides == show)
        return;

    m_showKxKyZeroGuides = show;
    if (!show)
    {
        hideKxKyZeroGuideItems();
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
        return;
    }

    if (!m_vecRects.isEmpty() && m_vecRects[0])
    {
        const QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
        updateKxKyZeroGuides(viewport.lower, viewport.upper);
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}


void WaveformDrawer::applyLTTBDownsampling(const QVector<double>& time, const QVector<double>& values,
                                          int targetPoints,
                                          QVector<double>& sampledTime, QVector<double>& sampledValues)
{
    if (time.isEmpty() || values.isEmpty() || time.size() != values.size()) {
        sampledTime.clear();
        sampledValues.clear();
        return;
    }

    // Guard targetPoints
    if (targetPoints <= 0) targetPoints = 2;
    if (targetPoints > time.size()) targetPoints = time.size();
    
    if (time.size() <= targetPoints) {
        // No downsampling needed
        sampledTime = time;
        sampledValues = values;
        return;
    }
    
    // LTTB algorithm implementation
    sampledTime.clear();
    sampledValues.clear();
    sampledTime.reserve(targetPoints);
    sampledValues.reserve(targetPoints);

    // Always include first point
    sampledTime.append(time.first());
    sampledValues.append(values.first());

    if (targetPoints <= 2) {
        // If only 2 points, include last point
        if (targetPoints == 2) {
            sampledTime.append(time.last());
            sampledValues.append(values.last());
        }
        return;
    }

    // Calculate bucket size
    int bucketSize = (time.size() - 2) / (targetPoints - 2);
    
    for (int i = 1; i < targetPoints - 1; ++i) {
        int bucketStart = (i - 1) * bucketSize + 1;
        int bucketEnd = qMin(bucketStart + bucketSize, static_cast<int>(time.size()) - 1);
        
        // Find the point with the largest triangle area
        double maxArea = -1.0;
        int maxIndex = bucketStart;
        
        for (int j = bucketStart; j < bucketEnd; ++j) {
            double area = calculateTriangleArea(
                time[i-1], values[i-1],           // Previous point
                time[j], values[j],               // Current point
                time[bucketEnd], values[bucketEnd] // Next bucket start
            );
            
            if (area > maxArea) {
                maxArea = area;
                maxIndex = j;
            }
        }
        
        sampledTime.append(time[maxIndex]);
        sampledValues.append(values[maxIndex]);
    }

    // Always include last point
    sampledTime.append(time.last());
    sampledValues.append(values.last());
}

double WaveformDrawer::calculateTriangleArea(double x1, double y1, double x2, double y2, double x3, double y3)
{
    // Using the formula: |(x2-x1)(y3-y1) - (x3-x1)(y2-y1)| / 2
    return qAbs((x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1)) / 2.0;
}

 

// Min-max per pixel buckets: O(n) and preserves envelope; outputs up to 2*pixelBuckets points
void WaveformDrawer::applyMinMaxDownsampling(const QVector<double>& time, const QVector<double>& values,
                                            int pixelBuckets,
                                            QVector<double>& outTime, QVector<double>& outValues)
{
    outTime.clear(); outValues.clear();
    if (time.isEmpty() || values.isEmpty() || time.size() != values.size() || pixelBuckets <= 0) return;
    int n = time.size();
    if (n <= 2*pixelBuckets) { outTime = time; outValues = values; return; }
    outTime.reserve(2*pixelBuckets);
    outValues.reserve(2*pixelBuckets);
    double tMin = time.first();
    double tMax = time.last();
    if (tMax <= tMin) { outTime = time; outValues = values; return; }
    double bucketW = (tMax - tMin) / pixelBuckets;
    int idx = 0;
    for (int b = 0; b < pixelBuckets; ++b) {
        double bx0 = tMin + b*bucketW;
        double bx1 = (b == pixelBuckets-1) ? tMax : (bx0 + bucketW);
        double ymin = std::numeric_limits<double>::infinity();
        double ymax = -std::numeric_limits<double>::infinity();
        double txMin = bx0, txMax = bx0;
        // advance idx to first point >= bx0
        while (idx < n && time[idx] < bx0) ++idx;
        int j = idx;
        for (; j < n && time[j] <= bx1; ++j) {
            double v = values[j];
            if (std::isnan(v)) continue; // skip breaks
            if (v < ymin) { ymin = v; txMin = time[j]; }
            if (v > ymax) { ymax = v; txMax = time[j]; }
        }
        if (ymin != std::numeric_limits<double>::infinity()) {
            outTime.append(txMin); outValues.append(ymin);
            if (ymax != ymin) { outTime.append(txMax); outValues.append(ymax); }
        }
        idx = j;
    }
}

// Simple LOD system - no complex precomputation needed
void WaveformDrawer::processViewportChangeSimple(double visibleStart, double visibleEnd)
{
    // Simple viewport change processing - just update the last viewport
    m_lastViewportLower = visibleStart;
    m_lastViewportUpper = visibleEnd;
}

// Simple LOD system - no complex caching needed

void WaveformDrawer::cleanupLODCache()
{
    // Simple cache cleanup - remove oldest entries
    if (m_lodCache.size() > 50) {
        auto it = m_lodCache.begin();
        while (it != m_lodCache.end() && m_lodCache.size() > 50) {
            it = m_lodCache.erase(it);
        }
    }
}

// Simple LOD system - no complex precomputation needed



void WaveformDrawer::ensureTeGuideCapacity()
{
    if (!m_mainWindow || !m_mainWindow->ui)
        return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    if (!plot)
        return;

    const int rectCount = m_vecRects.size();
    auto resetPool = [&](QVector<QVector<QCPItemStraightLine*>>& pool)
    {
        for (auto& axisPool : pool)
        {
            for (QCPItemStraightLine* line : axisPool)
                if (line)
                    plot->removeItem(line);
        }
        pool.clear();
        pool.resize(rectCount);
    };

    if (m_excitationGuideLines.size() != rectCount)
    {
        resetPool(m_excitationGuideLines);
    }
    else
    {
        for (int i = 0; i < m_excitationGuideLines.size(); ++i)
        {
            QCPAxisRect* rect = m_vecRects.value(i, nullptr);
            if (!rect)
                continue;
            for (QCPItemStraightLine* line : m_excitationGuideLines[i])
            {
                if (!line)
                    continue;
                line->setClipAxisRect(rect);
                line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            }
        }
    }

    if (m_teEchoGuideLines.size() != rectCount)
    {
        resetPool(m_teEchoGuideLines);
    }
    else
    {
        for (int i = 0; i < m_teEchoGuideLines.size(); ++i)
        {
            QCPAxisRect* rect = m_vecRects.value(i, nullptr);
            if (!rect)
                continue;
            for (QCPItemStraightLine* line : m_teEchoGuideLines[i])
            {
                if (!line)
                    continue;
                line->setClipAxisRect(rect);
                line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            }
        }
    }
}

void WaveformDrawer::hideTeGuideItems()
{
    for (auto& pool : m_excitationGuideLines)
        for (QCPItemStraightLine* line : pool)
            if (line)
                line->setVisible(false);
    for (auto& pool : m_teEchoGuideLines)
        for (QCPItemStraightLine* line : pool)
            if (line)
                line->setVisible(false);
}

void WaveformDrawer::updateTeGuides(double visibleStart, double visibleEnd)
{
    ensureTeGuideCapacity();
    if (!m_showTeGuides)
    {
        hideTeGuideItems();
        return;
    }

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
    {
        hideTeGuideItems();
        return;
    }

    // Only require a valid TE/EchoTime definition here. Excitation centers may come either
    // from explicit RF use metadata (v1.5+) or from detected RF uses (classifyRfUse) in
    // older Pulseq versions. The latter is allowed, with an approximate warning issued
    // by TRManager::canEnableTeOverlay.
    if (!loader->hasEchoTimeDefinition())
    {
        hideTeGuideItems();
        return;
    }

    const auto& centers = loader->getExcitationCenters();
    const double teOffset = loader->getTeDurationAxis();

    QVector<double> visibleCenters;
    visibleCenters.reserve(centers.size());
    QVector<double> visibleTePositions;
    visibleTePositions.reserve(centers.size());
    for (double c : centers)
    {
        if (c >= visibleStart && c <= visibleEnd)
            visibleCenters.append(c);
        double tePos = c + teOffset;
        if (tePos >= visibleStart && tePos <= visibleEnd)
            visibleTePositions.append(tePos);
    }

    if (!m_mainWindow || !m_mainWindow->ui)
        return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;

    auto syncPool = [&](QVector<QVector<QCPItemStraightLine*>>& pool,
                        const QVector<double>& positions,
                        double penWidth)
    {
        for (int axisIdx = 0; axisIdx < pool.size(); ++axisIdx)
        {
            auto& axisPool = pool[axisIdx];
            QCPAxisRect* rect = m_vecRects.value(axisIdx, nullptr);
            if (!rect)
            {
                for (QCPItemStraightLine* line : axisPool)
                    if (line)
                        line->setVisible(false);
                continue;
            }

            while (axisPool.size() < positions.size())
            {
                auto* line = new QCPItemStraightLine(plot);
                QPen pen(Qt::black);
                pen.setStyle(Qt::DashLine);
                pen.setWidthF(penWidth);
                line->setPen(pen);
                line->setClipToAxisRect(true);
                line->setClipAxisRect(rect);
                line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                line->setVisible(false);
                axisPool.append(line);
            }

            for (int idx = 0; idx < axisPool.size(); ++idx)
            {
                QCPItemStraightLine* line = axisPool[idx];
                if (!line)
                    continue;
                if (idx < positions.size())
                {
                    double x = positions[idx];
                    double lower = rect->axis(QCPAxis::atLeft)->range().lower;
                    double upper = rect->axis(QCPAxis::atLeft)->range().upper;
                    line->point1->setCoords(x, lower);
                    line->point2->setCoords(x, upper);
                    line->setVisible(true);
                }
                else
                {
                    line->setVisible(false);
                }
            }
        }
    };

    syncPool(m_excitationGuideLines, visibleCenters, 0.8);
    syncPool(m_teEchoGuideLines, visibleTePositions, 1.0);
}

void WaveformDrawer::ensureKxKyZeroGuideCapacity()
{
    if (!m_mainWindow || !m_mainWindow->ui)
        return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    if (!plot)
        return;

    const int rectCount = m_vecRects.size();
    auto resetPool = [&](QVector<QVector<QCPItemStraightLine*>>& pool)
    {
        for (auto& axisPool : pool)
        {
            for (QCPItemStraightLine* line : axisPool)
                if (line)
                    plot->removeItem(line);
        }
        pool.clear();
        pool.resize(rectCount);
    };

    if (m_kxKyZeroGuideLines.size() != rectCount)
    {
        resetPool(m_kxKyZeroGuideLines);
    }
    else
    {
        for (int i = 0; i < m_kxKyZeroGuideLines.size(); ++i)
        {
            QCPAxisRect* rect = m_vecRects.value(i, nullptr);
            if (!rect)
                continue;
            for (QCPItemStraightLine* line : m_kxKyZeroGuideLines[i])
            {
                if (!line)
                    continue;
                line->setClipAxisRect(rect);
                line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
                line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            }
        }
    }
}

void WaveformDrawer::hideKxKyZeroGuideItems()
{
    for (auto& pool : m_kxKyZeroGuideLines)
        for (QCPItemStraightLine* line : pool)
            if (line)
                line->setVisible(false);
}

void WaveformDrawer::updateKxKyZeroGuides(double visibleStart, double visibleEnd)
{
    ensureKxKyZeroGuideCapacity();
    if (!m_showKxKyZeroGuides)
    {
        hideKxKyZeroGuideItems();
        return;
    }

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
    {
        hideKxKyZeroGuideItems();
        return;
    }

    // Ensure trajectory is prepared
    loader->ensureTrajectoryPrepared();
    if (!loader->hasTrajectoryData())
    {
        hideKxKyZeroGuideItems();
        return;
    }

    const QVector<double> kxKyZeroTimes = loader->getKxKyZeroTimes();
    QVector<double> visibleKxKyZeroPositions;
    visibleKxKyZeroPositions.reserve(kxKyZeroTimes.size());
    for (double t : kxKyZeroTimes)
    {
        if (t >= visibleStart && t <= visibleEnd)
            visibleKxKyZeroPositions.append(t);
    }

    if (!m_mainWindow || !m_mainWindow->ui)
        return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;

    // Use a different style than TE guides to ensure visibility
    // TE uses: black dashed lines (DashLine)
    // kx=ky=0 uses: blue solid lines (SolidLine) - this ensures they won't be covered
    for (int axisIdx = 0; axisIdx < m_kxKyZeroGuideLines.size(); ++axisIdx)
    {
        auto& axisPool = m_kxKyZeroGuideLines[axisIdx];
        QCPAxisRect* rect = m_vecRects.value(axisIdx, nullptr);
        if (!rect)
        {
            for (QCPItemStraightLine* line : axisPool)
                if (line)
                    line->setVisible(false);
            continue;
        }

        while (axisPool.size() < visibleKxKyZeroPositions.size())
        {
            auto* line = new QCPItemStraightLine(plot);
            QPen pen(QColor(0, 100, 200)); // Blue color (RGB: 0, 100, 200)
            pen.setStyle(Qt::SolidLine);    // Solid line (different from TE's DashLine)
            pen.setWidthF(1.2);             // Slightly thicker than TE lines to ensure visibility
            line->setPen(pen);
            line->setClipToAxisRect(true);
            line->setClipAxisRect(rect);
            line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
            line->setVisible(false);
            axisPool.append(line);
        }

        for (int idx = 0; idx < axisPool.size(); ++idx)
        {
            QCPItemStraightLine* line = axisPool[idx];
            if (!line)
                continue;
            if (idx < visibleKxKyZeroPositions.size())
            {
                double x = visibleKxKyZeroPositions[idx];
                double lower = rect->axis(QCPAxis::atLeft)->range().lower;
                double upper = rect->axis(QCPAxis::atLeft)->range().upper;
                line->point1->setCoords(x, lower);
                line->point2->setCoords(x, upper);
                line->setVisible(true);
            }
            else
            {
                line->setVisible(false);
            }
        }
    }
}
