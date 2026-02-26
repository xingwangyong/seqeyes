#ifndef WAVEFORMDRAWER_H
#define WAVEFORMDRAWER_H

#include <QObject>
#include <QVector>
#include <QColor>
#include <QHash>
#include <QDateTime>
#include <QString>
#include <QTimer>
#include <memory>

class ExtensionPlotter;

// Forward declarations
class MainWindow;
class QCPAxisRect;
class QCPItemTracer;
class QCPItemStraightLine;
class QCPGraph;
class QCPMarginGroup;
class QCPItemText;
class Settings;
class ZoomManager;
namespace QCP { class Range; }

class WaveformDrawer : public QObject
{
    Q_OBJECT

public:
    explicit WaveformDrawer(MainWindow* mainWindow);
    ~WaveformDrawer();

    // Public methods for setup and control
    void InitSequenceFigure();
    void InitTracers();

    // Getters for objects needed by other handlers
    const QVector<QCPAxisRect*>& getRects() const { return m_vecRects; }
    const QVector<QCPItemStraightLine*>& getVerticalLines() const { return m_vecVerticalLine; }
    QCPAxisRect* getGzRect() const { return m_pGzRect; }
    bool getShowBlockEdges() const { return bShowBlocksEdges; }
    void setShowBlockEdges(bool show) { bShowBlocksEdges = show; }
    
    // Curve visibility control
    void setShowCurve(int curveIndex, bool show);
    void updateCurveVisibility();
    void setAutoExpandMode(bool autoExpand);
    bool getAutoExpandMode() const;
    // Programmatic layout control (rows x cols, using current axes order). Currently cols must be 1.
    void applySubplotLayout(int rows, int cols, int index);
    // Optional: expose label channel visibility map in future
    
    // Update axis labels when settings change
    void updateAxisLabels();
    
    // Ensure current viewport has been rendered at the correct detail
    void ensureRenderedForCurrentViewport();
    
    // Simple viewport change processing
    void processViewportChangeSimple(double visibleStart, double visibleEnd);

    // Axis reordering API
    const QStringList& getAxesOrder() const { return m_axesOrder; }
    void setAxesOrder(const QStringList& order); // reorders UI accordingly
    int axisIndexAtPositionY(int yInPlot) const; // hit-test by Y to nearest axis rect
    int axisCenterY(int index) const; // center Y of rect in widget coords
    void swapAxes(int i, int j); // swap two axes (visual order)
    void moveAxis(int fromIndex, int toIndex); // move axis to target position (insert-before semantics)
    void showDropIndicatorAt(int index);
    void clearDropIndicator();
    QString defaultLabelForRect(int index) const;

    // Drag ghost visual
    void startAxisDragVisual(int sourceIndex, const QPoint& startPos);
    void updateAxisDragVisual(int yInPlot);
    void finishAxisDragVisual();

    // Persistence
    void loadUiConfig();
    void saveUiConfig() const;
    
    // X-axis label configuration
    void configureXAxisLabels();
    void configureXAxisLabelsAfterReorder();
    QString currentTimeAxisLabel() const;
    void applyTimeAxisFormatting(class QCPAxis* axis) const;
    ZoomManager* getZoomManager() const { return m_zoomManager; }
    void setZoomManager(ZoomManager* zm) { m_zoomManager = zm; }

    // Compute and lock Y-axis ranges for all channels based on full sequence data.
    // Rationale: keep axes consistent across TR toggles and pan/zoom to avoid visual jitter.
    void computeAndLockYAxisRanges();
    void unlockYAxisRanges() { m_lockYAxisRanges = false; }

    // Rescale all time-dependent cached state by the given ratio (used by time unit changes).
    void rescaleTimeCachedState(double ratio);

    // Simple two-level LOD system
    enum class LODLevel {
        DOWNSAMPLED,    // Default: LTTB downsampling for performance
        FULL_DETAIL     // User choice: Full detail rendering (no downsampling)
    };
    
    // UI control for LOD level - default to downsampling mode
    bool m_useDownsampling { true };  // Default: use LTTB downsampling
    
    // Public interface for LOD control
    void setUseDownsampling(bool useDownsampling);
    bool getUseDownsampling() const { return m_useDownsampling; }
    LODLevel getCurrentLODLevel() const;
    void setShowTeGuides(bool show);
    void setShowKxKyZeroGuides(bool show);

public slots:
    void ResetView();
    void DrawRFWaveform(const double& dStartTime = 0, double dEndTime = -1);
    void DrawADCWaveform(const double& dStartTime = 0, double dEndTime = -1);
    void DrawGWaveform(const double& dStartTime = 0, double dEndTime = -1);
    void DrawBlockEdges();
    void DrawTriggerOverlay();

private:
    // Old time-based LOD functions removed - replaced with complexity-based LOD system

private:
    MainWindow* m_mainWindow;

    // Member variables moved from MainWindow
    // Plot rects and items
    QVector<QCPAxisRect*> m_vecRects; // 0: ADC label, 1: RF Mag, 2: RF ADC phase, 3: Gx, 4: Gy, 5: Gz
    QVector<QCPItemTracer*> m_vecTracers;
    QVector<QCPItemStraightLine*> m_vecVerticalLine;
    QCPAxisRect* m_pADCLabelsRect;
    QCPAxisRect* m_pRfMagRect;
    QCPAxisRect* m_pRfADCPhaseRect;
    QCPAxisRect* m_pGxRect;
    QCPAxisRect* m_pGyRect;
    QCPAxisRect* m_pGzRect;
    // Persistent graphs to avoid flicker on redraws
    QCPGraph* m_graphADC {nullptr};
    QCPGraph* m_graphRFMag {nullptr};
    QCPGraph* m_graphRFPh {nullptr};
    QCPGraph* m_graphGx {nullptr};
    QCPGraph* m_graphGy {nullptr};
    QCPGraph* m_graphGz {nullptr};

    // ADC custom phase graph (scatter dots only)
    QCPGraph* m_graphADCPh {nullptr};
    // Trigger overlay graphs on ADC/labels rect
    QCPGraph* m_graphTrigMarkers {nullptr};
    QCPGraph* m_graphTrigDurations {nullptr};
    // One persistent block-edge graph per axis rect (0..5), draws vertical lines with NaN breaks
    QVector<QCPGraph*> m_blockEdgeGraphs;
    QVector<QVector<QCPItemStraightLine*>> m_excitationGuideLines;
    QVector<QVector<QCPItemStraightLine*>> m_teEchoGuideLines;
    QVector<QVector<QCPItemStraightLine*>> m_kxKyZeroGuideLines;
    bool m_showTeGuides {false};
    bool m_showKxKyZeroGuides {false};

    // Plotting state
    QVector<QColor> colors;
    bool bShowBlocksEdges;
    QVector<bool> m_curveVisibility; // Track visibility of each curve (0: ADC, 1: RF Mag, 2: RF Phase, 3: GX, 4: GY, 5: GZ)
    bool m_autoExpandMode; // Control behavior: true = auto expand remaining curves, false = just hide curves

    // Performance optimization data
    static const int MAX_POINTS_PER_GRAPH = 500;
    static const int MAX_POINTS_PER_BLOCK = 50;
    static const int MAX_POINTS_FOR_LARGE_RANGE = 200;
    int m_nCurrentMaxPoints;

    
    struct LODRenderData {
        QVector<double> timePoints;
        QVector<double> values;
        LODLevel lodLevel;
        bool isValid = false;
        
        LODRenderData() = default;
        LODRenderData(const QVector<double>& time, const QVector<double>& val, LODLevel level)
            : timePoints(time), values(val), lodLevel(level), isValid(true) {}
    };
    
    // LOD Cache system for performance optimization
    struct LODCacheEntry {
        QVector<double> originalTime;
        QVector<double> originalValues;
        QHash<LODLevel, LODRenderData> lodData;
        bool isFullyProcessed = false;
        QDateTime lastAccess;
        
        LODCacheEntry() = default;
        LODCacheEntry(const QVector<double>& time, const QVector<double>& values)
            : originalTime(time), originalValues(values), lastAccess(QDateTime::currentDateTime()) {}
    };
    
    // Cache for LOD data - key: blockIndex + channel
    QHash<QString, LODCacheEntry> m_lodCache;
    static const int MAX_CACHE_SIZE = 5000; // Increased cache size for better performance
    
    // Simple LOD management
    void generateLODData(const QVector<double>& originalTime, const QVector<double>& originalValues, 
                        LODLevel level, QVector<double>& lodTime, QVector<double>& lodValues);
    
    // LTTB (Largest Triangle Three Buckets) downsampling
    void applyLTTBDownsampling(const QVector<double>& time, const QVector<double>& values,
                               int targetPoints,
                               QVector<double>& downsampledTime, QVector<double>& downsampledValues);
    double calculateTriangleArea(double x1, double y1, double x2, double y2, double x3, double y3);
    // Very fast min-max per pixel downsampling for huge spans
    void applyMinMaxDownsampling(const QVector<double>& time, const QVector<double>& values,
                                 int pixelBuckets,
                                 QVector<double>& outTime, QVector<double>& outValues);
    
    // Simple LOD data generation
    bool isComplexCurve(const class SeqBlock& block, int channel = -1); // -1 for RF
    
    // Simple LOD Cache management
    QString generateCacheKey(int blockIndex, int channel, const QVector<double>& timeData, const QVector<double>& valueData);
    LODRenderData getCachedLODData(const QString& cacheKey, LODLevel level);
    void cacheLODData(const QString& cacheKey, const QVector<double>& originalTime, const QVector<double>& originalValues, 
                     LODLevel level, const QVector<double>& lodTime, const QVector<double>& lodValues);
    void cleanupLODCache();
    void ensureTeGuideCapacity();
    void updateTeGuides(double visibleStart, double visibleEnd);
    void hideTeGuideItems();
    void ensureKxKyZeroGuideCapacity();
    void updateKxKyZeroGuides(double visibleStart, double visibleEnd);
    void hideKxKyZeroGuideItems();

    // Zoom management
    ZoomManager* m_zoomManager {nullptr};

    // Extension labels overlay (SLC/REP/AVG...)
    std::unique_ptr<ExtensionPlotter> m_extensionPlotter;

    // Fixed Y-axis ranges per rect (0..5). When locked, draw functions won't adjust Y ranges dynamically.
    bool m_lockYAxisRanges {false};
    QVector<QPair<double,double>> m_fixedYRanges; // size 6, (min,max) per axis rect

    // Simple cached render state
    double m_lastViewportLower { std::numeric_limits<double>::infinity() };
    double m_lastViewportUpper { -std::numeric_limits<double>::infinity() };
    LODLevel m_lastLODLevel { LODLevel::DOWNSAMPLED };
    
    // Simple viewport change detection
    QTimer* m_viewportChangeTimer;
    static const int VIEWPORT_CHANGE_DELAY_MS = 200; // 200ms delay before processing viewport changes
    double m_pendingViewportStart;
    double m_pendingViewportEnd;

    // Initial view state for reset functionality
public:
    double m_initialViewportLower {0.0};
    double m_initialViewportUpper {0.0};
    bool m_initialViewSaved {false};

    // Axis reorder state
    QStringList m_axesOrder; // labels in current visual order
    int m_dropIndicatorIndex {-1};
    QCPItemText* m_dragGhost {nullptr};
    

public:
    // ===== DEBUG CONTROL SECTION =====
    // Centralized debug control - modify these to enable/disable debug output
    static constexpr bool DEBUG_EXTTRAP_GRADIENTS = false;     // ExtTrap gradient debug output
    static constexpr bool DEBUG_GRADIENT_LIBRARY = false;      // Gradient library debug output
    static constexpr bool DEBUG_GRADIENT_DRAWING = false;      // Gradient drawing debug output
    static constexpr bool DEBUG_GRADIENT_EVENTS = false;       // Gradient events debug output
    static constexpr bool DEBUG_LABEL_EVENTS = false;          // Label events debug output
    
    // LOD System Debug Controls
    static constexpr bool DEBUG_LOD_SYSTEM = true;            // LOD system debug output
    static constexpr bool DEBUG_LOD_VIEWPORT_CHANGES = true;  // Viewport change detection
    static constexpr bool DEBUG_LOD_CURVE_COUNTING = true;     // Complex curve counting
    static constexpr bool DEBUG_LOD_LEVEL_DECISION = true;    // LOD level decision process
    static constexpr bool DEBUG_LOD_CACHE_OPERATIONS = true;   // Cache operations
    static constexpr bool DEBUG_LOD_RENDERING_PERFORMANCE = true; // Rendering performance timing
    static constexpr bool DEBUG_LOD_DOWNSAMPLING = true;       // Downsampling operations
    static constexpr bool DEBUG_LOD_DEBOUNCE = true;          // Debounce mechanism debug
    // ===== END DEBUG CONTROL SECTION =====

    int getDropIndicatorIndex() const { return m_dropIndicatorIndex; }
};

#endif // WAVEFORMDRAWER_H
