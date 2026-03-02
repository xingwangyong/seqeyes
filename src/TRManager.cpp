#include "TRManager.h"
#include "NumericLineEdit.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "WaveformDrawer.h"
#include "doublerangeslider.h"
#include "InteractionHandler.h"
#include "Settings.h"
#include "ExtensionLegendDialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QToolButton>
#include <QStyle>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QWheelEvent>
#include <QKeyEvent>
#include <iostream>
#include <cmath>
#include <limits>
#include <QIntValidator>
#include <algorithm>
#include <QFileInfo>

TRManager::TRManager(MainWindow* mainWindow)
    : QObject(mainWindow),
    m_mainWindow(mainWindow),
    m_pTrRangeSlider(nullptr), m_pTrRangeLabel(nullptr), m_pTrStartInput(nullptr), m_pTrEndInput(nullptr),
    m_pTrStartLabel(nullptr), m_pTrEndLabel(nullptr), m_pTimeRangeSlider(nullptr), m_pTimeRangeLabel(nullptr),
    m_pTimeStartInput(nullptr), m_pTimeEndInput(nullptr), m_pTimeStartLabel(nullptr), m_pTimeEndLabel(nullptr),
    m_pBlockStartLabel(nullptr), m_pBlockEndLabel(nullptr), m_pBlockStartInput(nullptr), m_pBlockEndInput(nullptr),
    m_pManualTrInput(nullptr), m_pManualTrLabel(nullptr), m_pApplyTrButton(nullptr), m_pTrSlider(nullptr),
    m_pTrLabel(nullptr), m_pIntraTrSlider(nullptr), m_pIntraTrLabel(nullptr), m_bTrRangeMode(false),
    m_dTimeWindowPosition(0.0), m_dTimeWindowSize(0.0), m_bUserSetTimeWindow(false),
    m_pUpdateTimer(nullptr), m_bPendingUpdate(false),
    m_pShowBlockEdgesCheckBox(nullptr), m_pShowTeCheckBox(nullptr), m_pShowPnsCheckBox(nullptr)
{
}

TRManager::~TRManager()
{
    // All widgets are parented to MainWindow, so they will be deleted automatically.
}

bool TRManager::isTrBasedMode() const
{
    // Avoid including MainWindow in the header by resolving here
    if (!m_pModeTrRadio || !m_pModeTrRadio->isChecked()) return false;
    if (!m_mainWindow) return false;
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    return loader && loader->hasRepetitionTime();
}

bool TRManager::isTimeBasedMode() const
{
    return !isTrBasedMode();
}

void TRManager::createWidgets()
{
    // TR Range Slider
    m_pTrRangeSlider = new DoubleRangeSlider(m_mainWindow);
    m_pTrRangeSlider->setRange(0, 0);
    m_pTrRangeSlider->setMinimumWidth(0);
    m_pTrRangeLabel = new QLabel("1-1", m_mainWindow);
    m_pTrRangeLabel->setVisible(false); // UI compact: hide redundant "1-2" display
    m_pTrStartLabel = new QLabel("Start:", m_mainWindow);
    m_pTrStartLabel->setVisible(false); // UI compact: hide "Start:"
    m_pTrStartInput = new QLineEdit(m_mainWindow);
    m_pTrStartInput->setPlaceholderText("1");
    m_pTrStartInput->setMaximumWidth(44);
    m_pTrStartInput->setValidator(new QIntValidator(1, std::numeric_limits<int>::max(), m_pTrStartInput));
    m_pTrEndLabel = new QLabel("End:", m_mainWindow);
    m_pTrEndLabel->setVisible(false); // UI compact: hide "End:"
    m_pTrEndInput = new QLineEdit(m_mainWindow);
    m_pTrEndInput->setPlaceholderText("1");
    m_pTrEndInput->setMaximumWidth(44);
    m_pTrEndInput->setValidator(new QIntValidator(1, std::numeric_limits<int>::max(), m_pTrEndInput));
    m_pTrIncLabel = new QLabel("Inc:", m_mainWindow);
    m_pTrIncLabel->setToolTip("Increase both start and end of the TR index");
    m_pTrIncInput = new QLineEdit(m_mainWindow);
    m_pTrIncInput->setPlaceholderText("0");
    m_pTrIncInput->setMaximumWidth(44);
    m_pTrIncInput->setValidator(new QIntValidator(-1000000, 1000000, m_pTrIncInput));

    // Time Range Slider
    m_pTimeRangeSlider = new DoubleRangeSlider(m_mainWindow);
    m_pTimeRangeSlider->setRange(0, 0);
    m_pTimeRangeSlider->setMinimumWidth(0);
    m_pTimeRangeLabel = new QLabel("0-0ms (Rel: 0-0ms)", m_mainWindow);
    m_pTimeRangeLabel->setVisible(false); // UI compact: hide textual time range; inputs show values
    m_pTimeStartLabel = new QLabel("Start:", m_mainWindow);
    m_pTimeStartLabel->setVisible(false); // UI compact: hide "Start:"
    m_pTimeStartInput = new NumericLineEdit(m_mainWindow);
    m_pTimeStartInput->setPlaceholderText("0");
    m_pTimeStartInput->setText("0");
    m_pTimeStartInput->setMaximumWidth(60);
    m_pTimeEndLabel = new QLabel("End:", m_mainWindow);
    m_pTimeEndLabel->setVisible(false); // UI compact: hide "End:"
    m_pTimeEndInput = new NumericLineEdit(m_mainWindow);
    m_pTimeEndInput->setPlaceholderText("0");
    m_pTimeEndInput->setText("4000");
    m_pTimeEndInput->setMaximumWidth(60);
    m_pTimeIncLabel = new QLabel("Inc:", m_mainWindow);
    m_pTimeIncLabel->setToolTip("Increase both start and end of the Time window (ms)");
    m_pTimeIncInput = new QLineEdit(m_mainWindow);
    m_pTimeIncInput->setAlignment(Qt::AlignCenter);
    m_pTimeIncInput->setMaximumWidth(60);
    m_pTimeIncInput->setText("0");
    m_pTimeIncInput->setValidator(new QIntValidator(-1000000, 1000000, m_pTimeIncInput));
    m_pBlockStartLabel = new QLabel("Block:", m_mainWindow);
    m_pBlockStartInput = new QLineEdit(m_mainWindow);
    m_pBlockStartInput->setAlignment(Qt::AlignCenter);
    m_pBlockStartInput->setMaximumWidth(60);
    m_pBlockStartInput->setPlaceholderText("-");
    m_pBlockStartInput->setText("");
    m_pBlockStartInput->setValidator(new QIntValidator(0, std::numeric_limits<int>::max(), m_pBlockStartInput));
    m_pBlockEndLabel = new QLabel("End:", m_mainWindow);
    m_pBlockEndLabel->setVisible(false); // UI compact: hide "End:"
    m_pBlockEndInput = new QLineEdit(m_mainWindow);
    m_pBlockEndInput->setAlignment(Qt::AlignCenter);
    m_pBlockEndInput->setMaximumWidth(60);
    m_pBlockEndInput->setPlaceholderText("-");
    m_pBlockEndInput->setText("");
    m_pBlockEndInput->setValidator(new QIntValidator(0, std::numeric_limits<int>::max(), m_pBlockEndInput));
    m_pBlockIncLabel = new QLabel("Inc:", m_mainWindow);
    m_pBlockIncLabel->setToolTip("Increase both start and end of the block index");
    m_pBlockIncInput = new QLineEdit(m_mainWindow);
    m_pBlockIncInput->setAlignment(Qt::AlignCenter);
    m_pBlockIncInput->setMaximumWidth(60);
    m_pBlockIncInput->setText("0");
    m_pBlockIncInput->setValidator(new QIntValidator(-1000000, 1000000, m_pBlockIncInput));

    // Legacy TR Controls
    m_pTrSlider = new QSlider(Qt::Horizontal, m_mainWindow);
    m_pTrSlider->setRange(0, 0);
    m_pTrSlider->setVisible(false);
    m_pTrLabel = new QLabel("TR: 0/0", m_mainWindow);
    m_pTrLabel->setVisible(false);
    m_pIntraTrSlider = new QSlider(Qt::Horizontal, m_mainWindow);
    m_pIntraTrSlider->setRange(0, 0);
    m_pIntraTrSlider->setVisible(false);
    m_pIntraTrLabel = new QLabel("Time: 0/0", m_mainWindow);
    m_pIntraTrLabel->setVisible(false);

    // Manual TR Input Controls
    m_pManualTrLabel = new QLabel("Manual TR (s):", m_mainWindow);
    m_pManualTrInput = new QLineEdit(m_mainWindow);
    m_pManualTrInput->setPlaceholderText("Enter TR in seconds (e.g., 2.0)");
    m_pManualTrInput->setMaximumWidth(90);
    m_pApplyTrButton = new QPushButton("Apply", m_mainWindow);
    m_pApplyTrButton->setMaximumWidth(64);
    m_pApplyTrButton->setVisible(false); // UI compact: Use Enter in input to apply; hide button

	// Pan/Zoom toolbar buttons
	m_pPanLeftButton = new QToolButton(m_mainWindow);
	m_pPanLeftButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_pPanLeftButton->setToolTip("Pan left");
	m_pPanLeftButton->setAutoRaise(true);
	m_pPanRightButton = new QToolButton(m_mainWindow);
	m_pPanRightButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_pPanRightButton->setToolTip("Pan right");
	m_pPanRightButton->setAutoRaise(true);
	m_pZoomInButton = new QToolButton(m_mainWindow);
	m_pZoomInButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_pZoomInButton->setToolTip("Zoom in");
	m_pZoomInButton->setAutoRaise(true);
	m_pZoomOutButton = new QToolButton(m_mainWindow);
	m_pZoomOutButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	m_pZoomOutButton->setToolTip("Zoom out");
	m_pZoomOutButton->setAutoRaise(true);

	// Assign cross-platform icons from style/theme (with fallback)
	if (auto style = m_mainWindow->style())
	{
		m_pPanLeftButton->setIcon(style->standardIcon(QStyle::SP_ArrowLeft));
		m_pPanRightButton->setIcon(style->standardIcon(QStyle::SP_ArrowRight));
	}
	QIcon zoomIn = QIcon::fromTheme("zoom-in");
	if (zoomIn.isNull()) zoomIn = makeMagnifierIcon(true);
	QIcon zoomOut = QIcon::fromTheme("zoom-out");
	if (zoomOut.isNull()) zoomOut = makeMagnifierIcon(false);
	m_pZoomInButton->setIcon(zoomIn);
	m_pZoomOutButton->setIcon(zoomOut);
	QSize iconSz(16, 16);
	m_pPanLeftButton->setIconSize(iconSz);
	m_pPanRightButton->setIconSize(iconSz);
	m_pZoomInButton->setIconSize(iconSz);
	m_pZoomOutButton->setIconSize(iconSz);

    // Display Options
    m_pShowBlockEdgesCheckBox = new QCheckBox("Show Block Boundaries", m_mainWindow);
    m_pShowBlockEdgesCheckBox->setChecked(false); // Default to OFF to reduce clutter
    m_pShowTeCheckBox = new QCheckBox("Show TE", m_mainWindow);
    m_pShowTeCheckBox->setChecked(false);
    m_pShowTeCheckBox->setToolTip("Draw excitation center and TE guide lines");
    m_pShowKxKyZeroCheckBox = new QCheckBox("Show kxy=0 (ADC)", m_mainWindow);
    m_pShowKxKyZeroCheckBox->setChecked(false);
    m_pShowKxKyZeroCheckBox->setToolTip(
        "Draw vertical lines at ADC samples where kx≈0 and ky≈0 (k-space center).\n\n"
        "Detection method:\n"
        "• Zero-crossing interpolation between adjacent trajectory points\n"
        "• Tolerance = 0.2×deltak (where deltak = 1/FOV)\n"
        "• Only shown within ADC acquisition blocks"
    );
    m_pShowTrajectoryCheckBox = new QCheckBox("Show trajectory", m_mainWindow);
    m_pShowTrajectoryCheckBox->setChecked(false);

    // Extension legend
    m_pShowExtensionLegendCheckBox = new QCheckBox("Extension legend", m_mainWindow);
    m_pShowExtensionLegendCheckBox->setChecked(false);

    // Render Mode Radios
    m_pModeTrRadio = new QRadioButton("TR-Segmented", m_mainWindow);
    m_pModeTimeRadio = new QRadioButton("Whole-Sequence", m_mainWindow);
    m_pModeGroup = new QButtonGroup(this);
    m_pModeGroup->addButton(m_pModeTrRadio, 0);
    m_pModeGroup->addButton(m_pModeTimeRadio, 1);
    m_pModeTrRadio->setChecked(true);
    m_pModeTrRadio->setToolTip("Render sequence TR by TR. Need TR or RepetitionTime in [DEFINITIONS]");
    m_pModeTimeRadio->setToolTip("Render entire sequence continuously");

    // Measure Δt button (frequently used): place on the Render Mode row.
    m_pMeasureDtButton = new QToolButton(m_mainWindow);
    m_pMeasureDtButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_pMeasureDtButton->setAutoRaise(false);
    m_pMeasureDtButton->setCursor(Qt::PointingHandCursor);
    m_pMeasureDtButton->setToolTip("Measure Δt: click to start, click again or press Esc to exit");

    // Visual styling so it looks like a button (not a label).
    m_pMeasureDtButton->setStyleSheet(
        "QToolButton {"
        "  padding: 2px 8px;"
        "  border: 1px solid #b5b5b5;"
        "  border-radius: 4px;"
        "  background: #f4f4f4;"
        "}"
        "QToolButton:hover { background: #eaeaea; }"
        "QToolButton:pressed { background: #dcdcdc; }"
        "QToolButton:checked {"
        "  background: #cfe8ff;"
        "  border-color: #4a90e2;"
        "}"
    );
    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->actionMeasureDt)
    {
        // Make the QAction checkable so the tool button can show "active" state.
        m_mainWindow->ui->actionMeasureDt->setCheckable(true);
        m_pMeasureDtButton->setDefaultAction(m_mainWindow->ui->actionMeasureDt);
    }
    else
    {
        m_pMeasureDtButton->setText("Measure Δt");
        m_pMeasureDtButton->setToolTip("Toggle Δt measurement mode");
    }

    // Curve Visibility Controls
    m_pShowADCCheckBox = new QCheckBox("ADC", m_mainWindow);
    m_pShowADCCheckBox->setChecked(true); // Default to showing all curves
    m_pShowRFMagCheckBox = new QCheckBox("RF Mag", m_mainWindow);
    m_pShowRFMagCheckBox->setChecked(true);
    m_pShowRFPhaseCheckBox = new QCheckBox("RF/ADC Phase", m_mainWindow);
    m_pShowRFPhaseCheckBox->setChecked(true);
    m_pShowGxCheckBox = new QCheckBox("GX", m_mainWindow);
    m_pShowGxCheckBox->setChecked(true);
    m_pShowGyCheckBox = new QCheckBox("GY", m_mainWindow);
    m_pShowGyCheckBox->setChecked(true);
    m_pShowGzCheckBox = new QCheckBox("GZ", m_mainWindow);
    m_pShowGzCheckBox->setChecked(true);
    m_pShowPnsCheckBox = new QCheckBox("PNS", m_mainWindow);
    // Default OFF: most users/CI don't have ASC configured.
    m_pShowPnsCheckBox->setChecked(false);

    // Debounce Timer
    m_pUpdateTimer = new QTimer(this);
    m_pUpdateTimer->setSingleShot(true);
    m_pUpdateTimer->setInterval(100);
}

void TRManager::setupLayouts(QVBoxLayout* mainLayout)
{
    // 0) Render mode row at the very top
    QHBoxLayout* modeLayout = new QHBoxLayout();
    modeLayout->setContentsMargins(0, 0, 0, 0);
    modeLayout->setSpacing(6);
    modeLayout->addWidget(new QLabel("Render Mode:"));
    modeLayout->addWidget(m_pModeTrRadio);
    modeLayout->addWidget(m_pModeTimeRadio);
    modeLayout->addWidget(m_pMeasureDtButton);
    // Keep the rest of the row compact; consume remaining space on the right.
    modeLayout->addStretch();

    // 1) Combined sliders row (TR + Manual TR + Time in one line)
    QHBoxLayout* combinedSliders = new QHBoxLayout();
    combinedSliders->setContentsMargins(0, 0, 0, 0);
    combinedSliders->setSpacing(8);

    // TR group inline
    combinedSliders->addWidget(new QLabel("TR:"));
    combinedSliders->addWidget(m_pTrRangeSlider, 2);
    // Remove redundant "1-2" label from toolbar (kept updated internally but hidden)
    QWidget* trInputs = new QWidget(m_mainWindow);
    QHBoxLayout* trInputsLayout = new QHBoxLayout(trInputs);
    trInputsLayout->setContentsMargins(0, 0, 0, 0);
    trInputsLayout->setSpacing(4);
    // Compact TR inputs: [startInput] - [endInput]
    trInputsLayout->addWidget(m_pTrStartInput);
    QLabel* hyphen = new QLabel("-", m_mainWindow);
    hyphen->setMinimumWidth(8);
    hyphen->setAlignment(Qt::AlignCenter);
    trInputsLayout->addWidget(hyphen);
    trInputsLayout->addWidget(m_pTrEndInput);
    trInputsLayout->addSpacing(4);
    trInputsLayout->addWidget(m_pTrIncLabel);
    trInputsLayout->addWidget(m_pTrIncInput);
    combinedSliders->addWidget(trInputs);

    combinedSliders->addSpacing(12);

    // Manual TR inline between TR and Time
    combinedSliders->addWidget(m_pManualTrLabel);
    combinedSliders->addWidget(m_pManualTrInput);
    // Remove Apply button from toolbar; Enter in input triggers apply
	// Insert pan/zoom buttons between Manual TR and Time
	combinedSliders->addSpacing(6);
	combinedSliders->addWidget(m_pPanLeftButton);
	combinedSliders->addWidget(m_pPanRightButton);
	combinedSliders->addSpacing(6);
	combinedSliders->addWidget(m_pZoomInButton);
	combinedSliders->addWidget(m_pZoomOutButton);

    combinedSliders->addSpacing(12);

    // Time group inline
    combinedSliders->addWidget(new QLabel("Time:"));
    combinedSliders->addWidget(m_pTimeRangeSlider, 2);
    // Remove textual time range display for compact UI
    QWidget* timeInputs = new QWidget(m_mainWindow);
    QHBoxLayout* timeInputsLayout = new QHBoxLayout(timeInputs);
    timeInputsLayout->setContentsMargins(0, 0, 0, 0);
    timeInputsLayout->setSpacing(4);
    // Compact Time inputs: [start] - [end]
    timeInputsLayout->addWidget(m_pTimeStartInput);
    QLabel* timeHyphen = new QLabel("-", m_mainWindow);
    timeHyphen->setMinimumWidth(8);
    timeHyphen->setAlignment(Qt::AlignCenter);
    timeInputsLayout->addWidget(timeHyphen);
    timeInputsLayout->addWidget(m_pTimeEndInput);
    // Add unit after the second input
    QLabel* timeUnitMs = new QLabel("ms", m_mainWindow);
    timeUnitMs->setMinimumWidth(12);
    timeUnitMs->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeInputsLayout->addWidget(timeUnitMs);
    timeInputsLayout->addSpacing(4);
    timeInputsLayout->addWidget(m_pTimeIncLabel);
    timeInputsLayout->addWidget(m_pTimeIncInput);
    timeInputsLayout->addSpacing(8);
    timeInputsLayout->addWidget(m_pBlockStartLabel);
    timeInputsLayout->addWidget(m_pBlockStartInput);
    // Compact Block inputs: [start] - [end]
    QLabel* blockHyphen = new QLabel("-", m_mainWindow);
    blockHyphen->setMinimumWidth(8);
    blockHyphen->setAlignment(Qt::AlignCenter);
    timeInputsLayout->addWidget(blockHyphen);
    timeInputsLayout->addWidget(m_pBlockEndInput);
    timeInputsLayout->addSpacing(4);
    timeInputsLayout->addWidget(m_pBlockIncLabel);
    timeInputsLayout->addWidget(m_pBlockIncInput);
    combinedSliders->addWidget(timeInputs);

    combinedSliders->addSpacing(12);

    combinedSliders->addStretch();

    // 2) Curve visibility row
    QHBoxLayout* curveVisibilityLayout = new QHBoxLayout();
    curveVisibilityLayout->setContentsMargins(0, 0, 0, 0);
    curveVisibilityLayout->setSpacing(6);
    curveVisibilityLayout->addWidget(new QLabel("Curves:"));
    curveVisibilityLayout->addWidget(m_pShowADCCheckBox);
    curveVisibilityLayout->addWidget(m_pShowRFMagCheckBox);
    curveVisibilityLayout->addWidget(m_pShowRFPhaseCheckBox);
    curveVisibilityLayout->addWidget(m_pShowGxCheckBox);
    curveVisibilityLayout->addWidget(m_pShowGyCheckBox);
    curveVisibilityLayout->addWidget(m_pShowGzCheckBox);
    curveVisibilityLayout->addWidget(m_pShowPnsCheckBox);
    curveVisibilityLayout->addSpacing(12);
    curveVisibilityLayout->addWidget(m_pShowBlockEdgesCheckBox);
    // Remove toolbar-level Undersample (menu contains the single source of truth)
    curveVisibilityLayout->addWidget(m_pShowTeCheckBox);
    curveVisibilityLayout->addWidget(m_pShowKxKyZeroCheckBox);
    curveVisibilityLayout->addWidget(m_pShowTrajectoryCheckBox);
    curveVisibilityLayout->addWidget(m_pShowExtensionLegendCheckBox);
    // TODO: per-label channel toggles can be added dynamically based on detected labels
    curveVisibilityLayout->addStretch();

    // Add to main layout in compact order
    mainLayout->addLayout(modeLayout);
    mainLayout->addLayout(combinedSliders);
    mainLayout->addLayout(curveVisibilityLayout);
}

void TRManager::connectSignals()
{
    connect(m_pTrRangeSlider, &DoubleRangeSlider::valuesChanged, this, &TRManager::onTrRangeSliderChanged);
    connect(m_pTimeRangeSlider, &DoubleRangeSlider::valuesChanged, this, &TRManager::onTimeRangeSliderChanged);
    connect(m_pTrStartInput, &QLineEdit::textChanged, this, &TRManager::onTrStartInputChanged);
    connect(m_pTrEndInput, &QLineEdit::textChanged, this, &TRManager::onTrEndInputChanged);
    connect(m_pTrIncInput, &QLineEdit::editingFinished, this, &TRManager::onTrIncrementEditingFinished);
    // Defer heavy updates to editingFinished/returnPressed to avoid per-keystroke redraws
    connect(m_pTimeStartInput, &QLineEdit::editingFinished, this, &TRManager::onTimeStartInputChanged);
    connect(m_pTimeStartInput, &QLineEdit::returnPressed, this, &TRManager::onTimeStartInputChanged);
    connect(m_pTimeEndInput, &QLineEdit::editingFinished, this, &TRManager::onTimeEndInputChanged);
    connect(m_pTimeEndInput, &QLineEdit::returnPressed, this, &TRManager::onTimeEndInputChanged);
    connect(m_pTrSlider, &QSlider::valueChanged, this, &TRManager::onTrSliderChanged);
    connect(m_pIntraTrSlider, &QSlider::valueChanged, this, &TRManager::onIntraTrSliderChanged);
    connect(m_pApplyTrButton, &QPushButton::clicked, this, &TRManager::onApplyManualTr);
    connect(m_pManualTrInput, &QLineEdit::returnPressed, this, &TRManager::onApplyManualTr);
	// Pan/Zoom buttons
	connect(m_pPanLeftButton,  &QToolButton::clicked, this, &TRManager::onPanLeftClicked);
	connect(m_pPanRightButton, &QToolButton::clicked, this, &TRManager::onPanRightClicked);
	connect(m_pZoomInButton,   &QToolButton::clicked, this, &TRManager::onZoomInClicked);
	connect(m_pZoomOutButton,  &QToolButton::clicked, this, &TRManager::onZoomOutClicked);
    connect(m_pUpdateTimer, &QTimer::timeout, this, &TRManager::performDelayedUpdate);
    connect(m_pShowBlockEdgesCheckBox, &QCheckBox::toggled, this, &TRManager::onShowBlockEdgesToggled);
    connect(m_pShowADCCheckBox, &QCheckBox::toggled, this, &TRManager::onShowADCToggled);
    connect(m_pShowRFMagCheckBox, &QCheckBox::toggled, this, &TRManager::onShowRFMagToggled);
    connect(m_pShowRFPhaseCheckBox, &QCheckBox::toggled, this, &TRManager::onShowRFPhaseToggled);
    connect(m_pShowGxCheckBox, &QCheckBox::toggled, this, &TRManager::onShowGxToggled);
    connect(m_pShowGyCheckBox, &QCheckBox::toggled, this, &TRManager::onShowGyToggled);
    connect(m_pShowGzCheckBox, &QCheckBox::toggled, this, &TRManager::onShowGzToggled);
    connect(m_pShowPnsCheckBox, &QCheckBox::toggled, this, &TRManager::onShowPnsToggled);
    connect(m_pShowTeCheckBox, &QCheckBox::toggled, this, &TRManager::onShowTeToggled);
    connect(m_pShowKxKyZeroCheckBox, &QCheckBox::toggled, this, &TRManager::onShowKxKyZeroToggled);
    connect(m_pShowTrajectoryCheckBox, &QCheckBox::toggled, this, &TRManager::onShowTrajectoryToggled);
    connect(m_pShowExtensionLegendCheckBox, &QCheckBox::toggled, this, &TRManager::onShowExtensionLegendToggled);
    connect(m_pBlockStartInput, &QLineEdit::editingFinished, this, &TRManager::onBlockStartEditingFinished);
    connect(m_pBlockEndInput, &QLineEdit::editingFinished, this, &TRManager::onBlockEndEditingFinished);
    connect(m_pBlockIncInput, &QLineEdit::editingFinished, this, &TRManager::onBlockIncrementEditingFinished);
    connect(m_pTimeIncInput, &QLineEdit::editingFinished, this, &TRManager::onTimeIncrementEditingFinished);
    connect(m_pModeGroup, &QButtonGroup::idClicked, this, &TRManager::onRenderModeChanged);

    // Keep the legend window in sync with Settings toggles.
    connect(&Settings::getInstance(), &Settings::settingsChanged, this, &TRManager::refreshExtensionLegend);

    // Sync initial checkbox states to drawer visibility once.
    WaveformDrawer* drawer = m_mainWindow ? m_mainWindow->getWaveformDrawer() : nullptr;
    if (drawer)
    {
        drawer->setShowCurve(0, m_pShowADCCheckBox && m_pShowADCCheckBox->isChecked());
        drawer->setShowCurve(1, m_pShowRFMagCheckBox && m_pShowRFMagCheckBox->isChecked());
        drawer->setShowCurve(2, m_pShowRFPhaseCheckBox && m_pShowRFPhaseCheckBox->isChecked());
        drawer->setShowCurve(3, m_pShowGxCheckBox && m_pShowGxCheckBox->isChecked());
        drawer->setShowCurve(4, m_pShowGyCheckBox && m_pShowGyCheckBox->isChecked());
        drawer->setShowCurve(5, m_pShowGzCheckBox && m_pShowGzCheckBox->isChecked());
        drawer->setShowCurve(6, m_pShowPnsCheckBox && m_pShowPnsCheckBox->isChecked());
        drawer->updateCurveVisibility();
    }

    if (m_mainWindow)
    {
        m_mainWindow->updatePnsStatusIndicator();
    }
}

void TRManager::onShowExtensionLegendToggled(bool checked)
{
    if (checked)
    {
        if (!m_pExtensionLegendDialog)
        {
            m_pExtensionLegendDialog = new ExtensionLegendDialog(m_mainWindow);
            // If user closes the legend window, reflect it back to checkbox state.
            connect(m_pExtensionLegendDialog, &QDialog::finished, this, [this](int) {
                if (m_pShowExtensionLegendCheckBox)
                    m_pShowExtensionLegendCheckBox->setChecked(false);
            });
        }
        refreshExtensionLegend();
        m_pExtensionLegendDialog->show();
        m_pExtensionLegendDialog->raise();
        m_pExtensionLegendDialog->activateWindow();
    }
    else
    {
        if (m_pExtensionLegendDialog)
            m_pExtensionLegendDialog->hide();
    }
}

void TRManager::refreshExtensionLegend()
{
    if (!m_pExtensionLegendDialog)
        return;
    if (!m_pShowExtensionLegendCheckBox || !m_pShowExtensionLegendCheckBox->isChecked())
        return;
    auto loader = m_mainWindow ? m_mainWindow->getPulseqLoader() : nullptr;
    m_pExtensionLegendDialog->refresh(loader);
}

void TRManager::setRenderModeTrSegmented()
{
    if (!m_pModeTrRadio) return;
    m_pModeTrRadio->setChecked(true);
    onRenderModeChanged(0);
}

void TRManager::setRenderModeWholeSequence()
{
    if (!m_pModeTimeRadio) return;
    m_pModeTimeRadio->setChecked(true);
    onRenderModeChanged(1);
}

void TRManager::setShowADC(bool visible)
{
    if (m_pShowADCCheckBox) m_pShowADCCheckBox->setChecked(visible);
    onShowADCToggled(visible);
}
void TRManager::setShowRFMag(bool visible)
{
    if (m_pShowRFMagCheckBox) m_pShowRFMagCheckBox->setChecked(visible);
    onShowRFMagToggled(visible);
}
void TRManager::setShowRFPhase(bool visible)
{
    if (m_pShowRFPhaseCheckBox) m_pShowRFPhaseCheckBox->setChecked(visible);
    onShowRFPhaseToggled(visible);
}
void TRManager::setShowGx(bool visible)
{
    if (m_pShowGxCheckBox) m_pShowGxCheckBox->setChecked(visible);
    onShowGxToggled(visible);
}
void TRManager::setShowGy(bool visible)
{
    if (m_pShowGyCheckBox) m_pShowGyCheckBox->setChecked(visible);
    onShowGyToggled(visible);
}
void TRManager::setShowGz(bool visible)
{
    if (m_pShowGzCheckBox) m_pShowGzCheckBox->setChecked(visible);
    onShowGzToggled(visible);
}
void TRManager::setShowPns(bool visible)
{
    if (m_pShowPnsCheckBox) m_pShowPnsCheckBox->setChecked(visible);
    onShowPnsToggled(visible);
}

bool TRManager::isShowPnsChecked() const
{
    return m_pShowPnsCheckBox && m_pShowPnsCheckBox->isChecked();
}

void TRManager::refreshShowTeOverlay()
{
    if (!m_pShowTeCheckBox)
        return;
    if (m_pShowTeCheckBox->isChecked())
    {
        onShowTeToggled(true);
    }
    else if (auto drawer = m_mainWindow->getWaveformDrawer())
    {
        drawer->setShowTeGuides(false);
    }
}

void TRManager::setShowTrajectory(bool visible)
{
    if (m_pShowTrajectoryCheckBox)
        m_pShowTrajectoryCheckBox->setChecked(visible);
    onShowTrajectoryToggled(visible);
}

void TRManager::installEventFilters()
{
    m_pTrStartInput->installEventFilter(m_mainWindow->getInteractionHandler());
    m_pTrEndInput->installEventFilter(m_mainWindow->getInteractionHandler());
    m_pTimeStartInput->installEventFilter(m_mainWindow->getInteractionHandler());
    m_pTimeEndInput->installEventFilter(m_mainWindow->getInteractionHandler());
    m_pTimeRangeSlider->installEventFilter(m_mainWindow->getInteractionHandler());
    m_pBlockStartInput->installEventFilter(this);
    m_pBlockEndInput->installEventFilter(this);
    m_pBlockIncInput->installEventFilter(this);
    m_pTrIncInput->installEventFilter(this);
}

void TRManager::resetTimeWindow()
{
    m_dTimeWindowPosition = 0.0;
    m_dTimeWindowSize = 0.0;
    m_bUserSetTimeWindow = false;
}

/*
TRManager responsibilities relevant to rendering:

1) Mode selection
   - TR-based vs Time-based radio group controls the interpretation of sliders:
     * TR-based: TR range slider selects TR indices; time slider is relative within TR span.
     * Time-based: TR controls are disabled; time slider spans the entire sequence duration.

2) TR detection and defaults
   - If the sequence provides TR (RepetitionTime/TR definition) we enable TR-based mode by default.
   - If not available, force Time-based mode and disable TR controls.

3) Range propagation
   - When TR range changes, we update block range and redraw graphs, and we keep the X-axes
     consistent across subplots. Time slider is updated accordingly.
*/
void TRManager::updateTrControls()
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
    {
        return;
    }

    // Determine availability of TR
    bool hasTr = loader->hasRepetitionTime() && loader->getTrCount() > 0;

    if (!hasTr)
    {
        m_pTrSlider->setRange(0, 0);
        m_pTrSlider->setEnabled(false);
        m_pTrLabel->setText("TR: N/A (No TR definition)");
        m_pIntraTrSlider->setEnabled(false);
        m_pTrRangeSlider->setRange(0, 0);
        m_pTrRangeSlider->setEnabled(false);
        // Time slider covers the whole sequence in time-based mode
        m_pTimeRangeSlider->setEnabled(true);
        m_pIntraTrLabel->setText("Time: N/A");

        // Force time-based mode, disable TR-based radio
        m_pModeTimeRadio->setChecked(true);
        m_pModeTrRadio->setEnabled(false);
        m_pModeTimeRadio->setEnabled(true);
        applyRenderModeUI();
        return;
    }

    // Enable TR-based radio if TR exists and default to TR-based
    m_pModeTrRadio->setEnabled(true);
    m_pModeTimeRadio->setEnabled(true);
    if (!m_pModeTrRadio->isChecked())
    {
        m_pModeTrRadio->setChecked(true);
    }

    int trCount = loader->getTrCount();
    m_pTrSlider->setRange(0, trCount - 1);
    m_pTrSlider->setValue(0);
    m_pTrSlider->setEnabled(true);

    {
        const QSignalBlocker blocker(m_pTrRangeSlider);
        m_pTrRangeSlider->setRange(0, trCount - 1);
        m_pTrRangeSlider->setValues(0, 0);
    }
    m_pTrRangeSlider->setEnabled(true);

    m_pTrStartInput->setText("1");
    m_pTrEndInput->setText("1");

    // Debug logging removed
    updateTimeSliderFromTrRange(1, 1);
    updateBlockRangeFromTrRange(1, 1);
    onTrSliderChanged(0);

    applyRenderModeUI();
    // Refresh legend if open, so it reflects the newly loaded sequence.
    refreshExtensionLegend();
}

void TRManager::onTrSliderChanged(int value)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader->getTrBlockIndices().empty()) return;

    double startTime, endTime;
    double tFactor = loader->getTFactor();

    if (loader->hasRepetitionTime())
    {
        startTime = value * loader->getRepetitionTime_us() * tFactor;
        endTime = (value + 1) * loader->getRepetitionTime_us() * tFactor;
        if (endTime > loader->getTotalDuration_us() * tFactor)
        {
            endTime = loader->getTotalDuration_us() * tFactor;
        }
    }
    else
    {
        int currentTrIndex = loader->getTrBlockIndices()[value];
        int nextTrIndex = (value + 1 < loader->getTrBlockIndices().size()) ? loader->getTrBlockIndices()[value + 1] : loader->getDecodedSeqBlocks().size();
        startTime = loader->getBlockEdges()[currentTrIndex];
        endTime = loader->getBlockEdges()[nextTrIndex];
    }

    // Preserve relative time window when switching TR
    double relStart_ms = m_pTimeStartInput->text().toDouble();
    double relEnd_ms = m_pTimeEndInput->text().toDouble();
    // Clamp relative time to TR bounds but preserve the window size
    double trDuration_ms = (endTime - startTime) / (loader->getTFactor() * 1000.0);
    double windowSize = relEnd_ms - relStart_ms;
    
    // Ensure window size is valid
    if (windowSize <= 0) windowSize = trDuration_ms;
    if (windowSize > trDuration_ms) windowSize = trDuration_ms;
    
    // Clamp start time to valid range
    if (relStart_ms < 0) relStart_ms = 0;
    if (relStart_ms + windowSize > trDuration_ms) relStart_ms = trDuration_ms - windowSize;
    
    relEnd_ms = relStart_ms + windowSize;
    double rangeStart = startTime + relStart_ms * loader->getTFactor() * 1000.0;
    double rangeEnd = startTime + relEnd_ms * loader->getTFactor() * 1000.0;
    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(rangeStart, rangeEnd);
    }

    QString trLabelText;
    if (loader->hasRepetitionTime())
    {
        trLabelText = QString("TR: %1/%2 (Duration: %3s)")
            .arg(value + 1).arg(loader->getTrCount()).arg(loader->getRepetitionTime_us() / 1e6, 0, 'f', 3);
    }
    else
    {
        trLabelText = QString("TR: %1/%2 (ADC-based)").arg(value + 1).arg(loader->getTrBlockIndices().size());
    }
    m_pTrLabel->setText(trLabelText);

    m_pIntraTrSlider->setRange(startTime, endTime);
    if (m_bUserSetTimeWindow && m_dTimeWindowSize > 0)
    {
        double newTimePosition = startTime + m_dTimeWindowPosition;
        if (newTimePosition > endTime - m_dTimeWindowSize) newTimePosition = endTime - m_dTimeWindowSize;
        if (newTimePosition < startTime) newTimePosition = startTime;
        m_pIntraTrSlider->setValue(newTimePosition);
    }
    else
    {
        m_pIntraTrSlider->setValue(startTime);
    }
    m_pIntraTrSlider->setEnabled(true);
    m_pIntraTrLabel->setText(QString("Time: %1 - %2").arg(startTime, 0, 'f', 1).arg(endTime, 0, 'f', 1));
    // Update time input fields to reflect preserved relative time
    setTimeRange(relStart_ms, relEnd_ms);
    
    // Update time slider range to new TR without resetting user inputs
    updateTimeSliderFromTrRange(value + 1, value + 1);
    // Ensure labels and plot reflect preserved window
    updateTimeRangeDisplay();
    m_mainWindow->ui->customPlot->replot();
}

void TRManager::onPanLeftClicked()
{
	auto ih = m_mainWindow->getInteractionHandler();
	if (!ih) return;
	QCPRange cur = m_mainWindow->ui->customPlot->xAxis->range();
	double step = cur.size() * 0.1; // 10% per click
	double newMin = cur.lower - step;
	double newMax = cur.upper - step;
	ih->synchronizeXAxes(QCPRange(newMin, newMax));
}

void TRManager::onPanRightClicked()
{
	auto ih = m_mainWindow->getInteractionHandler();
	if (!ih) return;
	QCPRange cur = m_mainWindow->ui->customPlot->xAxis->range();
	double step = cur.size() * 0.1; // 10% per click
	double newMin = cur.lower + step;
	double newMax = cur.upper + step;
	ih->synchronizeXAxes(QCPRange(newMin, newMax));
}

void TRManager::onZoomInClicked()
{
	auto ih = m_mainWindow->getInteractionHandler();
	if (!ih) return;
	QCPRange cur = m_mainWindow->ui->customPlot->xAxis->range();
	double factor = 1.2; // match context menu zoom granularity
	double newRange = cur.size() / factor;
	double center = (cur.lower + cur.upper) * 0.5;
	double newMin = center - newRange * 0.5;
	double newMax = center + newRange * 0.5;
	ih->synchronizeXAxes(QCPRange(newMin, newMax));
}

void TRManager::onZoomOutClicked()
{
	auto ih = m_mainWindow->getInteractionHandler();
	if (!ih) return;
	QCPRange cur = m_mainWindow->ui->customPlot->xAxis->range();
	double factor = 1.2;
	double newRange = cur.size() * factor;
	double center = (cur.lower + cur.upper) * 0.5;
	double newMin = center - newRange * 0.5;
	double newMax = center + newRange * 0.5;
	ih->synchronizeXAxes(QCPRange(newMin, newMax));
}

QIcon TRManager::makeMagnifierIcon(bool plus) const
{
	// Minimalistic magnifier icon with +/- drawn, used as a fallback when theme icons are unavailable.
	const int sz = 16;
	QPixmap pm(sz, sz);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	QPen pen(QColor(60, 60, 60));
	pen.setWidth(2);
	p.setPen(pen);
	// Lens
	QRectF circle(2, 2, 10, 10);
	p.drawEllipse(circle);
	// Handle
	p.drawLine(QPointF(9, 9), QPointF(14, 14));
	// Plus/Minus inside lens
	QPen symPen(QColor(60, 60, 60));
	symPen.setWidth(2);
	p.setPen(symPen);
	// Horizontal bar
	p.drawLine(QPointF(4, 7), QPointF(10, 7));
	if (plus)
	{
		// Vertical bar
		p.drawLine(QPointF(7, 4), QPointF(7, 10));
	}
	p.end();
	return QIcon(pm);
}

void TRManager::onIntraTrSliderChanged(int value)
{
    double currentTrStartTime = m_pIntraTrSlider->minimum();
    double currentTrEndTime = m_pIntraTrSlider->maximum();
    double visibleWindowSize = (currentTrEndTime - currentTrStartTime) / 5.0;
    double newStart = value;
    double newEnd = value + visibleWindowSize;

    if (newEnd > currentTrEndTime)
    {
        newEnd = currentTrEndTime;
        newStart = newEnd - visibleWindowSize;
    }
    if (newStart < currentTrStartTime)
    {
        newStart = currentTrStartTime;
        newEnd = newStart + visibleWindowSize;
    }

    m_dTimeWindowPosition = value - currentTrStartTime;
    m_dTimeWindowSize = visibleWindowSize;
    m_bUserSetTimeWindow = true;

    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
    {
        rect->axis(QCPAxis::atBottom)->setRange(newStart, newEnd);
    }

    double relativeTime = value - currentTrStartTime;
    double trDuration = currentTrEndTime - currentTrStartTime;
    double tFactor = m_mainWindow->getPulseqLoader()->getTFactor();
    m_pIntraTrLabel->setText(QString("Time: %1s/%2s (Window: %3s)")
        .arg(relativeTime / tFactor / 1e6, 0, 'f', 3)
        .arg(trDuration / tFactor / 1e6, 0, 'f', 3)
        .arg(visibleWindowSize / tFactor / 1e6, 0, 'f', 3));

    m_mainWindow->ui->customPlot->replot();
}

void TRManager::updateTrStatusDisplay()
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    QString statusText;
    bool trMode = isTrBasedMode();
    if (trMode && loader->hasRepetitionTime())
    {
        statusText = QString("Render: TR-Segmented (%1 TRs, %2s each)")
            .arg(loader->getTrCount())
            .arg(loader->getRepetitionTime_us() / 1e6, 0, 'f', 3);
    }
    else
    {
        double total_ms = loader->getTotalDuration_us() / 1000.0;
        statusText = QString("Render: Whole-Sequence (0-%1 ms)")
            .arg(static_cast<int>(std::round(total_ms)));
    }

    if (m_mainWindow->getCoordLabel())
    {
        QString currentText = m_mainWindow->getCoordLabel()->text();
        if (!currentText.isEmpty() && !currentText.contains("TR Mode"))
        {
            m_mainWindow->getCoordLabel()->setText(currentText + " | " + statusText);
        }
        else
        {
            m_mainWindow->getCoordLabel()->setText(statusText);
        }
    }
}

void TRManager::onApplyManualTr()
{
    QString inputText = m_pManualTrInput->text().trimmed();
    if (inputText.isEmpty()) return;

    bool ok;
    double trValue = inputText.toDouble(&ok);
    if (!ok || trValue <= 0) return;

    // This logic should be in PulseqLoader as it modifies sequence properties
    m_mainWindow->getPulseqLoader()->setManualRepetitionTime(trValue);

    updateTrControls();
    updateTrStatusDisplay();
}

void TRManager::onTrRangeSliderChanged(int start, int end)
{
    int startTr = start + 1;
    int endTr = end + 1;
    m_pTrStartInput->setText(QString::number(startTr));
    m_pTrEndInput->setText(QString::number(endTr));
    updateTrRangeDisplay();
}

void TRManager::onTimeRangeSliderChanged(int start, int end)
{
    double startTime_ms = start;
    double endTime_ms = end;
    setTimeRange(startTime_ms, endTime_ms);
    // Apply to axes immediately when user moves the time slider
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader) {
        bool trMode = m_pModeTrRadio->isChecked() && loader->hasRepetitionTime();
        double absStart_ms, absEnd_ms;
        if (trMode) {
            int startTr = m_pTrStartInput->text().toInt();
            double trStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
            absStart_ms = trStart_ms + startTime_ms;
            absEnd_ms = trStart_ms + endTime_ms;
        } else {
            absStart_ms = startTime_ms;
            absEnd_ms = endTime_ms;
        }
        double tFactor = loader->getTFactor();
        double rangeStart = absStart_ms * tFactor * 1000.0;
        double rangeEnd   = absEnd_ms   * tFactor * 1000.0;
        if (auto ih = m_mainWindow->getInteractionHandler()) {
            ih->synchronizeXAxes(QCPRange(rangeStart, rangeEnd));
        }
    }
}

void TRManager::onTrStartInputChanged()
{
    bool ok;
    int startTr = m_pTrStartInput->text().toInt(&ok);
    if (ok && startTr > 0)
    {
        int currentEnd = m_pTrEndInput->text().toInt();
        if (currentEnd < startTr)
        {
            m_pTrEndInput->setText(QString::number(startTr));
            currentEnd = startTr;
        }
        {
            const QSignalBlocker blocker(m_pTrRangeSlider);
            m_pTrRangeSlider->setValues(startTr - 1, currentEnd - 1);
        }
        m_bPendingUpdate = true;
        m_pUpdateTimer->start();
    }
}

void TRManager::onTrEndInputChanged()
{
    bool ok;
    int endTr = m_pTrEndInput->text().toInt(&ok);
    if (ok && endTr > 0)
    {
        int currentStart = m_pTrStartInput->text().toInt();
        if (currentStart > endTr)
        {
            m_pTrStartInput->setText(QString::number(endTr));
            currentStart = endTr;
        }
        {
            const QSignalBlocker blocker(m_pTrRangeSlider);
            m_pTrRangeSlider->setValues(currentStart - 1, endTr - 1);
        }
        m_bPendingUpdate = true;
        m_pUpdateTimer->start();
    }
}

void TRManager::onTimeStartInputChanged()
{
    bool ok;
    double startTime_ms = m_pTimeStartInput->text().toDouble(&ok);
    if (ok)
    {
        double minTime = m_pTimeRangeSlider->minimum();
        double maxTime = m_pTimeRangeSlider->maximum();

        // Ensure non-negative time - STRICTLY PROHIBIT negative values
        if (startTime_ms < 0) startTime_ms = 0;
        if (startTime_ms < minTime) startTime_ms = minTime;
        if (startTime_ms > maxTime) startTime_ms = maxTime;

        double currentEnd = m_pTimeEndInput->text().toDouble();
        if (currentEnd < startTime_ms) currentEnd = startTime_ms;
        
        setTimeRange(startTime_ms, currentEnd);
        // Apply to axes on commit (editingFinished/return)
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        if (loader) {
            bool trMode = m_pModeTrRadio->isChecked() && loader->hasRepetitionTime();
            double absStart_ms, absEnd_ms;
            if (trMode) {
                int startTr = m_pTrStartInput->text().toInt();
                double trStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
                absStart_ms = trStart_ms + startTime_ms;
                absEnd_ms = trStart_ms + currentEnd;
            } else {
                absStart_ms = startTime_ms;
                absEnd_ms = currentEnd;
            }
            double tFactor = loader->getTFactor();
            double rangeStart = absStart_ms * tFactor * 1000.0;
            double rangeEnd   = absEnd_ms   * tFactor * 1000.0;
            if (auto ih = m_mainWindow->getInteractionHandler()) {
                ih->synchronizeXAxes(QCPRange(rangeStart, rangeEnd));
            }
        }
    }
}

void TRManager::onTimeEndInputChanged()
{
    bool ok;
    double endTime_ms = m_pTimeEndInput->text().toDouble(&ok);
    if (ok)
    {
        double minTime = m_pTimeRangeSlider->minimum();
        double maxTime = m_pTimeRangeSlider->maximum();

        // Ensure non-negative time - STRICTLY PROHIBIT negative values
        if (endTime_ms < 0) endTime_ms = 0;
        if (endTime_ms < minTime) endTime_ms = minTime;
        if (endTime_ms > maxTime) endTime_ms = maxTime;

        double currentStart = m_pTimeStartInput->text().toDouble();
        if (currentStart > endTime_ms) currentStart = endTime_ms;
        
        setTimeRange(currentStart, endTime_ms);
        // Apply to axes on commit (editingFinished/return)
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        if (loader) {
            bool trMode = m_pModeTrRadio->isChecked() && loader->hasRepetitionTime();
            double absStart_ms, absEnd_ms;
            if (trMode) {
                int startTr = m_pTrStartInput->text().toInt();
                double trStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
                absStart_ms = trStart_ms + currentStart;
                absEnd_ms = trStart_ms + endTime_ms;
            } else {
                absStart_ms = currentStart;
                absEnd_ms = endTime_ms;
            }
            double tFactor = loader->getTFactor();
            double rangeStart = absStart_ms * tFactor * 1000.0;
            double rangeEnd   = absEnd_ms   * tFactor * 1000.0;
            if (auto ih = m_mainWindow->getInteractionHandler()) {
                ih->synchronizeXAxes(QCPRange(rangeStart, rangeEnd));
            }
        }
    }
}

void TRManager::onTimeIncrementEditingFinished()
{
    if (!m_pTimeIncInput || !m_pTimeStartInput || !m_pTimeEndInput || !m_pTimeRangeSlider)
        return;

    bool okInc = false;
    double inc = m_pTimeIncInput->text().toDouble(&okInc);
    if (!okInc || inc == 0.0)
        return;

    bool okStart = false, okEnd = false;
    double startMs = m_pTimeStartInput->text().toDouble(&okStart);
    double endMs   = m_pTimeEndInput->text().toDouble(&okEnd);
    if (!okStart || !okEnd)
        return;

    if (endMs < startMs)
        std::swap(startMs, endMs);

    double windowSize = endMs - startMs;
    if (windowSize < 0.0)
        windowSize = 0.0;

    double minAllowed = std::max(0.0, static_cast<double>(m_pTimeRangeSlider->minimum()));
    double maxAllowed = static_cast<double>(m_pTimeRangeSlider->maximum());

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

    setTimeRange(newStart, newEnd);

    // Apply to axes (same semantics as manual Time start/end edits)
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (loader)
    {
        bool trMode = m_pModeTrRadio->isChecked() && loader->hasRepetitionTime();
        double absStart_ms, absEnd_ms;
        if (trMode)
        {
            int startTr = m_pTrStartInput->text().toInt();
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
}

void TRManager::performDelayedUpdate()
{
    if (!m_bPendingUpdate) return;
    updateTrRangeDisplay();
    m_bPendingUpdate = false;
}

void TRManager::updateTrRangeDisplay()
{
    int startTr = m_pTrStartInput->text().toInt();
    int endTr = m_pTrEndInput->text().toInt();
    m_pTrRangeLabel->setText(QString("%1-%2").arg(startTr).arg(endTr));

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    // Only apply TR-Segmented behavior when TR-Segmented mode is selected and TR exists
    bool trMode = m_pModeTrRadio->isChecked();
    if (trMode && loader->hasRepetitionTime())
    {
        double tFactor = loader->getTFactor();
        double startTime = (startTr - 1) * loader->getRepetitionTime_us() * tFactor;
        double endTime = endTr * loader->getRepetitionTime_us() * tFactor;
        if (endTime > loader->getTotalDuration_us() * tFactor)
        {
            endTime = loader->getTotalDuration_us() * tFactor;
        }

        int newStartBlock, newEndBlock;
        calculateBlockRangeFromTrRange(startTr, endTr, newStartBlock, newEndBlock);
        if (newStartBlock != loader->getBlockRangeStart() || newEndBlock != loader->getBlockRangeEnd())
        {
            loader->setBlockRange(newStartBlock, newEndBlock);
            
            // Update time slider range for new TR, but postpone actual axis replot
            updateTimeSliderFromTrRange(startTr, endTr);
            
            // Redraw with correct viewport using persistent graphs (no clearGraphs)
            WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
            drawer->DrawRFWaveform(0, -1);
            drawer->DrawADCWaveform(0, -1);
            drawer->DrawGWaveform(0, -1);
            if (drawer->getShowBlockEdges())
            {
                drawer->DrawBlockEdges();
            }
            // Update axis range to the new TR window (preserve current relative window)
            {
                double relStart_ms = m_pTimeStartInput->text().toDouble();
                double relEnd_ms   = m_pTimeEndInput->text().toDouble();
                double trStart_ms  = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
                double absStart_ms = trStart_ms + relStart_ms;
                double absEnd_ms   = trStart_ms + relEnd_ms;
                double tFactor     = loader->getTFactor();
                QCPRange newRange(absStart_ms * tFactor * 1000.0,
                                  absEnd_ms   * tFactor * 1000.0);
                if (auto ih = m_mainWindow->getInteractionHandler()) {
                    ih->synchronizeXAxes(newRange);
                } else {
                    // Fallback: set each rect directly if InteractionHandler unavailable
                    for (auto& rect : m_mainWindow->getWaveformDrawer()->getRects())
                        rect->axis(QCPAxis::atBottom)->setRange(newRange);
                }
            }

            // Update labels and replot once
            updateTimeRangeDisplay();
        }
        else
        {
            // Preserve user-chosen relative time range instead of forcing [startTime, endTime]
            updateTimeSliderFromTrRange(startTr, endTr);
            updateTimeRangeDisplay();
        }
    }
    m_mainWindow->ui->customPlot->replot();
}

// Unified function to update time range - all time modifications should call this
void TRManager::setTimeRange(double startMs, double endMs)
{
    // Debug logging removed
    
    // Update input fields
    {
        const QSignalBlocker b1(m_pTimeStartInput);
        const QSignalBlocker b2(m_pTimeEndInput);
        m_pTimeStartInput->setText(QString::number(startMs, 'f', 1));
        m_pTimeEndInput->setText(QString::number(endMs, 'f', 1));
    }
    
    // Update slider
    {
        const QSignalBlocker b3(m_pTimeRangeSlider);
        m_pTimeRangeSlider->setValues(static_cast<int>(startMs), static_cast<int>(endMs));
    }
    // Remember last relative window
    m_lastRelStartMs = startMs;
    m_lastRelEndMs = endMs;
    
    // Update display
    updateTimeRangeDisplay();
}

void TRManager::updateTimeRangeDisplay()
{
    double relativeStartTime_ms = m_pTimeStartInput->text().toDouble();
    double relativeEndTime_ms = m_pTimeEndInput->text().toDouble();
    
    // Debug logging removed

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    bool trMode = m_pModeTrRadio->isChecked() && loader->hasRepetitionTime();

    double absoluteStartTime_ms;
    double absoluteEndTime_ms;
    if (trMode)
    {
        int startTr = m_pTrStartInput->text().toInt();
        int endTr = m_pTrEndInput->text().toInt();
        double absoluteTrStartTime_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
        double absoluteTrEndTime_ms = endTr * loader->getRepetitionTime_us() / 1000.0;
        
        absoluteStartTime_ms = absoluteTrStartTime_ms + relativeStartTime_ms;
        absoluteEndTime_ms = absoluteTrStartTime_ms + relativeEndTime_ms;
        
        // Clamp to TR range bounds
        if (absoluteEndTime_ms > absoluteTrEndTime_ms) absoluteEndTime_ms = absoluteTrEndTime_ms;
        if (absoluteStartTime_ms < absoluteTrStartTime_ms) absoluteStartTime_ms = absoluteTrStartTime_ms;
    }
    else
    {
        // Whole-Sequence: relative inputs are absolute over the whole sequence
        absoluteStartTime_ms = relativeStartTime_ms;
        absoluteEndTime_ms = relativeEndTime_ms;
    }

    m_pTimeRangeLabel->setText(QString("%1-%2ms (Rel: %3-%4ms)")
        .arg(static_cast<int>(std::round(absoluteStartTime_ms)))
        .arg(static_cast<int>(std::round(absoluteEndTime_ms)))
        .arg(static_cast<int>(std::round(relativeStartTime_ms)))
        .arg(static_cast<int>(std::round(relativeEndTime_ms))));

    updateBlockWindowDisplay(absoluteStartTime_ms, absoluteEndTime_ms);

    double tFactor = loader->getTFactor();
    double rangeStart, rangeEnd;
    
    if (trMode) {
        // In TR-segmented mode, show absolute time on x-axis
        rangeStart = absoluteStartTime_ms * tFactor * 1000;
        rangeEnd = absoluteEndTime_ms * tFactor * 1000;
    } else {
        // In whole-sequence mode, show absolute time
        rangeStart = absoluteStartTime_ms * tFactor * 1000;
        rangeEnd = absoluteEndTime_ms * tFactor * 1000;
    }

    // Do not trigger viewport sync here; keep a single entry-point (initial sync
    // happens after load, and user interactions/explicit time window changes will
    // drive synchronizeXAxes). This avoids duplicate heavy work.
}

void TRManager::updateBlockWindowDisplay(double absoluteStartTime_ms, double absoluteEndTime_ms)
{
    if (!m_pBlockStartInput || !m_pBlockEndInput)
        return;

    auto setPlaceholder = [this]() {
        const QSignalBlocker b1(m_pBlockStartInput);
        const QSignalBlocker b2(m_pBlockEndInput);
        m_pBlockStartInput->setText("-");
        m_pBlockEndInput->setText("-");
    };

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
    {
        setPlaceholder();
        return;
    }

    const QVector<double>& edges = loader->getBlockEdges();
    if (edges.size() < 2)
    {
        setPlaceholder();
        return;
    }

    double tFactor = loader->getTFactor();
    if (tFactor <= 0.0)
    {
        setPlaceholder();
        return;
    }

    double startInternal = absoluteStartTime_ms * tFactor * 1000.0;
    double endInternal = absoluteEndTime_ms * tFactor * 1000.0;
    if (endInternal < startInternal)
        std::swap(startInternal, endInternal);

    auto findBlockIndex = [&](double internal) -> int {
        auto it = std::upper_bound(edges.begin(), edges.end(), internal);
        int idx = static_cast<int>(it - edges.begin()) - 1;
        if (idx < 0) idx = 0;
        int maxIdx = static_cast<int>(edges.size()) - 2;
        if (idx > maxIdx) idx = maxIdx;
        return idx;
    };

    int startBlock = findBlockIndex(startInternal);
    int endBlock = findBlockIndex(endInternal);

    const QSignalBlocker b1(m_pBlockStartInput);
    const QSignalBlocker b2(m_pBlockEndInput);
    m_pBlockStartInput->setText(QString::number(startBlock));
    m_pBlockEndInput->setText(QString::number(endBlock));
}

bool TRManager::applyBlockRangeSelection(int startBlock, int endBlock)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
        return false;
    const QVector<double>& edges = loader->getBlockEdges();
    if (edges.size() < 2)
        return false;
    int maxBlock = edges.size() - 2;
    if (maxBlock < 0)
        maxBlock = 0;
    startBlock = std::clamp(startBlock, 0, maxBlock);
    endBlock = std::clamp(endBlock, startBlock, maxBlock);

    double startInternal = edges[startBlock];
    double endInternal = edges[endBlock + 1];
    if (!std::isfinite(startInternal) || !std::isfinite(endInternal) || endInternal <= startInternal)
        endInternal = startInternal + 1.0;

    if (auto ih = m_mainWindow->getInteractionHandler())
    {
        ih->synchronizeXAxes(QCPRange(startInternal, endInternal));
    }
    else
    {
        QCPRange range(startInternal, endInternal);
        WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
        if (drawer)
        {
            for (auto* rect : drawer->getRects())
            {
                rect->axis(QCPAxis::atBottom)->setRange(range);
            }
        }
        m_mainWindow->ui->customPlot->replot();
        syncTimeControlsToAxisRange(range);
    }

    {
        const QSignalBlocker b1(m_pBlockStartInput);
        const QSignalBlocker b2(m_pBlockEndInput);
        m_pBlockStartInput->setText(QString::number(startBlock));
        m_pBlockEndInput->setText(QString::number(endBlock));
    }
    return true;
}

bool TRManager::eventFilter(QObject* watched, QEvent* event)
{
    auto handleDelta = [&](QLineEdit* target, int delta, bool allowNegative) -> bool {
        if (!target || delta == 0)
            return false;
        bool ok = false;
        int value = target->text().toInt(&ok);
        if (!ok)
            value = allowNegative ? 0 : std::max(0, value);
        long long next = static_cast<long long>(value) + delta;
        if (!allowNegative && next < 0)
            next = 0;
        const QSignalBlocker blocker(target);
        target->setText(QString::number(next));
        if (target == m_pBlockStartInput)
            onBlockStartEditingFinished();
        else if (target == m_pBlockEndInput)
            onBlockEndEditingFinished();
        else if (target == m_pBlockIncInput)
            onBlockIncrementEditingFinished();
        return true;
    };

    auto processWheel = [&](QWheelEvent* wheel, QLineEdit* target, bool allowNegative) -> bool {
        if (!wheel)
            return false;
        int delta = wheel->angleDelta().y();
        if (delta == 0)
            delta = wheel->pixelDelta().y();
        if (delta == 0)
            return false;
        int step = (delta > 0) ? 1 : -1;
        bool handled = handleDelta(target, step, allowNegative);
        if (handled)
            wheel->accept();
        return handled;
    };

    auto processKey = [&](QKeyEvent* key, QLineEdit* target, bool allowNegative) -> bool {
        if (!key)
            return false;
        int step = 0;
        if (key->key() == Qt::Key_Up)
            step = 1;
        else if (key->key() == Qt::Key_Down)
            step = -1;
        if (step == 0)
            return false;
        bool handled = handleDelta(target, step, allowNegative);
        if (handled)
            key->accept();
        return handled;
    };

    const bool watchingBlockStart = watched == m_pBlockStartInput;
    const bool watchingBlockEnd = watched == m_pBlockEndInput;
    const bool watchingBlockInc = watched == m_pBlockIncInput;
    const bool watchingTrInc = watched == m_pTrIncInput;
    const bool watchingTimeInc = watched == m_pTimeIncInput;

    if (watchingBlockStart || watchingBlockEnd || watchingBlockInc || watchingTrInc || watchingTimeInc)
    {
        const bool allowNegative = watchingBlockInc || watchingTrInc || watchingTimeInc;
        if (event->type() == QEvent::Wheel)
        {
            if (processWheel(static_cast<QWheelEvent*>(event),
                             qobject_cast<QLineEdit*>(watched),
                             allowNegative))
                return true;
        }
        else if (event->type() == QEvent::KeyPress)
        {
            if (processKey(static_cast<QKeyEvent*>(event),
                           qobject_cast<QLineEdit*>(watched),
                           allowNegative))
                return true;
        }
    }

    return QObject::eventFilter(watched, event);
}

void TRManager::updateTimeSliderFromTrRange(int startTr, int endTr)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader->hasRepetitionTime()) return;

    // Calculate the total duration of the selected TR range
    double absoluteStartTime_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
    double absoluteEndTime_ms = endTr * loader->getRepetitionTime_us() / 1000.0;
    double totalTrDuration_ms = absoluteEndTime_ms - absoluteStartTime_ms;

    // Debug logging removed

    int newMax = static_cast<int>(std::round(totalTrDuration_ms));
    m_lastTimeSliderMax = newMax;
    {
        const QSignalBlocker blocker(m_pTimeRangeSlider);
        m_pTimeRangeSlider->setRange(0, newMax);
    }

    // Determine desired slider values from last known relative window
    // Prefer numeric inputs if valid; else fall back to remembered window
    bool okStart = false, okEnd = false;
    double curStartD = m_pTimeStartInput->text().toDouble(&okStart);
    double curEndD = m_pTimeEndInput->text().toDouble(&okEnd);
    if (!okStart && !okEnd) {
        curStartD = m_lastRelStartMs;
        curEndD = m_lastRelEndMs;
    }
    // Clamp
    int curStart = std::max(0, std::min(newMax, static_cast<int>(std::round(curStartD))));
    int curEnd = std::max(curStart, std::min(newMax, static_cast<int>(std::round(curEndD))));

    {
        const QSignalBlocker blocker(m_pTimeRangeSlider);
        m_pTimeRangeSlider->setValues(curStart, curEnd);
    }
}

void TRManager::applyRenderModeUI()
{
    // Reentrancy guard to coalesce multiple signal-driven calls
    static bool s_applying = false;
    if (s_applying) return;
    s_applying = true;

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader) return;

    bool trAvailable = loader->hasRepetitionTime();
    bool trMode = m_pModeTrRadio->isChecked() && trAvailable;

    // TR controls enabled only in TR-Segmented mode
    m_pTrRangeSlider->setEnabled(trMode);
    m_pTrStartInput->setEnabled(trMode);
    m_pTrEndInput->setEnabled(trMode);

    // Time range label content changes depending on mode
    if (trMode)
    {
        // Time slider shows within selected TRs
        // It is configured by updateTimeSliderFromTrRange
        int startTr = m_pTrStartInput->text().toInt();
        int endTr = m_pTrEndInput->text().toInt();
        updateTimeSliderFromTrRange(startTr, endTr);
    }
    else
    {
        // Whole-Sequence: time slider covers the whole sequence duration in ms
        // Prefer computing from block edges to avoid any mismatch
        double total_ms_edges = 0.0;
        const auto& edges = loader->getBlockEdges();
        if (!edges.isEmpty())
        {
            // edges are in internal units (microseconds * tFactor)
            // Convert back to milliseconds: edges.last() / (tFactor * 1000)
            total_ms_edges = edges.last() / (loader->getTFactor() * 1000.0);
        }
        double total_ms_cached = loader->getTotalDuration_us() / 1000.0;
        double total_ms = std::max(total_ms_edges, total_ms_cached);
        int total_ms_i = static_cast<int>(std::round(total_ms));
        {
            const QSignalBlocker blocker(m_pTimeRangeSlider);
            m_pTimeRangeSlider->setRange(0, total_ms_i);
        }
        // Do not trigger redraws here; the time range is applied via the interaction path on user actions.
        setTimeRange(0, total_ms_i);
    }

    updateTimeRangeDisplay();

    s_applying = false;
}

bool TRManager::canEnableTeOverlay(QString& reason) const
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
    {
        reason = "No sequence is currently loaded.";
        return false;
    }
    if (!loader->hasEchoTimeDefinition())
    {
        reason = "The sequence does not define TE or EchoTime in the [DEFINITIONS] section.";
        return false;
    }
    // For older Pulseq versions without explicit RF use metadata, we still try to
    // draw TE guides based on detected RF uses (classifyRfUse), but warn the user
    // that the result may be approximate.
    if (!loader->supportsExcitationMetadata())
    {
        const auto& centers = loader->getExcitationCenters();
        if (centers.isEmpty())
        {
            reason = "This Pulseq version does not tag RF excitation uses and no excitation centers could be detected; TE overlay cannot be drawn.";
            return false;
        }
        reason = "This Pulseq version does not tag RF excitation uses (requires v1.5 or newer). "
                 "Using detected RF uses to estimate excitation centers; TE overlay may be inaccurate.";
        return true;
    }

    if (loader->getExcitationCenters().isEmpty())
    {
        reason = "No RF pulses with use='e' were found, so excitation center lines cannot be drawn.";
        return false;
    }

    reason.clear();
    return true;
}

void TRManager::onRenderModeChanged(int id)
{
    Q_UNUSED(id);
    applyRenderModeUI();
}

void TRManager::updateBlockRangeFromTrRange(int startTr, int endTr)
{
    int startBlock, endBlock;
    calculateBlockRangeFromTrRange(startTr, endTr, startBlock, endBlock);
    m_mainWindow->getPulseqLoader()->setBlockRange(startBlock, endBlock);
}

void TRManager::calculateBlockRangeFromTrRange(int startTr, int endTr, int& startBlock, int& endBlock)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader->hasRepetitionTime() || loader->getDecodedSeqBlocks().empty())
    {
        startBlock = 0;
        endBlock = 0;
        return;
    }

    double tFactor = loader->getTFactor();
    double startTime = (startTr - 1) * loader->getRepetitionTime_us() * tFactor;
    double endTime = endTr * loader->getRepetitionTime_us() * tFactor;
    if (endTime > loader->getTotalDuration_us() * tFactor)
    {
        endTime = loader->getTotalDuration_us() * tFactor;
    }

    const auto& blockEdges = loader->getBlockEdges();
    startBlock = 0;
    endBlock = loader->getDecodedSeqBlocks().size() - 1;

    for (int i = 0; i < blockEdges.size() - 1; ++i)
    {
        if (startTime >= blockEdges[i] && startTime < blockEdges[i + 1])
        {
            startBlock = i;
            break;
        }
    }
    for (int i = 0; i < blockEdges.size() - 1; ++i)
    {
        if (endTime >= blockEdges[i] && endTime < blockEdges[i + 1])
        {
            endBlock = i;
            break;
        }
    }

    if (startBlock < 0) startBlock = 0;
    if (endBlock >= loader->getDecodedSeqBlocks().size()) endBlock = loader->getDecodedSeqBlocks().size() - 1;
    if (startBlock > endBlock) startBlock = endBlock;
}

void TRManager::updateTrFromManualInput()
{
    onApplyManualTr();
}

void TRManager::onShowBlockEdgesToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowBlockEdges(checked);
        
        // Only update the ticker, don't redraw all waveforms
        PulseqLoader* loader = m_mainWindow->getPulseqLoader();
        if (loader && !loader->getDecodedSeqBlocks().empty())
        {
            // Just update the block edges display (which handles both cases)
            drawer->DrawBlockEdges();
            // Ensure UI updates immediately after toggling
            if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
                m_mainWindow->ui->customPlot->replot();
        }
    }
}

void TRManager::onShowADCToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(0, checked); // ADC is at index 0
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowRFMagToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(1, checked); // RF Mag is at index 1
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowRFPhaseToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(2, checked); // RF/ADC Phase is at index 2
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowGxToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(3, checked); // GX is at index 3
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowGyToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(4, checked); // GY is at index 4
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowGzToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(5, checked); // GZ is at index 5
        drawer->updateCurveVisibility();
    }
}

void TRManager::onShowPnsToggled(bool checked)
{
    if (checked)
    {
        const QString ascPath = Settings::getInstance().getPnsAscPath().trimmed();
        if (ascPath.isEmpty() || !QFileInfo::exists(ascPath))
        {
            QMessageBox::warning(
                m_mainWindow,
                "Show PNS unavailable",
                "PNS requires a valid ASC profile.\n"
                "Open Settings > Safety and select a valid .asc file.");
            if (m_pShowPnsCheckBox)
            {
                QSignalBlocker blocker(m_pShowPnsCheckBox);
                m_pShowPnsCheckBox->setChecked(false);
            }
            checked = false;
        }
        else
        {
            PulseqLoader* loader = m_mainWindow ? m_mainWindow->getPulseqLoader() : nullptr;
            const QString status = loader ? loader->getPnsStatusMessage() : QString();
            if (!status.isEmpty() && status.contains("ASC", Qt::CaseInsensitive))
            {
                QMessageBox::warning(m_mainWindow, "Show PNS unavailable", status);
                if (m_pShowPnsCheckBox)
                {
                    QSignalBlocker blocker(m_pShowPnsCheckBox);
                    m_pShowPnsCheckBox->setChecked(false);
                }
                checked = false;
            }
        }
    }

    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        drawer->setShowCurve(6, checked); // PNS is at index 6
        drawer->updateCurveVisibility();
        if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
        if (checked && m_mainWindow)
        {
            // Defer one tick so axis rect geometry is finalized before PNS decimation uses rect width.
            QTimer::singleShot(0, m_mainWindow, [this]() {
                if (!m_mainWindow) return;
                auto* d = m_mainWindow->getWaveformDrawer();
                if (!d) return;
                d->DrawGWaveform();
                if (m_mainWindow->ui && m_mainWindow->ui->customPlot)
                    m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
            });
        }
    }
    if (m_mainWindow)
    {
        m_mainWindow->updatePnsStatusIndicator();
    }
}

void TRManager::onShowTeToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer)
        return;

    if (checked)
    {
        QString reason;
        if (!canEnableTeOverlay(reason))
        {
            QMessageBox::warning(m_mainWindow, "Show TE unavailable", reason);
            QSignalBlocker blocker(m_pShowTeCheckBox);
            m_pShowTeCheckBox->setChecked(false);
            drawer->setShowTeGuides(false);
            return;
        }
        else if (!reason.isEmpty())
        {
            // Non-fatal warning: older Pulseq versions without explicit RF use tags.
            // We fall back to detected RF uses, which may be approximate.
            Settings& s = Settings::getInstance();
            if (s.getShowTeApproximateDialog())
            {
                QMessageBox msg(m_mainWindow);
                msg.setIcon(QMessageBox::Warning);
                msg.setWindowTitle("Show TE (approximate)");
                msg.setText(reason);
                QCheckBox* cb = new QCheckBox("Do not show this warning again", &msg);
                msg.setCheckBox(cb);
                msg.addButton(QMessageBox::Ok);
                msg.exec();
                if (cb->isChecked())
                {
                    s.setShowTeApproximateDialog(false);
                }
            }
        }
    }

    drawer->setShowTeGuides(checked);
}

void TRManager::onShowKxKyZeroToggled(bool checked)
{
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (!drawer)
        return;

    drawer->setShowKxKyZeroGuides(checked);
}

void TRManager::onShowTrajectoryToggled(bool checked)
{
    if (m_mainWindow)
    {
        m_mainWindow->setTrajectoryVisible(checked);
    }
}

void TRManager::onBlockStartEditingFinished()
{
    if (!m_pBlockStartInput || !m_pBlockEndInput)
        return;
    bool okStart = false;
    int startBlock = m_pBlockStartInput->text().toInt(&okStart);
    if (!okStart)
        return;
    bool okEnd = false;
    int endBlock = m_pBlockEndInput->text().toInt(&okEnd);
    if (!okEnd)
        endBlock = startBlock;
    applyBlockRangeSelection(startBlock, endBlock);
}

void TRManager::onBlockEndEditingFinished()
{
    if (!m_pBlockStartInput || !m_pBlockEndInput)
        return;
    bool okEnd = false;
    int endBlock = m_pBlockEndInput->text().toInt(&okEnd);
    if (!okEnd)
        return;
    bool okStart = false;
    int startBlock = m_pBlockStartInput->text().toInt(&okStart);
    if (!okStart)
        startBlock = endBlock;
    applyBlockRangeSelection(startBlock, endBlock);
}

void TRManager::onBlockIncrementEditingFinished()
{
    if (!m_pBlockIncInput || !m_pBlockStartInput || !m_pBlockEndInput)
        return;
    bool okInc = false;
    int inc = m_pBlockIncInput->text().toInt(&okInc);
    if (!okInc)
        return;

    bool okStart = false;
    int startBlock = m_pBlockStartInput->text().toInt(&okStart);
    bool okEnd = false;
    int endBlock = m_pBlockEndInput->text().toInt(&okEnd);
    if (!okStart || !okEnd)
        return;

    applyBlockRangeSelection(startBlock + inc, endBlock + inc);
}

void TRManager::onTrIncrementEditingFinished()
{
    if (!m_pTrIncInput || !m_pTrStartInput || !m_pTrEndInput)
        return;

    bool okInc = false;
    int inc = m_pTrIncInput->text().toInt(&okInc);
    if (!okInc || inc == 0)
        return;

    bool okStart = false;
    int startTr = m_pTrStartInput->text().toInt(&okStart);
    bool okEnd = false;
    int endTr = m_pTrEndInput->text().toInt(&okEnd);
    if (!okStart || !okEnd)
        return;

    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader)
        return;
    int trCount = loader->getTrCount();
    if (trCount <= 0)
        return;

    startTr += inc;
    endTr += inc;

    startTr = std::clamp(startTr, 1, trCount);
    endTr = std::clamp(endTr, 1, trCount);
    if (startTr > endTr)
        startTr = endTr;

    {
        const QSignalBlocker b1(m_pTrStartInput);
        const QSignalBlocker b2(m_pTrEndInput);
        m_pTrStartInput->setText(QString::number(startTr));
        m_pTrEndInput->setText(QString::number(endTr));
    }
    {
        const QSignalBlocker blocker(m_pTrRangeSlider);
        m_pTrRangeSlider->setValues(startTr - 1, endTr - 1);
    }

    // Apply full TR range update immediately
    updateTrRangeDisplay();
}

void TRManager::syncTimeControlsToAxisRange(const QCPRange& axisRange)
{
    PulseqLoader* loader = m_mainWindow->getPulseqLoader();
    if (!loader) return;

    double tFactor = loader->getTFactor();
    double absoluteStart_ms = axisRange.lower / (tFactor * 1000.0);
    double absoluteEnd_ms = axisRange.upper / (tFactor * 1000.0);
    if (absoluteEnd_ms < absoluteStart_ms) {
        double tmp = absoluteStart_ms; absoluteStart_ms = absoluteEnd_ms; absoluteEnd_ms = tmp;
    }

    bool trMode = isTrBasedMode();
    double relStart_ms = absoluteStart_ms;
    double relEnd_ms = absoluteEnd_ms;
    double labelAbsStart_ms = absoluteStart_ms;
    double labelAbsEnd_ms = absoluteEnd_ms;

    if (trMode && loader->hasRepetitionTime())
    {
        int startTr = m_pTrStartInput->text().toInt();
        int endTr = m_pTrEndInput->text().toInt();
        double trAbsStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
        double trAbsEnd_ms = endTr * loader->getRepetitionTime_us() / 1000.0;
        double trDuration_ms = trAbsEnd_ms - trAbsStart_ms;
        if (trDuration_ms < 0) trDuration_ms = 0;

        relStart_ms = absoluteStart_ms - trAbsStart_ms;
        relEnd_ms = absoluteEnd_ms - trAbsStart_ms;
        if (relStart_ms < 0) relStart_ms = 0;
        if (relEnd_ms < 0) relEnd_ms = 0;
        if (relStart_ms > trDuration_ms) relStart_ms = trDuration_ms;
        if (relEnd_ms > trDuration_ms) relEnd_ms = trDuration_ms;

        // Keep label absolute within TR bounds
        if (labelAbsStart_ms < trAbsStart_ms) labelAbsStart_ms = trAbsStart_ms;
        if (labelAbsEnd_ms > trAbsEnd_ms) labelAbsEnd_ms = trAbsEnd_ms;
        if (labelAbsEnd_ms < labelAbsStart_ms) labelAbsEnd_ms = labelAbsStart_ms;
    }

    // Silent updates to inputs and slider
    {
        const QSignalBlocker b1(m_pTimeStartInput);
        const QSignalBlocker b2(m_pTimeEndInput);
        m_pTimeStartInput->setText(QString::number(relStart_ms, 'f', 1));
        m_pTimeEndInput->setText(QString::number(relEnd_ms, 'f', 1));
    }
    {
        const QSignalBlocker b3(m_pTimeRangeSlider);
        m_pTimeRangeSlider->setValues(static_cast<int>(std::round(relStart_ms)),
                                      static_cast<int>(std::round(relEnd_ms)));
    }
    // Remember last relative window
    m_lastRelStartMs = relStart_ms;
    m_lastRelEndMs = relEnd_ms;

    // Update label only; do not call updateTimeRangeDisplay here
    m_pTimeRangeLabel->setText(QString("%1-%2ms (Rel: %3-%4ms)")
        .arg(static_cast<int>(std::round(labelAbsStart_ms)))
        .arg(static_cast<int>(std::round(labelAbsEnd_ms)))
        .arg(static_cast<int>(std::round(relStart_ms)))
        .arg(static_cast<int>(std::round(relEnd_ms))));

    updateBlockWindowDisplay(labelAbsStart_ms, labelAbsEnd_ms);
}
