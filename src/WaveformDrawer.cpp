#include "WaveformDrawer.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "InteractionHandler.h"
#include "Settings.h"
#include "ZoomManager.h"
#include "TRManager.h"
#include "ExtensionPlotter.h"
#include "LogManager.h"
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
#include <algorithm>

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
namespace {
constexpr double kPhaseAxisLower = -3.5;
constexpr double kPhaseAxisUpper =  3.5;
}

WaveformDrawer::WaveformDrawer(MainWindow* mainWindow)
    : QObject(mainWindow),
      m_mainWindow(mainWindow),
      m_pADCLabelsRect(nullptr), m_pRfMagRect(nullptr), m_pRfADCPhaseRect(nullptr),
      m_pGxRect(nullptr), m_pGyRect(nullptr), m_pGzRect(nullptr), m_pPnsRect(nullptr),
      bShowBlocksEdges(false), m_nCurrentMaxPoints(MAX_POINTS_PER_GRAPH)
{
    // Initialize curve visibility.
    // Keep PNS and M1x/y/z hidden by default so initial layout is deterministic
    // and does not depend on a later checkbox sync from TRManager.
    m_curveVisibility.resize(10);
    for (int i = 0; i < 10; i++) { m_curveVisibility[i] = true; }
    m_curveVisibility[6] = false; // PNS
    m_curveVisibility[7] = false; // M1x
    m_curveVisibility[8] = false; // M1y
    m_curveVisibility[9] = false; // M1z

    // Initialize auto expand mode - default to true (auto expand behavior)
    m_autoExpandMode = true;

    // Default axes order (UI config default)
    m_axesOrder = QStringList() << "RF mag" << "PNS" << "GZ" << "GY" << "GX" << "RF/ADC ph" << "ADC/labels" << "M1x" << "M1y" << "M1z";
    // Initialize fixed Y ranges container
    m_fixedYRanges.resize(10);
}

WaveformDrawer::~WaveformDrawer()
{
    // All QCustomPlot items are owned by the plot itself.
}

QPen WaveformDrawer::makeRfChannelPen(int channelIndex) const
{
    static const QVector<Qt::PenStyle> styles = {
        Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine, Qt::DashDotDotLine
    };
    const QColor base = colors.isEmpty()
        ? QColor::fromHsv((channelIndex * 47) % 360, 180, 200)
        : colors[channelIndex % colors.size()];
    QPen pen(base);
    pen.setWidthF(1.5);
    pen.setStyle(styles[channelIndex / qMax(1, colors.size()) % styles.size()]);
    return pen;
}

void WaveformDrawer::ensureRfChannelGraphs(int channelCount)
{
    if (!m_mainWindow || !m_mainWindow->ui || !m_mainWindow->ui->customPlot) {
        return;
    }
    QCustomPlot* customPlot = m_mainWindow->ui->customPlot;
    while (m_graphRFMagChannels.size() < channelCount) {
        QCPGraph* graph = customPlot->addGraph(m_pRfMagRect->axis(QCPAxis::atBottom), m_pRfMagRect->axis(QCPAxis::atLeft));
        graph->setLineStyle(QCPGraph::lsLine);
        graph->setScatterStyle(QCPScatterStyle::ssNone);
        graph->setAdaptiveSampling(false);
        graph->setAntialiased(true);
        m_graphRFMagChannels.append(graph);
    }
    while (m_graphRFPhChannels.size() < channelCount) {
        QCPGraph* graph = customPlot->addGraph(m_pRfADCPhaseRect->axis(QCPAxis::atBottom), m_pRfADCPhaseRect->axis(QCPAxis::atLeft));
        graph->setLineStyle(QCPGraph::lsLine);
        graph->setScatterStyle(QCPScatterStyle::ssNone);
        graph->setAdaptiveSampling(false);
        graph->setAntialiased(true);
        m_graphRFPhChannels.append(graph);
    }

    for (int i = 0; i < m_graphRFMagChannels.size(); ++i) {
        if (m_graphRFMagChannels[i]) {
            m_graphRFMagChannels[i]->setPen(makeRfChannelPen(i));
            m_graphRFMagChannels[i]->setVisible(i < channelCount && m_curveVisibility.value(1, true));
        }
    }
    for (int i = 0; i < m_graphRFPhChannels.size(); ++i) {
        if (m_graphRFPhChannels[i]) {
            m_graphRFPhChannels[i]->setPen(makeRfChannelPen(i));
            m_graphRFPhChannels[i]->setVisible(i < channelCount && m_curveVisibility.value(2, true));
        }
    }

    m_graphRFMag = m_graphRFMagChannels.isEmpty() ? nullptr : m_graphRFMagChannels.first();
    m_graphRFPh = m_graphRFPhChannels.isEmpty() ? nullptr : m_graphRFPhChannels.first();
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
    m_pPnsRect = new QCPAxisRect(customPlot);
    m_pM1xRect = new QCPAxisRect(customPlot);
    m_pM1yRect = new QCPAxisRect(customPlot);
    m_pM1zRect = new QCPAxisRect(customPlot);

    m_vecRects.append(m_pADCLabelsRect);
    m_vecRects.append(m_pRfMagRect);
    m_vecRects.append(m_pRfADCPhaseRect);
    m_vecRects.append(m_pGxRect);
    m_vecRects.append(m_pGyRect);
    m_vecRects.append(m_pGzRect);
    m_vecRects.append(m_pPnsRect);
    m_vecRects.append(m_pM1xRect);
    m_vecRects.append(m_pM1yRect);
    m_vecRects.append(m_pM1zRect);

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
        else if (lastAxisName == "PNS") targetRect = m_pPnsRect;
        
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
    m_pPnsRect->axis(QCPAxis::atLeft)->setLabel("PNS (%)");

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
            // then redraw. Without this, changing e.g. mT/m -> Hz/m would keep old Y-ranges.
            computeAndLockYAxisRanges();
            DrawRFWaveform();
            DrawADCWaveform();
            DrawGWaveform();
            if (getShowBlockEdges()) DrawBlockEdges();
            m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
        });
    }

    colors = QVector<QColor>({
        QColor::fromRgbF(0,0.447,0.741), QColor::fromRgbF(0.85,0.325,0.098),
        QColor::fromRgbF(0.929,0.694,0.125), QColor::fromRgbF(0.494,0.184,0.556),
        QColor::fromRgbF(0.466,0.674,0.188), QColor::fromRgbF(0.301,0.745,0.933),
        QColor::fromRgbF(0.635,0.078,0.184), QColor::fromRgbF(0.25,0.25,0.25)
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
    ensureRfChannelGraphs(1);
    // ADC Phase (same rect as RF Phase: m_pRfADCPhaseRect)
    // PERF NOTE: Must use lsLine (not scatter ssDisc). QCustomPlot renders scatter dots
    // individually (per-point QPainter::drawEllipse), while line segments are batched into
    // a single QPainterPath - the difference is ~10x. Scatter caused severe UI lag on
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
    // PNS (rect 6) draws X/Y/Z and norm in one axis
    m_graphPnsX = customPlot->addGraph(m_pPnsRect->axis(QCPAxis::atBottom), m_pPnsRect->axis(QCPAxis::atLeft));
    if (m_graphPnsX)
    {
        // Match PNS-X color to GX color
        QPen pen(colors.isEmpty() ? Qt::blue : colors[2 % colors.size()]);
        pen.setWidthF(1.4);
        m_graphPnsX->setPen(pen);
        m_graphPnsX->setLineStyle(QCPGraph::lsLine);
        m_graphPnsX->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphPnsX->setAdaptiveSampling(true);
        m_graphPnsX->setAntialiased(true);
        m_graphPnsX->setVisible(m_curveVisibility.value(6, true));
    }
    m_graphPnsY = customPlot->addGraph(m_pPnsRect->axis(QCPAxis::atBottom), m_pPnsRect->axis(QCPAxis::atLeft));
    if (m_graphPnsY)
    {
        // Match PNS-Y color to GY color
        QPen pen(colors.isEmpty() ? Qt::darkYellow : colors[3 % colors.size()]);
        pen.setWidthF(1.4);
        m_graphPnsY->setPen(pen);
        m_graphPnsY->setLineStyle(QCPGraph::lsLine);
        m_graphPnsY->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphPnsY->setAdaptiveSampling(true);
        m_graphPnsY->setAntialiased(true);
        m_graphPnsY->setVisible(m_curveVisibility.value(6, true));
    }
    m_graphPnsZ = customPlot->addGraph(m_pPnsRect->axis(QCPAxis::atBottom), m_pPnsRect->axis(QCPAxis::atLeft));
    if (m_graphPnsZ)
    {
        // Match PNS-Z color to GZ color
        QPen pen(colors.isEmpty() ? Qt::darkCyan : colors[4 % colors.size()]);
        pen.setWidthF(1.4);
        m_graphPnsZ->setPen(pen);
        m_graphPnsZ->setLineStyle(QCPGraph::lsLine);
        m_graphPnsZ->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphPnsZ->setAdaptiveSampling(true);
        m_graphPnsZ->setAntialiased(true);
        m_graphPnsZ->setVisible(m_curveVisibility.value(6, true));
    }
    m_graphPnsNorm = customPlot->addGraph(m_pPnsRect->axis(QCPAxis::atBottom), m_pPnsRect->axis(QCPAxis::atLeft));
    if (m_graphPnsNorm)
    {
        // MATLAB safe_plot style: k--
        QPen pen(Qt::black);
        pen.setWidthF(1.6);
        pen.setStyle(Qt::SolidLine);
        m_graphPnsNorm->setPen(pen);
        m_graphPnsNorm->setLineStyle(QCPGraph::lsLine);
        m_graphPnsNorm->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphPnsNorm->setAdaptiveSampling(true);
        m_graphPnsNorm->setAntialiased(true);
        m_graphPnsNorm->setVisible(m_curveVisibility.value(6, true));
    }

    auto m1AxisColor = [&](const QColor& fallback, int colorIndex) {
        const QColor base = colors.isEmpty() ? fallback : colors[colorIndex % colors.size()];
        return base.lighter(135);
    };

    // M1 (first gradient moment) per-axis graphs, one rect each.
    m_graphM1x = customPlot->addGraph(m_pM1xRect->axis(QCPAxis::atBottom), m_pM1xRect->axis(QCPAxis::atLeft));
    if (m_graphM1x)
    {
        QPen pen(m1AxisColor(QColor(Qt::red), 2));
        pen.setWidthF(1.4);
        m_graphM1x->setPen(pen);
        m_graphM1x->setLineStyle(QCPGraph::lsLine);
        m_graphM1x->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphM1x->setAdaptiveSampling(true);
        m_graphM1x->setAntialiased(true);
        m_graphM1x->setVisible(m_curveVisibility.value(7, false));
    }
    m_graphM1y = customPlot->addGraph(m_pM1yRect->axis(QCPAxis::atBottom), m_pM1yRect->axis(QCPAxis::atLeft));
    if (m_graphM1y)
    {
        QPen pen(m1AxisColor(QColor(Qt::darkYellow), 3));
        pen.setWidthF(1.4);
        m_graphM1y->setPen(pen);
        m_graphM1y->setLineStyle(QCPGraph::lsLine);
        m_graphM1y->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphM1y->setAdaptiveSampling(true);
        m_graphM1y->setAntialiased(true);
        m_graphM1y->setVisible(m_curveVisibility.value(8, false));
    }
    m_graphM1z = customPlot->addGraph(m_pM1zRect->axis(QCPAxis::atBottom), m_pM1zRect->axis(QCPAxis::atLeft));
    if (m_graphM1z)
    {
        QPen pen(m1AxisColor(QColor(Qt::darkCyan), 4));
        pen.setWidthF(1.4);
        m_graphM1z->setPen(pen);
        m_graphM1z->setLineStyle(QCPGraph::lsLine);
        m_graphM1z->setScatterStyle(QCPScatterStyle::ssNone);
        m_graphM1z->setAdaptiveSampling(true);
        m_graphM1z->setAntialiased(true);
        m_graphM1z->setVisible(m_curveVisibility.value(9, false));
    }
    // Label the M1 axes once. These are independent of the per-curve
    // visibility flags so the user can still see the axis context when
    // the curve is hidden.
    if (m_pM1xRect) m_pM1xRect->axis(QCPAxis::atLeft)->setLabel("M1x ref=t [s/m]");
    if (m_pM1yRect) m_pM1yRect->axis(QCPAxis::atLeft)->setLabel("M1y ref=t [s/m]");
    if (m_pM1zRect) m_pM1zRect->axis(QCPAxis::atLeft)->setLabel("M1z ref=t [s/m]");

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
    if (!customPlot || !customPlot->plotLayout()) return;
    auto safeTake = [&](QCPAxisRect* rect) {
        if (!rect) return;
        if (rect->layout() != nullptr)
            customPlot->plotLayout()->take(rect);
    };
    // Remove existing elements and re-add in order
    // Take rects and re-add in the new order
    for (int i = 0; i < m_vecRects.size(); ++i)
    {
        safeTake(m_vecRects[i]);
    }
    // Map labels to rects
    QMap<QString, QCPAxisRect*> labelToRect;
    labelToRect["ADC/labels"] = m_pADCLabelsRect;
    labelToRect["RF mag"] = m_pRfMagRect;
    labelToRect["RF/ADC ph"] = m_pRfADCPhaseRect;
    labelToRect["GX"] = m_pGxRect;
    labelToRect["GY"] = m_pGyRect;
    labelToRect["GZ"] = m_pGzRect;
    labelToRect["PNS"] = m_pPnsRect;
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

void WaveformDrawer::applyAxesOrderAndSave(const QStringList& order)
{
    if (order.size() != m_vecRects.size())
        return;
    if (order == m_axesOrder)
        return;

    setAxesOrder(order);
    saveUiConfig();
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
    QStringList newOrder = m_axesOrder;
    QString item = newOrder.takeAt(fromIndex);
    // Insert-before semantics: when dragging to a target row, insert at that row index
    newOrder.insert(toIndex, item);
    applyAxesOrderAndSave(newOrder);
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
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
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
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
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
    m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
}

void WaveformDrawer::updateAxisDragVisual(int yInPlot)
{
    if (!m_dragGhost) return;
    m_dragGhost->position->setCoords(10, yInPlot);
    m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
}

void WaveformDrawer::finishAxisDragVisual()
{
    if (!m_dragGhost) return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;
    plot->removeItem(m_dragGhost);
    m_dragGhost = nullptr;
    m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
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
        setAxesOrder(QStringList() << "RF mag" << "PNS" << "GZ" << "GY" << "GX" << "RF/ADC ph" << "ADC/labels");
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
    m_vecVerticalLine.resize(m_vecRects.size());
    for (int i = 0; i < m_vecVerticalLine.size(); i++)
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

    rebindVerticalLinesToRects();
    ensureTeGuideCapacity();
    ensureKxKyZeroGuideCapacity();
}

void WaveformDrawer::rebindVerticalLinesToRects()
{
    // Bind each vertical line to its owning axis rect (stable by fixed rect index),
    // then toggle visibility by current curve visibility/layout.
    const int n = std::min(m_vecVerticalLine.size(), m_vecRects.size());
    for (int i = 0; i < n; ++i)
    {
        QCPItemStraightLine* line = m_vecVerticalLine[i];
        QCPAxisRect* rect = m_vecRects[i];
        if (!line || !rect) continue;
        line->setClipAxisRect(rect);
        line->point1->setAxisRect(rect);
        line->point2->setAxisRect(rect);
        line->point1->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
        line->point2->setAxes(rect->axis(QCPAxis::atBottom), rect->axis(QCPAxis::atLeft));
    }
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
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
}

void WaveformDrawer::DrawRFWaveform(const double& dStartTime, double dEndTime)
{
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

    LODLevel currentLODLevel = getCurrentLODLevel();
    int pxRF = 0;
    if (m_vecRects.size() > 1 && m_vecRects[1]) {
        pxRF = qMax(1, static_cast<int>(qRound(m_vecRects[1]->width() * m_mainWindow->devicePixelRatioF())));
    }
    int pxRFEffective = (currentLODLevel == LODLevel::DOWNSAMPLED ? pxRF : qMax(pxRF, 100000));

    PulseqLoader::UnifiedRfViewport rfViewport;
    loader->getUnifiedRfViewport(visibleStart, visibleEnd, pxRFEffective, rfViewport);
    ensureRfChannelGraphs(rfViewport.ampTimeByChannel.size());

    auto computeMinMax = [](const QVector<QVector<double>>& allValues, double& mn, double& mx) {
        for (const QVector<double>& values : allValues) {
            for (double value : values) {
                if (!std::isfinite(value)) continue;
                mn = std::min(mn, value);
                mx = std::max(mx, value);
            }
        }
    };

    for (int i = 0; i < m_graphRFMagChannels.size(); ++i) {
        QCPGraph* graph = m_graphRFMagChannels[i];
        if (!graph) continue;
        if (i < rfViewport.ampTimeByChannel.size()) {
            QVector<double> tAmp = rfViewport.ampTimeByChannel[i];
            QVector<double> vAmp = rfViewport.ampValueByChannel[i];
            QVector<double> tAmpCapped, vAmpCapped;
            if (applyFinalPixelBudgetEnvelope(tAmp, vAmp, visibleStart, visibleEnd, pxRF, 8, tAmpCapped, vAmpCapped)) {
                tAmp = std::move(tAmpCapped);
                vAmp = std::move(vAmpCapped);
            }
            rfViewport.ampTimeByChannel[i] = tAmp;
            rfViewport.ampValueByChannel[i] = vAmp;
            graph->setData(tAmp, vAmp);
            graph->setVisible(m_curveVisibility.value(1, true) && !tAmp.isEmpty());
        } else {
            graph->setData(QVector<double>(), QVector<double>());
            graph->setVisible(false);
        }
    }
    // TODO(perf): RF phase and ADC phase need their own budget strategy. Do not
    // apply min/max envelopes directly to wrapped phase data; crossing -pi/pi
    // would create false full-height spikes. Use unwrap-aware reduction or
    // stride/representative sampling if phase point counts become a bottleneck.
    for (int i = 0; i < m_graphRFPhChannels.size(); ++i) {
        QCPGraph* graph = m_graphRFPhChannels[i];
        if (!graph) continue;
        if (i < rfViewport.phaseTimeByChannel.size()) {
            graph->setData(rfViewport.phaseTimeByChannel[i], rfViewport.phaseValueByChannel[i]);
            graph->setVisible(m_curveVisibility.value(2, true) && !rfViewport.phaseTimeByChannel[i].isEmpty());
        } else {
            graph->setData(QVector<double>(), QVector<double>());
            graph->setVisible(false);
        }
    }

    QVector<double> tAdcPh, vAdcPh;
    int pxADCPh = pxRFEffective;
    if (m_vecRects.size() > 2 && m_vecRects[2]) {
        pxADCPh = qMax(1, static_cast<int>(qRound(m_vecRects[2]->width() * m_mainWindow->devicePixelRatioF())));
    }
    if (currentLODLevel != LODLevel::DOWNSAMPLED) {
        pxADCPh = qMax(pxADCPh, 100000);
    }
    loader->getAdcPhaseViewport(visibleStart, visibleEnd, pxADCPh, tAdcPh, vAdcPh);
    if (m_graphADCPh) {
        m_graphADCPh->setData(tAdcPh, vAdcPh);
        m_graphADCPh->setVisible(m_curveVisibility.value(2, true) && !tAdcPh.isEmpty());
    }

    if (!m_lockYAxisRanges) {
        double minMag = std::numeric_limits<double>::infinity();
        double maxMag = -std::numeric_limits<double>::infinity();
        computeMinMax(rfViewport.ampValueByChannel, minMag, maxMag);
        if (std::isfinite(minMag) && std::isfinite(maxMag) && m_vecRects.size() > 1 && m_vecRects[1]) {
            double pad = (maxMag - minMag) * 0.05;
            if (pad == 0.0) pad = 1.0;
            m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(minMag - pad, maxMag + pad);
        }
        if (m_vecRects.size() > 2 && m_vecRects[2])
            m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(kPhaseAxisLower, kPhaseAxisUpper);
    } else {
        if (m_vecRects.size() > 1 && m_vecRects[1]) m_vecRects[1]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[1].first, m_fixedYRanges[1].second);
        if (m_vecRects.size() > 2 && m_vecRects[2]) m_vecRects[2]->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[2].first, m_fixedYRanges[2].second);
    }
}

void WaveformDrawer::clearAllWaveformData()
{
    QVector<double> emptyX;
    QVector<double> emptyY;

    auto clearGraph = [&](QCPGraph* g) {
        if (!g) return;
        g->setData(emptyX, emptyY);
        g->setVisible(false);
    };

    clearGraph(m_graphADC);
    for (QCPGraph* graph : m_graphRFMagChannels)
        clearGraph(graph);
    for (QCPGraph* graph : m_graphRFPhChannels)
        clearGraph(graph);
    clearGraph(m_graphADCPh);
    clearGraph(m_graphGx);
    clearGraph(m_graphGy);
    clearGraph(m_graphGz);
    clearGraph(m_graphTrigMarkers);
    clearGraph(m_graphTrigDurations);
    clearGraph(m_graphM1x);
    clearGraph(m_graphM1y);
    clearGraph(m_graphM1z);

    for (QCPGraph* edge : m_blockEdgeGraphs)
        clearGraph(edge);

    if (m_extensionPlotter)
        m_extensionPlotter->reset();

    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
        m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
}

void WaveformDrawer::DrawADCWaveform(const double& dStartTime, double dEndTime)
{
    QElapsedTimer adcPerfTimer;
    adcPerfTimer.start();
    qint64 adcPerfLastMs = 0;
    auto markAdcPerf = [&]() {
        const qint64 nowMs = adcPerfTimer.elapsed();
        const qint64 deltaMs = nowMs - adcPerfLastMs;
        adcPerfLastMs = nowMs;
        return deltaMs;
    };
    m_lastAdcLabelInitMs = 0;
    m_lastAdcViewportMs = 0;
    m_lastAdcHeightMs = 0;
    m_lastAdcRangeCollectMs = 0;
    m_lastAdcBuildMs = 0;
    m_lastAdcSetDataMs = 0;
    m_lastAdcExtensionMs = 0;

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getDecodedSeqBlocks().empty()) return;
    m_lastAdcLabelInitMs = markAdcPerf();

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
    m_lastAdcViewportMs = markAdcPerf();

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
    m_lastAdcHeightMs = markAdcPerf();

    QVector<double> processedTime, processedValues;

    struct AdcRange {
        double start = 0.0;
        double end = 0.0;
    };

    QVector<AdcRange> adcRanges;
    const auto& blocks = loader->getDecodedSeqBlocks();
    const QVector<double>& edges = loader->getBlockEdges();
    if (!blocks.empty() && edges.size() > 1) {
        auto itStart = std::lower_bound(edges.begin(), edges.end(), visibleStart);
        int startBlock = std::max(0, int(std::distance(edges.begin(), itStart)) - 1);
        int endBlock = std::min(int(blocks.size()) - 1, int(edges.size()) - 2);
        const double tFactor = loader->getTFactor();

        for (int i = startBlock; i <= endBlock; ++i) {
            if (edges[i] > visibleEnd) break;

            SeqBlock* blk = blocks[i];
            if (!blk || !blk->isADC()) continue;

            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples <= 0) continue;

            const double tStart = edges[i] + adc.delay * tFactor;
            const double tEnd = tStart + (adc.numSamples * adc.dwellTime / 1000.0) * tFactor;
            if (tEnd <= visibleStart || tStart >= visibleEnd) continue;

            adcRanges.append({std::max(tStart, visibleStart), std::min(tEnd, visibleEnd)});
        }
    }
    m_lastAdcRangeCollectMs = markAdcPerf();

    auto appendAdcRect = [&](double t0, double t1) {
        if (t1 <= t0) return;
        if (!processedTime.isEmpty()) {
            processedTime.append(t0);
            processedValues.append(std::numeric_limits<double>::quiet_NaN());
        }
        processedTime.append(t0); processedValues.append(0.0);
        processedTime.append(t0); processedValues.append(adcHeight);
        processedTime.append(t1); processedValues.append(adcHeight);
        processedTime.append(t1); processedValues.append(0.0);
        processedTime.append(t1); processedValues.append(std::numeric_limits<double>::quiet_NaN());
    };

    auto appendAdcMarker = [&](double t) {
        if (!processedTime.isEmpty()) {
            processedTime.append(t);
            processedValues.append(std::numeric_limits<double>::quiet_NaN());
        }
        processedTime.append(t); processedValues.append(0.0);
        processedTime.append(t); processedValues.append(adcHeight);
        processedTime.append(t); processedValues.append(std::numeric_limits<double>::quiet_NaN());
    };

    int pxADC = 0;
    if (m_vecRects.size() > 0 && m_vecRects[0]) {
        pxADC = qMax(1, static_cast<int>(qRound(m_vecRects[0]->width() * m_mainWindow->devicePixelRatioF())));
    }

    const int exactPointCount = adcRanges.size() * 5;
    const int exactPointBudget = qMax(1, pxADC * 4);
    const bool usePixelBudget = currentLODLevel == LODLevel::DOWNSAMPLED
                             && pxADC > 0
                             && exactPointCount > exactPointBudget
                             && visibleEnd > visibleStart;

    if (usePixelBudget) {
        const int bucketCount = qMax(1, pxADC);
        QVector<char> occupied(bucketCount, 0);
        const double window = visibleEnd - visibleStart;

        for (const AdcRange& range : adcRanges) {
            int b0 = static_cast<int>(std::floor((range.start - visibleStart) / window * bucketCount));
            int b1 = static_cast<int>(std::ceil((range.end - visibleStart) / window * bucketCount)) - 1;
            b0 = std::max(0, std::min(bucketCount - 1, b0));
            b1 = std::max(0, std::min(bucketCount - 1, b1));
            for (int b = b0; b <= b1; ++b) {
                occupied[b] = 1;
            }
        }

        const double bucketWidth = window / bucketCount;
        for (int b = 0; b < bucketCount; ++b) {
            if (!occupied[b]) {
                continue;
            }

            const double t = visibleStart + (static_cast<double>(b) + 0.5) * bucketWidth;
            appendAdcMarker(t);
        }
    } else {
        for (const AdcRange& range : adcRanges) {
            appendAdcRect(range.start, range.end);
        }
    }
    m_lastAdcBuildMs = markAdcPerf();
    
    // Draw ADC rectangles using persistent graph
    if (m_graphADC) {
        m_graphADC->setLineStyle(usePixelBudget ? QCPGraph::lsLine : QCPGraph::lsStepLeft);
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
    m_lastAdcSetDataMs = markAdcPerf();

    // Extension labels overlay (SLC/REP/AVG...); controlled by Settings checkboxes.
    if (m_extensionPlotter)
    {
        const bool adcHostVisible = m_curveVisibility.value(0, true);
        m_extensionPlotter->setHostVisible(adcHostVisible);

        QStringList enabledUsedLabels;
        if (adcHostVisible)
        {
            Settings& st = Settings::getInstance();
            const QSet<QString> usedExtensions = loader->getUsedExtensions();
            const QStringList labels = loader->getAvailableExtensionLabels();
            for (const QString& label : labels)
            {
                if (st.isExtensionLabelEnabled(label) && usedExtensions.contains(label.toUpper()))
                    enabledUsedLabels.append(label);
            }
        }

        m_extensionPlotter->updateForViewport(loader, visibleStart, visibleEnd, enabledUsedLabels, pxADC);

        // Ensure extension values are within the visible Y-range of the ADC/labels panel.
        // ADC y-range was originally sized for ADC rectangles only; extension counters like LIN can grow large (e.g. 63),
        // which would make the line appear "missing" even though tooltip shows the correct value.
        if (m_vecRects.size() > 0 && m_vecRects[0] && adcHostVisible && !enabledUsedLabels.isEmpty())
        {
            const int maxExt = m_extensionPlotter->lastVisibleMaxValue();

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
    m_lastAdcExtensionMs = markAdcPerf();

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

        QVector<double> tGCapped, vGCapped;
        if (applyFinalPixelBudgetEnvelope(tG, vG, visibleStart, visibleEnd, px, 8, tGCapped, vGCapped)) {
            tG = std::move(tGCapped);
            vG = std::move(vGCapped);
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

    // PNS curve (single axis with X/Y/Z/norm)
    if (m_graphPnsX && m_graphPnsY && m_graphPnsZ && m_graphPnsNorm)
    {
                const bool pnsCurveEnabled = m_curveVisibility.value(6, true);
        const Settings& settings = Settings::getInstance();
        const bool interactionFastMode = (m_mainWindow && m_mainWindow->isInteractionFastMode());
        const bool renderX = !interactionFastMode && settings.getPnsChannelVisibleX();
        const bool renderY = !interactionFastMode && settings.getPnsChannelVisibleY();
        const bool renderZ = !interactionFastMode && settings.getPnsChannelVisibleZ();
        const bool renderN = settings.getPnsChannelVisibleNorm();
        const bool anyPnsChannelEnabled = renderX || renderY || renderZ || renderN;

        // Fast path: when PNS checkbox is off (or all channels hidden), skip
        // PNS vector work only. M1 is drawn below and must not depend on PNS.
        if (!pnsCurveEnabled || !anyPnsChannelEnabled)
        {
            m_graphPnsX->setVisible(false);
            m_graphPnsY->setVisible(false);
            m_graphPnsZ->setVisible(false);
            m_graphPnsNorm->setVisible(false);
        }
        else
        {
            QVector<double> pnsT;
            QVector<double> pnsX;
            QVector<double> pnsY;
            QVector<double> pnsZ;
            QVector<double> pnsN;
            const QVector<double>& tSec = loader->getPnsTimeSec();
            const QVector<double>& sx = loader->getPnsX();
            const QVector<double>& sy = loader->getPnsY();
            const QVector<double>& sz = loader->getPnsZ();
            const QVector<double>& sn = loader->getPnsNorm();
            const int n = std::min({tSec.size(), sx.size(), sy.size(), sz.size(), sn.size()});
            const double secStart = visibleStart / (1e6 * tFactor);
            const double secEnd = visibleEnd / (1e6 * tFactor);
            int i0 = 0;
            int i1 = n;
            if (n > 0)
            {
                i0 = static_cast<int>(std::lower_bound(tSec.constBegin(), tSec.constBegin() + n, secStart) - tSec.constBegin());
                i1 = static_cast<int>(std::upper_bound(tSec.constBegin() + i0, tSec.constBegin() + n, secEnd) - tSec.constBegin());
            }
            const int visibleCount = std::max(0, i1 - i0);

            pnsT.reserve(visibleCount);
            pnsX.reserve(visibleCount);
            pnsY.reserve(visibleCount);
            pnsZ.reserve(visibleCount);
            pnsN.reserve(visibleCount);

            // Display in percent to match MATLAB safe_plot.
            for (int i = i0; i < i1; ++i)
            {
                pnsT.append(tSec[i] * 1e6 * tFactor);
                pnsX.append(100.0 * sx[i]);
                pnsY.append(100.0 * sy[i]);
                pnsZ.append(100.0 * sz[i]);
                pnsN.append(100.0 * sn[i]);
            }

            // Downsample each channel independently with min-max buckets (shape-preserving envelope).
            QVector<double> pnsTx = pnsT, pnsTy = pnsT, pnsTz = pnsT, pnsTn = pnsT;
            QVector<double> pnsVx = pnsX, pnsVy = pnsY, pnsVz = pnsZ, pnsVn = pnsN;
            if (m_pPnsRect && pnsT.size() > 0)
            {
                const int px = qMax(1, static_cast<int>(qRound(m_pPnsRect->width() * m_mainWindow->devicePixelRatioF())));
                const int maxPoints = interactionFastMode
                    ? qMax(80, px / 2)
                    : ((currentLODLevel == LODLevel::DOWNSAMPLED)
                        ? qMax(160, px)
                        : qMax(220, static_cast<int>(qRound(1.2 * px))));
                if (pnsT.size() > maxPoints)
                {
                    if (renderX) applyMinMaxDownsampling(pnsT, pnsX, maxPoints, pnsTx, pnsVx);
                    if (renderY) applyMinMaxDownsampling(pnsT, pnsY, maxPoints, pnsTy, pnsVy);
                    if (renderZ) applyMinMaxDownsampling(pnsT, pnsZ, maxPoints, pnsTz, pnsVz);
                    applyMinMaxDownsampling(pnsT, pnsN, maxPoints, pnsTn, pnsVn);
                }
            }

            if (!renderX) { pnsTx.clear(); pnsVx.clear(); }
            if (!renderY) { pnsTy.clear(); pnsVy.clear(); }
            if (!renderZ) { pnsTz.clear(); pnsVz.clear(); }

            const bool showPnsX = !pnsTx.isEmpty();
            const bool showPnsY = !pnsTy.isEmpty();
            const bool showPnsZ = !pnsTz.isEmpty();
            const bool showPnsN = renderN && !pnsTn.isEmpty();
            m_graphPnsX->setData(pnsTx, pnsVx);
            m_graphPnsY->setData(pnsTy, pnsVy);
            m_graphPnsZ->setData(pnsTz, pnsVz);
            m_graphPnsNorm->setData(pnsTn, pnsVn);
            m_graphPnsX->setVisible(showPnsX);
            m_graphPnsY->setVisible(showPnsY);
            m_graphPnsZ->setVisible(showPnsZ);
            m_graphPnsNorm->setVisible(showPnsN);

            if (!m_lockYAxisRanges)
            {
                double yMax = 120.0;
                for (double v : pnsVn)
                {
                    if (std::isfinite(v))
                    {
                        yMax = std::max(yMax, v);
                    }
                }
                if (m_pPnsRect)
                {
                    m_pPnsRect->axis(QCPAxis::atLeft)->setRange(0.0, yMax * 1.05);
                }
            }
            else if (m_pPnsRect && m_fixedYRanges.size() > 6)
            {
                m_pPnsRect->axis(QCPAxis::atLeft)->setRange(m_fixedYRanges[6].first, m_fixedYRanges[6].second);
            }
        }
    }

    // M1 (first gradient moment) per-axis curves. Mirrors the PNS pattern:
    // data lives in PulseqLoader::m_m1Result, here we slice the visible viewport
    // and min-max downsample to the rect width so very long sequences stay
    // smooth on pan/zoom.
    if (m_graphM1x && m_graphM1y && m_graphM1z)
    {
        const bool m1xEnabled = m_curveVisibility.value(7, false);
        const bool m1yEnabled = m_curveVisibility.value(8, false);
        const bool m1zEnabled = m_curveVisibility.value(9, false);
        const bool anyM1Enabled = m1xEnabled || m1yEnabled || m1zEnabled;
        const bool interactionFastMode = (m_mainWindow && m_mainWindow->isInteractionFastMode());

        // Fast path: when all M1 toggles are off, hide graphs and skip all vector work.
        if (!anyM1Enabled || interactionFastMode)
        {
            // During fast mode we still respect the user toggles — curves that
            // were on stay hidden until interaction finishes, then the next
            // DrawGWaveform pass restores them.
            m_graphM1x->setVisible(false);
            m_graphM1y->setVisible(false);
            m_graphM1z->setVisible(false);
        }
        else
        {
            const QVector<double>& tSec = loader->getM1TimeSec();
            const QVector<double>& sx = loader->getM1X();
            const QVector<double>& sy = loader->getM1Y();
            const QVector<double>& sz = loader->getM1Z();
            const int n = std::min({tSec.size(), sx.size(), sy.size(), sz.size()});
            const double secStart = visibleStart / (1e6 * tFactor);
            const double secEnd = visibleEnd / (1e6 * tFactor);
            int i0 = 0;
            int i1 = n;
            if (n > 0)
            {
                i0 = static_cast<int>(std::lower_bound(tSec.constBegin(), tSec.constBegin() + n, secStart) - tSec.constBegin());
                i1 = static_cast<int>(std::upper_bound(tSec.constBegin() + i0, tSec.constBegin() + n, secEnd) - tSec.constBegin());
            }
            const int visibleCount = std::max(0, i1 - i0);

            QVector<double> m1T;
            QVector<double> m1Vx, m1Vy, m1Vz;
            m1T.reserve(visibleCount);
            m1Vx.reserve(visibleCount);
            m1Vy.reserve(visibleCount);
            m1Vz.reserve(visibleCount);
            for (int i = i0; i < i1; ++i)
            {
                m1T.append(tSec[i] * 1e6 * tFactor);
                m1Vx.append(sx[i]);
                m1Vy.append(sy[i]);
                m1Vz.append(sz[i]);
            }

            // Min-max downsample each axis independently to the rect width, using
            // the same helper the PNS path uses. Keeps envelope (zero-crossings
            // and slopes visible) while capping point count to the visible pixels.
            QVector<double> m1Tx = m1T, m1Ty = m1T, m1Tz = m1T;
            QVector<double> m1VxDs = m1Vx, m1VyDs = m1Vy, m1VzDs = m1Vz;
            if (m1T.size() > 0)
            {
                auto rectPixelWidth = [&](QCPAxisRect* rect) {
                    if (!rect || !m_mainWindow) return 1;
                    return qMax(1, static_cast<int>(qRound(rect->width() * m_mainWindow->devicePixelRatioF())));
                };
                const int pxX = rectPixelWidth(m_pM1xRect);
                const int pxY = rectPixelWidth(m_pM1yRect);
                const int pxZ = rectPixelWidth(m_pM1zRect);
                const int targetX = qMax(160, pxX);
                const int targetY = qMax(160, pxY);
                const int targetZ = qMax(160, pxZ);
                if (m1T.size() > targetX) applyMinMaxDownsampling(m1T, m1Vx, targetX, m1Tx, m1VxDs);
                if (m1T.size() > targetY) applyMinMaxDownsampling(m1T, m1Vy, targetY, m1Ty, m1VyDs);
                if (m1T.size() > targetZ) applyMinMaxDownsampling(m1T, m1Vz, targetZ, m1Tz, m1VzDs);
            }

            m_graphM1x->setData(m1Tx, m1VxDs);
            m_graphM1y->setData(m1Ty, m1VyDs);
            m_graphM1z->setData(m1Tz, m1VzDs);
            m_graphM1x->setVisible(m1xEnabled);
            m_graphM1y->setVisible(m1yEnabled);
            m_graphM1z->setVisible(m1zEnabled);

            // Auto Y-range when not locked. Use the visible sample max so the
            // chosen viewport drives the scale, just like PNS does.
            if (!m_lockYAxisRanges)
            {
                auto computeBounds = [&](const QVector<double>& vals, double& lo, double& hi) {
                    lo = 0.0;
                    hi = 0.0;
                    bool any = false;
                    for (double v : vals)
                    {
                        if (std::isfinite(v))
                        {
                            if (!any) { lo = v; hi = v; any = true; }
                            else { lo = std::min(lo, v); hi = std::max(hi, v); }
                        }
                    }
                    // Guard against a zero / inverted range, which QCustomPlot
                    // treats as "no data" and refuses to draw. If bounds are
                    // identical (e.g. only reset events in viewport, or empty
                    // samples), fall back to a unit interval centered on the
                    // single value (or 0..1 when nothing came through).
                    if (!any || lo >= hi)
                    {
                        const double center = any ? lo : 0.0;
                        lo = center - 0.5;
                        hi = center + 0.5;
                    }
                    // Add ~5% headroom so the curve does not sit on the axis
                    // edge. Mirrors the small padding used by the PNS block.
                    const double pad = 0.05 * (hi - lo);
                    lo -= pad;
                    hi += pad;
                };
                double loX = 0.0, hiX = 0.0, loY = 0.0, hiY = 0.0, loZ = 0.0, hiZ = 0.0;
                if (m1xEnabled) computeBounds(m1VxDs, loX, hiX);
                if (m1yEnabled) computeBounds(m1VyDs, loY, hiY);
                if (m1zEnabled) computeBounds(m1VzDs, loZ, hiZ);
                if (m_pM1xRect && m1xEnabled)
                    m_pM1xRect->axis(QCPAxis::atLeft)->setRange(loX, hiX);
                if (m_pM1yRect && m1yEnabled)
                    m_pM1yRect->axis(QCPAxis::atLeft)->setRange(loY, hiY);
                if (m_pM1zRect && m1zEnabled)
                    m_pM1zRect->axis(QCPAxis::atLeft)->setRange(loZ, hiZ);
            }
            else if (m_fixedYRanges.size() > 9)
            {
                auto applyFixed = [&](QCPAxisRect* rect, bool enabled, int slot) {
                    if (!rect || !enabled) return;
                    const double lo = m_fixedYRanges[slot].first;
                    const double hi = m_fixedYRanges[slot].second;
                    // Only apply the locked range if it's well-formed; an
                    // uninitialized (0,0) entry would otherwise squash the axis.
                    if (std::isfinite(lo) && std::isfinite(hi) && hi > lo)
                    {
                        rect->axis(QCPAxis::atLeft)->setRange(lo, hi);
                    }
                };
                applyFixed(m_pM1xRect, m1xEnabled, 7);
                applyFixed(m_pM1yRect, m1yEnabled, 8);
                applyFixed(m_pM1zRect, m1zEnabled, 9);
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
        for (double v : vals) { if (!std::isfinite(v)) continue; mn = std::min(mn, v); mx = std::max(mx, v);}
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
        double padA = (rAmp.second - rAmp.first) * 0.05; if (padA == 0) padA = 1.0;
        m_fixedYRanges[1] = qMakePair(rAmp.first - padA, rAmp.second + padA);
        m_fixedYRanges[2] = qMakePair(kPhaseAxisLower, kPhaseAxisUpper);
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

    // 6: PNS
    {
        double pMax = 120.0;
        for (double v : loader->getPnsNorm())
        {
            if (std::isfinite(v))
            {
                pMax = std::max(pMax, 100.0 * v);
            }
        }
        m_fixedYRanges[6] = qMakePair(0.0, pMax * 1.05);
    }

    // 7/8/9: M1x/M1y/M1z. These ranges may become available after the initial
    // sequence draw because M1 is computed asynchronously.
    if (m_fixedYRanges.size() > 9)
    {
        m_fixedYRanges[7] = computeRange(loader->getM1X());
        m_fixedYRanges[8] = computeRange(loader->getM1Y());
        m_fixedYRanges[9] = computeRange(loader->getM1Z());
    }

    // Apply ranges now
    for (int i = 0; i < m_vecRects.size() && i < m_fixedYRanges.size(); ++i) {
        if (m_vecRects[i]) {
            const double lo = m_fixedYRanges[i].first;
            const double hi = m_fixedYRanges[i].second;
            if (std::isfinite(lo) && std::isfinite(hi) && hi > lo)
                m_vecRects[i]->axis(QCPAxis::atLeft)->setRange(lo, hi);
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
    auto safeTake = [&](QCPAxisRect* rect) {
        if (!rect) return;
        if (rect->layout() != nullptr)
            customPlot->plotLayout()->take(rect);
    };
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
        if (rect == m_pPnsRect) return 6;
        if (rect == m_pM1xRect) return 7;
        if (rect == m_pM1yRect) return 8;
        if (rect == m_pM1zRect) return 9;
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
    labelToRect["PNS"] = m_pPnsRect;
    labelToRect["M1x"] = m_pM1xRect;
    labelToRect["M1y"] = m_pM1yRect;
    labelToRect["M1z"] = m_pM1zRect;
    
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
            safeTake(rect);
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
            safeTake(rect);
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
    for (QCPGraph* graph : m_graphRFMagChannels)
        if (graph)
            graph->setVisible(m_curveVisibility.value(1, false));
    for (QCPGraph* graph : m_graphRFPhChannels)
        if (graph)
            graph->setVisible(m_curveVisibility.value(2, false));
    if (m_graphGx)
        m_graphGx->setVisible(m_curveVisibility.value(3, false));
    if (m_graphGy)
        m_graphGy->setVisible(m_curveVisibility.value(4, false));
    if (m_graphGz)
        m_graphGz->setVisible(m_curveVisibility.value(5, false));
    if (m_graphPnsX)
        m_graphPnsX->setVisible(m_curveVisibility.value(6, false) && Settings::getInstance().getPnsChannelVisibleX());
    if (m_graphPnsY)
        m_graphPnsY->setVisible(m_curveVisibility.value(6, false) && Settings::getInstance().getPnsChannelVisibleY());
    if (m_graphPnsZ)
        m_graphPnsZ->setVisible(m_curveVisibility.value(6, false) && Settings::getInstance().getPnsChannelVisibleZ());
    if (m_graphPnsNorm)
        m_graphPnsNorm->setVisible(m_curveVisibility.value(6, false) && Settings::getInstance().getPnsChannelVisibleNorm());
    if (m_graphM1x)
        m_graphM1x->setVisible(m_curveVisibility.value(7, false));
    if (m_graphM1y)
        m_graphM1y->setVisible(m_curveVisibility.value(8, false));
    if (m_graphM1z)
        m_graphM1z->setVisible(m_curveVisibility.value(9, false));
    if (m_graphTrigMarkers)
        m_graphTrigMarkers->setVisible(m_curveVisibility.value(0, false));
    if (m_graphTrigDurations)
        m_graphTrigDurations->setVisible(m_curveVisibility.value(0, false));

    // Keep extension overlay in sync with ADC axis visibility.
    if (m_extensionPlotter)
        m_extensionPlotter->setHostVisible(m_curveVisibility.value(0, true));

    // Ensure x-axis labels are on the bottom-most visible rect after layout/visibility changes.
    configureXAxisLabels();
    rebindVerticalLinesToRects();

    // Replot to apply both layout and visibility changes
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
}

void WaveformDrawer::setPnsInteractionFastVisibility(bool enabled)
{
    const bool pnsEnabled = m_curveVisibility.value(6, false);
    const Settings& s = Settings::getInstance();
    const bool showNorm = pnsEnabled && s.getPnsChannelVisibleNorm();
    const bool showX = !enabled && pnsEnabled && s.getPnsChannelVisibleX();
    const bool showY = !enabled && pnsEnabled && s.getPnsChannelVisibleY();
    const bool showZ = !enabled && pnsEnabled && s.getPnsChannelVisibleZ();

    if (m_graphPnsX) m_graphPnsX->setVisible(showX);
    if (m_graphPnsY) m_graphPnsY->setVisible(showY);
    if (m_graphPnsZ) m_graphPnsZ->setVisible(showZ);
    if (m_graphPnsNorm) m_graphPnsNorm->setVisible(showNorm);

    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
    {
        m_mainWindow->requestReplot(QCustomPlot::rpImmediateRefresh, "unknown", "");
    }
}

void WaveformDrawer::setM1InteractionFastVisibility(bool enabled)
{
    // Mirrors setPnsInteractionFastVisibility: when fast mode is on, hide the
    // per-axis M1 curves so pan/zoom interactions stay responsive. The next
    // DrawGWaveform() pass (after setInteractionFastMode(false)) restores
    // them with the correct viewport-aware downsampling.
    const bool m1xEnabled = m_curveVisibility.value(7, false);
    const bool m1yEnabled = m_curveVisibility.value(8, false);
    const bool m1zEnabled = m_curveVisibility.value(9, false);

    if (m_graphM1x) m_graphM1x->setVisible(!enabled && m1xEnabled);
    if (m_graphM1y) m_graphM1y->setVisible(!enabled && m1yEnabled);
    if (m_graphM1z) m_graphM1z->setVisible(!enabled && m1zEnabled);

    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
    {
        m_mainWindow->requestReplot(QCustomPlot::rpImmediateRefresh, "unknown", "");
    }
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

    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
}

WaveformDrawer::RenderStats WaveformDrawer::ensureRenderedForCurrentViewport()
{
    RenderStats stats;
    try {
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        if (!loader || !loader->canRenderSequence()) {
            if (DEBUG_LOD_SYSTEM) {
                qDebug().noquote() << "[LOD] Sequence render skipped:"
                                   << (loader ? "loader not ready" : "no loader");
            }
            return stats;
        }
        QElapsedTimer stageTimer;
        stageTimer.start();
        
        qint64 rfTime = 0, adcTime = 0, gradTime = 0, trigTime = 0, edgeTime = 0;
        int rfPoints = 0, adcPoints = 0, gradPoints = 0, trigPoints = 0, edgePoints = 0;
        int rfMagPoints = 0, rfPhasePoints = 0;
        int rfMagChannels = 0, rfPhaseChannels = 0;
        int rfMagMaxPoints = 0, rfPhaseMaxPoints = 0;

        // Redraw visible content for all channels based on the current viewport
        stageTimer.restart();
        DrawRFWaveform();
        rfTime = stageTimer.restart();
        auto countGraphPoints = [](QCPGraph* graph, int& total, int& channels, int& maxPoints) {
            const int points = (graph && graph->data()) ? graph->data()->size() : 0;
            if (points <= 0) {
                return;
            }
            total += points;
            ++channels;
            maxPoints = std::max(maxPoints, points);
        };
        countGraphPoints(m_graphRFMag, rfMagPoints, rfMagChannels, rfMagMaxPoints);
        countGraphPoints(m_graphRFPh, rfPhasePoints, rfPhaseChannels, rfPhaseMaxPoints);
        for (auto* g : m_graphRFMagChannels) countGraphPoints(g, rfMagPoints, rfMagChannels, rfMagMaxPoints);
        for (auto* g : m_graphRFPhChannels) countGraphPoints(g, rfPhasePoints, rfPhaseChannels, rfPhaseMaxPoints);
        rfPoints = rfMagPoints + rfPhasePoints;

        DrawADCWaveform();
        adcTime = stageTimer.restart();
        int adcRectPoints = (m_graphADC && m_graphADC->data() ? m_graphADC->data()->size() : 0);
        int adcPhasePoints = (m_graphADCPh && m_graphADCPh->data() ? m_graphADCPh->data()->size() : 0);
        adcPoints += adcRectPoints;
        adcPoints += adcPhasePoints;

        DrawGWaveform();
        gradTime = stageTimer.restart();
        gradPoints += (m_graphGx && m_graphGx->data() ? m_graphGx->data()->size() : 0);
        gradPoints += (m_graphGy && m_graphGy->data() ? m_graphGy->data()->size() : 0);
        gradPoints += (m_graphGz && m_graphGz->data() ? m_graphGz->data()->size() : 0);
        gradPoints += (m_graphPnsNorm && m_graphPnsNorm->data() ? m_graphPnsNorm->data()->size() : 0);
        gradPoints += (m_graphM1x && m_graphM1x->data() ? m_graphM1x->data()->size() : 0);

        DrawTriggerOverlay();
        trigTime = stageTimer.restart();
        trigPoints += (m_graphTrigMarkers && m_graphTrigMarkers->data() ? m_graphTrigMarkers->data()->size() : 0);

        if (getShowBlockEdges()) {
            DrawBlockEdges();
            edgeTime = stageTimer.restart();
            for (auto* g : m_blockEdgeGraphs) edgePoints += (g && g->data() ? g->data()->size() : 0);
        }
        
        // Calculate visible blocks early to pass to stats
        int visibleBlocks = 0;
        QCPRange range;
        if (!m_vecRects.isEmpty() && m_vecRects[0]) {
            range = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
            auto edges = m_mainWindow->getPulseqLoader()->getBlockEdges();
            auto itStart = std::lower_bound(edges.begin(), edges.end(), range.lower);
            auto itEnd = std::upper_bound(edges.begin(), edges.end(), range.upper);
            visibleBlocks = std::distance(itStart, itEnd);
        }
        int totalPoints = rfPoints + adcPoints + gradPoints + trigPoints + edgePoints;
        qint64 totalTimeMs = rfTime + adcTime + gradTime + trigTime + edgeTime;
        
        // Find slowest stage
        qint64 maxTime = rfTime;
        QString slowestStage = "DrawRFWaveform";
        if (adcTime > maxTime) { maxTime = adcTime; slowestStage = "DrawADCWaveform"; }
        if (gradTime > maxTime) { maxTime = gradTime; slowestStage = "DrawGWaveform"; }
        if (trigTime > maxTime) { maxTime = trigTime; slowestStage = "DrawTriggerOverlay"; }
        if (edgeTime > maxTime) { maxTime = edgeTime; slowestStage = "DrawBlockEdges"; }

        if (m_mainWindow->isInitialLoadPerfActive()) {
            m_mainWindow->recordInitialLoadRenderStats(totalTimeMs, visibleBlocks, totalPoints, slowestStage,
                                                       rfPoints, rfMagPoints, rfPhasePoints,
                                                       rfMagChannels, rfPhaseChannels,
                                                       rfMagMaxPoints, rfPhaseMaxPoints,
                                                       adcRectPoints, adcPhasePoints,
                                                       gradPoints, trigPoints, edgePoints,
                                                       rfTime, adcTime, gradTime, trigTime, edgeTime,
                                                       m_lastAdcLabelInitMs, m_lastAdcViewportMs,
                                                       m_lastAdcHeightMs, m_lastAdcRangeCollectMs,
                                                       m_lastAdcBuildMs, m_lastAdcSetDataMs,
                                                       m_lastAdcExtensionMs);
            m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "initial-load", m_mainWindow->currentInitialLoadTraceId());
        } else {
            QString traceId = "";
            if (m_mainWindow->getInteractionHandler()) {
                traceId = m_mainWindow->getInteractionHandler()->currentInteractionTraceId();
            }
            m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "deferred-render", traceId);
        }
        
        stats.totalTimeMs = totalTimeMs;
        stats.visibleBlocks = visibleBlocks;
        stats.totalPoints = totalPoints;
        stats.rfPoints = rfPoints;
        stats.rfMagPoints = rfMagPoints;
        stats.rfPhasePoints = rfPhasePoints;
        stats.rfMagChannels = rfMagChannels;
        stats.rfPhaseChannels = rfPhaseChannels;
        stats.rfMagMaxPoints = rfMagMaxPoints;
        stats.rfPhaseMaxPoints = rfPhaseMaxPoints;
        stats.adcRectPoints = adcRectPoints;
        stats.adcPhasePoints = adcPhasePoints;
        stats.gradPoints = gradPoints;
        stats.trigPoints = trigPoints;
        stats.edgePoints = edgePoints;
        stats.rfTimeMs = rfTime;
        stats.adcTimeMs = adcTime;
        stats.gradTimeMs = gradTime;
        stats.trigTimeMs = trigTime;
        stats.edgeTimeMs = edgeTime;
        stats.adcLabelInitMs = m_lastAdcLabelInitMs;
        stats.adcViewportMs = m_lastAdcViewportMs;
        stats.adcHeightMs = m_lastAdcHeightMs;
        stats.adcRangeCollectMs = m_lastAdcRangeCollectMs;
        stats.adcBuildMs = m_lastAdcBuildMs;
        stats.adcSetDataMs = m_lastAdcSetDataMs;
        stats.adcExtensionMs = m_lastAdcExtensionMs;
        stats.slowestStage = slowestStage;
        
        QString lodStr = (getCurrentLODLevel() == LODLevel::DOWNSAMPLED) ? QStringLiteral("Downsampled") : QStringLiteral("Full");
        
        if (Settings::getInstance().getPerformanceDebugEnabled()) {
            LOG_DEBUG_CAT("Performance", QStringLiteral("Render Viewport [%1, %2] LOD=%3 VisBlocks=%4 TotalPts=%5 (RF:%6ms/%7pts mag=%8 phase=%9 magCh=%10 phaseCh=%11 magMax=%12 phaseMax=%13, ADC:%14ms/%15pts rect=%16 phase=%17 [labelInit=%18 viewport=%19 height=%20 ranges=%21 build=%22 setData=%23 ext=%24], G:%25ms/%26pts, Trig:%27ms/%28pts, Edge:%29ms/%30pts)")
                .arg(range.lower).arg(range.upper).arg(lodStr).arg(visibleBlocks).arg(totalPoints)
                .arg(rfTime).arg(rfPoints).arg(rfMagPoints).arg(rfPhasePoints)
                .arg(rfMagChannels).arg(rfPhaseChannels).arg(rfMagMaxPoints).arg(rfPhaseMaxPoints)
                .arg(adcTime).arg(adcPoints).arg(adcRectPoints).arg(adcPhasePoints)
                .arg(m_lastAdcLabelInitMs).arg(m_lastAdcViewportMs).arg(m_lastAdcHeightMs)
                .arg(m_lastAdcRangeCollectMs).arg(m_lastAdcBuildMs).arg(m_lastAdcSetDataMs).arg(m_lastAdcExtensionMs)
                .arg(gradTime).arg(gradPoints)
                .arg(trigTime).arg(trigPoints)
                .arg(edgeTime).arg(edgePoints));
        }
    } catch (const std::exception& e) {
        if (DEBUG_LOD_SYSTEM) {
            qDebug().noquote() << "[LOD] Exception in ensureRenderedForCurrentViewport:" << e.what();
        }
    } catch (...) {
        if (DEBUG_LOD_SYSTEM) {
            qDebug().noquote() << "[LOD] Unknown exception in ensureRenderedForCurrentViewport";
        }
    }
    // Store and return stats
    m_lastRenderStats = stats;
    return stats;
}

void WaveformDrawer::updateAxisLabels()
{
    // Update Y-axis labels using each rect's fixed identity (matching InitSequenceFigure).
    // Do NOT use m_axesOrder indices - m_axesOrder is the visual order which differs
    // from m_vecRects order when the user has reordered subplots.
    Settings& settings = Settings::getInstance();
    QString gradientUnit = settings.getGradientUnitString();

    if (m_pADCLabelsRect)   m_pADCLabelsRect->axis(QCPAxis::atLeft)->setLabel("ADC/labels");
    if (m_pRfMagRect)       m_pRfMagRect->axis(QCPAxis::atLeft)->setLabel("RF mag(Hz)");
    if (m_pRfADCPhaseRect)  m_pRfADCPhaseRect->axis(QCPAxis::atLeft)->setLabel("RF/ADC ph(rad)");
    if (m_pGxRect)          m_pGxRect->axis(QCPAxis::atLeft)->setLabel("GX (" + gradientUnit + ")");
    if (m_pGyRect)          m_pGyRect->axis(QCPAxis::atLeft)->setLabel("GY (" + gradientUnit + ")");
    if (m_pGzRect)          m_pGzRect->axis(QCPAxis::atLeft)->setLabel("GZ (" + gradientUnit + ")");
    if (m_pPnsRect)         m_pPnsRect->axis(QCPAxis::atLeft)->setLabel("PNS (%)");

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
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
}

// Simple LOD system - no complex decision logic needed
void WaveformDrawer::setUseDownsampling(bool useDownsampling)
{
    m_useDownsampling = useDownsampling;
    
    // Trigger replot to apply new LOD level
    m_mainWindow->requestReplot(QCustomPlot::rpRefreshHint, "unknown", "");
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
            m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
        return;
    }

    if (!m_vecRects.isEmpty() && m_vecRects[0])
    {
        const QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
        updateTeGuides(viewport.lower, viewport.upper);
        updateKxKyZeroGuides(viewport.lower, viewport.upper);
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
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
            m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
        return;
    }

    if (!m_vecRects.isEmpty() && m_vecRects[0])
    {
        const QCPRange viewport = m_vecRects[0]->axis(QCPAxis::atBottom)->range();
        updateKxKyZeroGuides(viewport.lower, viewport.upper);
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->requestReplot(QCustomPlot::rpQueuedReplot, "unknown", "");
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

bool WaveformDrawer::applyFinalPixelBudgetEnvelope(const QVector<double>& time,
                                                   const QVector<double>& values,
                                                   double visibleStart,
                                                   double visibleEnd,
                                                   int pixelWidth,
                                                   int pointsPerPixel,
                                                   QVector<double>& outTime,
                                                   QVector<double>& outValues) const
{
    outTime.clear();
    outValues.clear();
    if (time.isEmpty() || values.isEmpty() || time.size() != values.size())
        return false;
    if (pixelWidth <= 0 || pointsPerPixel <= 0 || visibleEnd <= visibleStart)
        return false;

    const int pointBudget = pixelWidth * pointsPerPixel;
    if (time.size() <= pointBudget)
        return false;

    struct Bucket {
        bool used = false;
        double minValue = std::numeric_limits<double>::infinity();
        double maxValue = -std::numeric_limits<double>::infinity();
    };

    const int bucketCount = qMax(1, pixelWidth);
    QVector<Bucket> buckets(bucketCount);
    const double window = visibleEnd - visibleStart;

    for (int i = 0; i < time.size(); ++i) {
        const double t = time[i];
        const double v = values[i];
        if (!std::isfinite(t) || !std::isfinite(v))
            continue;
        if (t < visibleStart || t > visibleEnd)
            continue;

        int bucketIndex = static_cast<int>(std::floor((t - visibleStart) / window * bucketCount));
        bucketIndex = std::max(0, std::min(bucketCount - 1, bucketIndex));

        Bucket& bucket = buckets[bucketIndex];
        bucket.used = true;
        bucket.minValue = std::min(bucket.minValue, v);
        bucket.maxValue = std::max(bucket.maxValue, v);
    }

    outTime.reserve(bucketCount * 3);
    outValues.reserve(bucketCount * 3);
    const double bucketWidth = window / bucketCount;
    for (int b = 0; b < bucketCount; ++b) {
        const Bucket& bucket = buckets[b];
        if (!bucket.used)
            continue;

        const double x = visibleStart + (static_cast<double>(b) + 0.5) * bucketWidth;
        outTime.append(x);
        outValues.append(bucket.minValue);
        outTime.append(x);
        outValues.append(bucket.maxValue);
        outTime.append(x);
        outValues.append(std::numeric_limits<double>::quiet_NaN());
    }

    return !outTime.isEmpty();
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
    const auto& teOffsets = loader->getTeDurationsAxis();
    const qsizetype teCount = teOffsets.isEmpty() ? 1 : teOffsets.size();

    QVector<double> visibleCenters;
    visibleCenters.reserve(centers.size());
    QVector<double> visibleTePositions;
    visibleTePositions.reserve(centers.size() * teCount);
    for (double c : centers)
    {
        if (c >= visibleStart && c <= visibleEnd)
            visibleCenters.append(c);
        if (teOffsets.isEmpty())
        {
            double tePos = c + loader->getTeDurationAxis();
            if (tePos >= visibleStart && tePos <= visibleEnd)
                visibleTePositions.append(tePos);
        }
        else
        {
            for (double teOffset : teOffsets)
            {
                double tePos = c + teOffset;
                if (tePos >= visibleStart && tePos <= visibleEnd)
                    visibleTePositions.append(tePos);
            }
        }
    }

    if (!m_mainWindow || !m_mainWindow->ui)
        return;
    QCustomPlot* plot = m_mainWindow->ui->customPlot;

    auto syncPool = [&](QVector<QVector<QCPItemStraightLine*>>& pool,
                        const QVector<double>& positions,
                        const QColor& color,
                        Qt::PenStyle style,
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
                QPen pen(color);
                pen.setStyle(style);
                pen.setWidthF(penWidth);
                pen.setCapStyle(Qt::FlatCap);
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
                QPen pen(color);
                pen.setStyle(style);
                pen.setWidthF(penWidth);
                pen.setCapStyle(Qt::FlatCap);
                line->setPen(pen);
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

    syncPool(m_excitationGuideLines, visibleCenters, QColor(0, 0, 0, 190), Qt::DashLine, 1.0);
    syncPool(m_teEchoGuideLines, visibleTePositions, QColor(0, 0, 0, 220), Qt::DashLine, 1.1);
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

    // Use sparse blue dots so overlapping TE/excitation dashed guides remain visible in the gaps.
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
            QPen pen(QColor(0, 90, 255, 230)); // High-contrast blue
            pen.setStyle(Qt::CustomDashLine);
            pen.setDashPattern({0.8, 5.0});
            pen.setWidthF(2.2);
            pen.setCapStyle(Qt::RoundCap);
            line->setPen(pen);
            line->setLayer("overlay");
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
            QPen pen(QColor(0, 90, 255, 230));
            pen.setStyle(Qt::CustomDashLine);
            pen.setDashPattern({0.8, 5.0});
            pen.setWidthF(2.2);
            pen.setCapStyle(Qt::RoundCap);
            line->setPen(pen);
            line->setLayer("overlay");
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
