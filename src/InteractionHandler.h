#ifndef INTERACTIONHANDLER_H
#define INTERACTIONHANDLER_H

#include <QObject>
#include <QPoint>
#include <QTimer>
#include <QVector>

// Forward declarations to avoid circular dependencies
class MainWindow;
class QDragEnterEvent;
class QDropEvent;
class QMouseEvent;
class QWheelEvent;
class QLineEdit;
class QCPItemRect;
class QCPItemStraightLine;
class EventBlockInfoDialog;
class QCPRange;

class InteractionHandler : public QObject
{
    Q_OBJECT

public:
    explicit InteractionHandler(MainWindow* mainWindow);
    ~InteractionHandler();

    // These methods will be called directly from MainWindow's overridden event handlers
    void dragEnterEvent(QDragEnterEvent *event);
    void dropEvent(QDropEvent *event);
    void wheelEvent(QWheelEvent* event);
    bool eventFilter(QObject* obj, QEvent* event);

public slots:
    void onMouseMove(QMouseEvent* event);
    void onMouseWheel(QWheelEvent* event); // For QCustomPlot's signal
    void onMousePress(QMouseEvent* event);
    void onMouseRelease(QMouseEvent* event);
    void showContextMenu(const QPoint& pos);
    void showBlockInformation();
    void showRawBlockData();
    void zoomIn();
    void zoomOut();
    void synchronizeXAxes(const QCPRange& newRange);
    void toggleMeasureDtMode();
    void exitMeasureDtMode();

public:
    // Pan boundary control
    void setPanBoundaryMode(bool enableBoundaries);
    bool getPanBoundaryMode() const;
    void showBoundaryTooltip(const QString& message);

private:
    void handleTrInputWheelEvent(QWheelEvent* event, QLineEdit* input);
    void handleTimeInputWheelEvent(QWheelEvent* event, QLineEdit* input);
    QCPRange getCurrentTimeRange() const;
    void applyBoundaryRestrictionsToAllAxes();
    // Coalesced wheel processing to improve "stickiness" and avoid post-scroll zoom bursts
    void processAccumulatedWheel();
    void processDeferredViewportRender();
    void processFinalViewportRender();

    // Axis drag-reorder helpers
    bool isOverAxisLabelArea(const QPoint& pos, int& axisIndex) const;
    void beginAxisDrag(int axisIndex, const QPoint& pos);
    void updateAxisDrag(const QPoint& pos);
    void endAxisDrag(const QPoint& pos);

private:
    MainWindow* m_mainWindow; // Pointer to access MainWindow members (like ui and other handlers)

    // Member variables moved from MainWindow
    QPoint m_rightClickPos;
    EventBlockInfoDialog* m_pBlockInfoDialog;

    bool m_bIsSelecting;
    QPoint m_objSelectStartPos;
    QCPItemRect* m_pSelectionRect;
    QPoint m_objDragStartPos;
    double m_dDragStartRange;
    
    // Pan boundary control
    bool m_enablePanBoundaries; // Control whether pan operations respect time range boundaries
    
    // Tooltip for boundary feedback
    QTimer* m_tooltipTimer; // Timer to auto-hide tooltip
    bool m_tooltipVisible; // Track tooltip visibility state

    // Wheel coalescing state (throttling bursty high-res wheel events)
    QTimer* m_wheelTimer {nullptr};
    int m_accumulatedWheelDelta {0};
    QPointF m_lastWheelPos;
    Qt::KeyboardModifiers m_lastWheelModifiers {Qt::NoModifier};

    // Axis drag state
    bool m_axisDragging {false};
    int m_dragSourceIndex {-1};
    QPoint m_dragStartPos;
    bool m_overAxisHandle {false};
    unsigned int m_prevInteractions {0};
    // Drag gesture recognition
    int m_pendingAxisIndex {-1};
    QPoint m_pressPos;
    const int m_dragStartThresholdPx {6};

    // Measure Δt mode state
    bool m_measureMode {false};
    bool m_measureHasFirst {false};
    double m_measureT1 {0.0};
    double m_measureT2 {0.0};
    // Visuals for measure: two vertical lines and a shaded rect
    QVector<QCPItemStraightLine*> m_measureLines; // size 2 per row
    QVector<QCPItemRect*> m_measureShades; // 1 per row

    // Synchronization guard to avoid re-entrant/duplicate heavy work
    bool m_syncInProgress {false};
    // Coalesce expensive viewport re-render work during continuous zoom/pan.
    QTimer* m_viewportRenderTimer {nullptr};
    QTimer* m_viewportFinalTimer {nullptr};
    bool m_pendingTrajectoryRefresh {false};
};

#endif // INTERACTIONHANDLER_H
