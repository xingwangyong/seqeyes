#ifndef TRMANAGER_H
#define TRMANAGER_H

#include <QObject>

// Forward declarations
class MainWindow;
class InteractionHandler;
class DoubleRangeSlider;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTimer;
class QVBoxLayout;
class QCheckBox;
class QRadioButton;
class QButtonGroup;
class QCPRange;
class QToolButton;

class TRManager : public QObject
{
    Q_OBJECT

public:
    explicit TRManager(MainWindow* mainWindow);
    ~TRManager();

    // Public methods for setup
    void createWidgets();
    void setupLayouts(QVBoxLayout* mainLayout);
    void connectSignals();
    void installEventFilters();

    // Render mode query
    bool isTrBasedMode() const;
    bool isTimeBasedMode() const;

    // Public methods for control
    void updateTrControls(); // Called by PulseqLoader after loading a file
    void resetTimeWindow();

    // Programmatic render mode control
    void setRenderModeTrSegmented();
    void setRenderModeWholeSequence();

    // Programmatic curve visibility control (syncs checkbox state and rendering)
    void setShowADC(bool visible);
    void setShowRFMag(bool visible);
    void setShowRFPhase(bool visible);
    void setShowGx(bool visible);
    void setShowGy(bool visible);
    void setShowGz(bool visible);
    void setShowPns(bool visible);
    bool isShowPnsChecked() const;
    void refreshShowTeOverlay();
    void setShowTrajectory(bool visible);
    void refreshExtensionLegend();

    // Getters for widgets needed by InteractionHandler
    QLineEdit* getTrStartInput() const { return m_pTrStartInput; }
    QLineEdit* getTrEndInput() const { return m_pTrEndInput; }
    QLineEdit* getTrIncInput() const { return m_pTrIncInput; }
    QLineEdit* getTimeStartInput() const { return m_pTimeStartInput; }
    QLineEdit* getTimeEndInput() const { return m_pTimeEndInput; }
    QLineEdit* getTimeIncInput() const { return m_pTimeIncInput; }
    DoubleRangeSlider* getTrRangeSlider() const { return m_pTrRangeSlider; }
    DoubleRangeSlider* getTimeRangeSlider() const { return m_pTimeRangeSlider; }

public slots:
    // Slots for UI connections
    void onTrRangeSliderChanged(int start, int end);
    void onTimeRangeSliderChanged(int start, int end);
    void onTrStartInputChanged();
    void onTrEndInputChanged();
    void onTrIncrementEditingFinished();
    void onTimeStartInputChanged();
    void onTimeEndInputChanged();
    void onTimeIncrementEditingFinished();
    void onTrSliderChanged(int value);
    void onIntraTrSliderChanged(int value);
    void updateTrStatusDisplay();
    void onApplyManualTr();
    void performDelayedUpdate();
    void onShowBlockEdgesToggled(bool checked);
    void onShowADCToggled(bool checked);
    void onShowRFMagToggled(bool checked);
    void onShowRFPhaseToggled(bool checked);
    void onShowGxToggled(bool checked);
    void onShowGyToggled(bool checked);
    void onShowGzToggled(bool checked);
    void onShowPnsToggled(bool checked);
    void onShowTeToggled(bool checked);
    void onShowKxKyZeroToggled(bool checked);
    void onShowTrajectoryToggled(bool checked);
    void onShowExtensionLegendToggled(bool checked);
    void onBlockStartEditingFinished();
    void onBlockEndEditingFinished();
    void onBlockIncrementEditingFinished();
    // Future: per-label channel toggles could be declared here
    void onRenderModeChanged(int id);

	// Pan/Zoom toolbar button handlers
	void onPanLeftClicked();
	void onPanRightClicked();
	void onZoomInClicked();
	void onZoomOutClicked();

private:
    // Helper Functions
    void updateTrFromManualInput();
    void updateTrRangeDisplay();
    void setTimeRange(double startMs, double endMs);  // Unified function for all time range updates
    void updateTimeRangeDisplay();
    void setTrRangeSliderValues(int start, int end);
    void setTimeRangeSliderValues(double start, double end);
    void updateTimeSliderFromTrRange(int startTr, int endTr);
    void updateBlockRangeFromTrRange(int startTr, int endTr);
    void calculateBlockRangeFromTrRange(int startTr, int endTr, int& startBlock, int& endBlock);
    void applyRenderModeUI();
    bool canEnableTeOverlay(QString& reason) const;
    void updateBlockWindowDisplay(double absoluteStartTime_ms, double absoluteEndTime_ms);
    bool applyBlockRangeSelection(int startBlock, int endBlock);
	// Icon helpers
	QIcon makeMagnifierIcon(bool plus) const;

public:
    // Reflect the current axis range back to time inputs/slider without emitting signals
    void syncTimeControlsToAxisRange(const QCPRange& axisRange);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Allow InteractionHandler to invoke certain private helpers for keyboard shortcuts.
    friend class InteractionHandler;

    MainWindow* m_mainWindow;

    // Member variables moved from MainWindow
    // Double-slider TR controls
    DoubleRangeSlider* m_pTrRangeSlider;
    QLabel* m_pTrRangeLabel;
    QLineEdit* m_pTrStartInput;
    QLineEdit* m_pTrEndInput;
    QLabel* m_pTrStartLabel;
    QLabel* m_pTrEndLabel;
    QLabel* m_pTrIncLabel;
    QLineEdit* m_pTrIncInput;

    // Double-slider Time controls
    DoubleRangeSlider* m_pTimeRangeSlider;
    QLabel* m_pTimeRangeLabel;
    QLineEdit* m_pTimeStartInput;
    QLineEdit* m_pTimeEndInput;
    QLabel* m_pTimeStartLabel;
    QLabel* m_pTimeEndLabel;
    QLabel* m_pTimeIncLabel;
    QLineEdit* m_pTimeIncInput;
    QLabel* m_pBlockStartLabel;
    QLabel* m_pBlockEndLabel;
    QLabel* m_pBlockIncLabel;
    QLineEdit* m_pBlockStartInput;
    QLineEdit* m_pBlockEndInput;
    QLineEdit* m_pBlockIncInput;

    // Manual TR Input Controls
    QLineEdit* m_pManualTrInput;
    QLabel* m_pManualTrLabel;
    QPushButton* m_pApplyTrButton;

	// Pan/Zoom toolbar buttons (between Manual TR and Time)
	QToolButton* m_pPanLeftButton {nullptr};
	QToolButton* m_pPanRightButton {nullptr};
	QToolButton* m_pZoomInButton {nullptr};
	QToolButton* m_pZoomOutButton {nullptr};

    // Display Options
    QCheckBox* m_pShowBlockEdgesCheckBox;
    QCheckBox* m_pShowTeCheckBox;
    QCheckBox* m_pShowKxKyZeroCheckBox {nullptr};
    QCheckBox* m_pShowTrajectoryCheckBox;
    QCheckBox* m_pShowExtensionLegendCheckBox {nullptr};
    class ExtensionLegendDialog* m_pExtensionLegendDialog {nullptr};
    
    // Curve Visibility Controls
    QCheckBox* m_pShowADCCheckBox;
    QCheckBox* m_pShowRFMagCheckBox;
    QCheckBox* m_pShowRFPhaseCheckBox;
    QCheckBox* m_pShowGxCheckBox;
    QCheckBox* m_pShowGyCheckBox;
    QCheckBox* m_pShowGzCheckBox;
    QCheckBox* m_pShowPnsCheckBox;

    // Render mode controls
    QRadioButton* m_pModeTrRadio;
    QRadioButton* m_pModeTimeRadio;
    QButtonGroup* m_pModeGroup;
    QToolButton* m_pMeasureDtButton {nullptr};

    // Legacy compatibility controls
    QSlider* m_pTrSlider;
    QLabel* m_pTrLabel;
    QSlider* m_pIntraTrSlider;
    QLabel* m_pIntraTrLabel;
    bool m_bTrRangeMode;

    // Time Window Navigation State
    double m_dTimeWindowPosition;
    double m_dTimeWindowSize;
    bool m_bUserSetTimeWindow;

    // Performance optimization
    QTimer* m_pUpdateTimer;
    bool m_bPendingUpdate;

    // Preserve last relative time window across TR changes in TR mode
    double m_lastRelStartMs {0.0};
    double m_lastRelEndMs {0.0};
    int m_lastTimeSliderMax {0};
};

#endif // TRMANAGER_H
