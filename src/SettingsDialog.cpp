#include "SettingsDialog.h"
#include <QMessageBox>
#include <QDebug>
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpacerItem>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QSignalBlocker>
#include <QSet>
#include <algorithm>
#include <limits>
#include "mainwindow.h"
#include "WaveformDrawer.h"
#include "PnsCalculator.h"

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , m_tabWidget(nullptr)
    , m_gradientUnitCombo(nullptr)
    , m_slewUnitCombo(nullptr)
    , m_timeUnitCombo(nullptr)
    , m_gammaCombo(nullptr)
    , m_logLevelCombo(nullptr)
    , m_zoomModeCombo(nullptr)
    , m_panDragCheck(nullptr)
    , m_panWheelCheck(nullptr)
    , m_showExtensionTooltipCheck(nullptr)
    , m_enableRoosPtxHackAutoDetectionCheck(nullptr)
    , m_systemProfileCombo(nullptr)
    , m_addSystemProfileButton(nullptr)
    , m_removeSystemProfileButton(nullptr)
    , m_systemAliasEdit(nullptr)
    , m_systemAscPathEdit(nullptr)
    , m_systemAscBrowseButton(nullptr)
    , m_systemAscClearButton(nullptr)
    , m_systemMaxGradEdit(nullptr)
    , m_systemMaxSlewEdit(nullptr)
    , m_systemMaxB1Edit(nullptr)
    , m_pnsShowXCheck(nullptr)
    , m_pnsShowYCheck(nullptr)
    , m_pnsShowZCheck(nullptr)
    , m_pnsShowNormCheck(nullptr)
    , m_axisOrderList(nullptr)
    , m_moveAxisTopButton(nullptr)
    , m_moveAxisUpButton(nullptr)
    , m_moveAxisDownButton(nullptr)
    , m_moveAxisBottomButton(nullptr)
    , m_applyButton(nullptr)
    , m_okButton(nullptr)
    , m_cancelButton(nullptr)
    , m_resetButton(nullptr)
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
{
    setupUI();
    loadCurrentSettings();
}

SettingsDialog::~SettingsDialog()
{
}

void SettingsDialog::setupUI()
{
    setWindowTitle("Settings");
    setModal(true);
    resize(600, 500);
    
    // Create main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
	
	// Settings file path section (placed above tabs)
	QWidget* pathWidget = new QWidget(this);
	QHBoxLayout* pathLayout = new QHBoxLayout(pathWidget);
	pathLayout->setContentsMargins(0,0,0,0);
	QLabel* settingsPathLabel = new QLabel("Settings File:", pathWidget);
	m_settingsPathValue = new QLabel(pathWidget);
	m_settingsPathValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_settingsPathValue->setWordWrap(true);
	pathLayout->addWidget(settingsPathLabel);
	pathLayout->addWidget(m_settingsPathValue, 1);
	mainLayout->addWidget(pathWidget);
    
    // Create ribbon-style tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::North);
    m_tabWidget->setMovable(false);
    
    // ============================================================================
    // Display Units Tab (includes Physics Parameters)
    // ============================================================================
    QWidget* displayUnitsTab = new QWidget();
    QVBoxLayout* displayUnitsLayout = new QVBoxLayout(displayUnitsTab);
    
    // Display Units group (no title since it's already in the tab)
    QGroupBox* unitsGroup = new QGroupBox("", displayUnitsTab);
    QFormLayout* unitsLayout = new QFormLayout(unitsGroup);
    
    // Time unit combo
    m_timeUnitCombo = new QComboBox(displayUnitsTab);
    m_timeUnitCombo->addItem("ms (milliseconds)", static_cast<int>(Settings::TimeUnit::Milliseconds));
    m_timeUnitCombo->addItem("us (microseconds)", static_cast<int>(Settings::TimeUnit::Microseconds));
    unitsLayout->addRow("Time Unit:", m_timeUnitCombo);

    // Trajectory unit combo (unit only; appearance is in a separate group)
    m_trajectoryUnitCombo = new QComboBox(displayUnitsTab);
    m_trajectoryUnitCombo->addItems({"1/m", "rad/m", "1/FOV"});
    unitsLayout->addRow("Trajectory Unit:", m_trajectoryUnitCombo);
    
    // Gradient unit combo
    m_gradientUnitCombo = new QComboBox(displayUnitsTab);
    m_gradientUnitCombo->addItems({"Hz/m", "mT/m", "rad/ms/mm", "G/cm"});
    unitsLayout->addRow("Gradient Unit:", m_gradientUnitCombo);
    
    // Slew unit combo
    m_slewUnitCombo = new QComboBox(displayUnitsTab);
    m_slewUnitCombo->addItems({"Hz/m/s", "mT/m/ms", "T/m/s", "rad/ms/mm/ms", "G/cm/ms", "G/cm/s"});
    unitsLayout->addRow("Slew Rate Unit:", m_slewUnitCombo);
    
    // Gamma combo box with common nuclei (moved from Physics Parameters)
    m_gammaCombo = new QComboBox(displayUnitsTab);
    m_gammaCombo->setEditable(true); // Allow manual input
    m_gammaCombo->addItem("¹H (Hydrogen) - 42.576 MHz/T", 42.576e6);
    m_gammaCombo->addItem("²³Na (Sodium) - 11.262 MHz/T", 11.262e6);
    m_gammaCombo->addItem("³¹P (Phosphorus) - 17.235 MHz/T", 17.235e6);
    m_gammaCombo->addItem("¹³C (Carbon-13) - 10.705 MHz/T", 10.705e6);
    m_gammaCombo->addItem("¹⁹F (Fluorine) - 40.053 MHz/T", 40.053e6);
    m_gammaCombo->addItem("⁷Li (Lithium-7) - 16.546 MHz/T", 16.546e6);
    m_gammaCombo->addItem("Custom...", -1); // Special value for custom input
    m_gammaCombo->setCurrentIndex(0); // Default to ¹H
    unitsLayout->addRow("Gyromagnetic Ratio:", m_gammaCombo);
    
    displayUnitsLayout->addWidget(unitsGroup);

    // Separate Trajectory Appearance group (no title; just spacing and one row)
    QGroupBox* trajAppearanceGroup = new QGroupBox("", displayUnitsTab);
    QFormLayout* trajLayout = new QFormLayout(trajAppearanceGroup);
    m_trajectoryColormapCombo = new QComboBox(displayUnitsTab);
    m_trajectoryColormapCombo->addItem("Jet",     static_cast<int>(Settings::TrajectoryColormap::Jet));
    m_trajectoryColormapCombo->addItem("Cividis", static_cast<int>(Settings::TrajectoryColormap::Cividis));
    m_trajectoryColormapCombo->addItem("Plasma",  static_cast<int>(Settings::TrajectoryColormap::Plasma));
    trajLayout->addRow("Trajectory Colormap:", m_trajectoryColormapCombo);

    displayUnitsLayout->addWidget(trajAppearanceGroup);
    displayUnitsLayout->addStretch();
    
    m_tabWidget->addTab(displayUnitsTab, "Display Units");
    
    // ============================================================================
    // Logging Tab
    // ============================================================================
    QWidget* loggingTab = new QWidget();
    QVBoxLayout* loggingLayout = new QVBoxLayout(loggingTab);
    
    // Logging Settings group (no title since it's already in the tab)
    QGroupBox* loggingGroup = new QGroupBox("", loggingTab);
    QFormLayout* loggingFormLayout = new QFormLayout(loggingGroup);
    
    // Log level combo (minimum level semantics), ordered: Debug < Info < Warning < Critical < Fatal
    m_logLevelCombo = new QComboBox(loggingTab);
    m_logLevelCombo->addItem("Debug",   static_cast<int>(Settings::LogLevel::Debug));
    m_logLevelCombo->addItem("Info",    static_cast<int>(Settings::LogLevel::Info));
    m_logLevelCombo->addItem("Warning", static_cast<int>(Settings::LogLevel::Warning));
    m_logLevelCombo->addItem("Critical",static_cast<int>(Settings::LogLevel::Critical));
    m_logLevelCombo->addItem("Fatal",   static_cast<int>(Settings::LogLevel::Fatal));
    loggingFormLayout->addRow("Log Level:", m_logLevelCombo);

    // Show settings file path (read-only)
	// Moved to the top of the dialog (above tabs)
    
    loggingLayout->addWidget(loggingGroup);
    loggingLayout->addStretch();
    
    m_tabWidget->addTab(loggingTab, "Logging");

    // ============================================================================
    // Interactions Tab
    // ============================================================================
    QWidget* interactionsTab = new QWidget();
    QVBoxLayout* interactionsLayout = new QVBoxLayout(interactionsTab);

    QGroupBox* interactionsGroup = new QGroupBox("", interactionsTab);
    QFormLayout* interactionsForm = new QFormLayout(interactionsGroup);

    // Zoom mode combo
    m_zoomModeCombo = new QComboBox(interactionsTab);
    m_zoomModeCombo->addItem("Ctrl+Mouse wheel", static_cast<int>(Settings::ZoomInputMode::CtrlWheel));
    m_zoomModeCombo->addItem("Mouse wheel", static_cast<int>(Settings::ZoomInputMode::Wheel));
    interactionsForm->addRow("Zoom:", m_zoomModeCombo);

    // Pan options
    QWidget* panOpts = new QWidget(interactionsTab);
    QHBoxLayout* panLayout = new QHBoxLayout(panOpts);
    panLayout->setContentsMargins(0,0,0,0);
    m_panDragCheck = new QCheckBox("Drag", panOpts);
    m_panDragCheck->setChecked(true);
    m_panDragCheck->setEnabled(false); // Always enabled but not user-toggleable
    m_panWheelCheck = new QCheckBox("Mouse wheel", panOpts);
    panLayout->addWidget(m_panDragCheck);
    panLayout->addWidget(m_panWheelCheck);
    interactionsForm->addRow("Pan:", panOpts);

    // Keyboard shortcuts info (fixed, not editable; full-width block under the form)
    m_shortcutInfoLabel = new QLabel(interactionsTab);
    m_shortcutInfoLabel->setWordWrap(true);
    m_shortcutInfoLabel->setText(
        "<b>Pan</b><br>"
        "  A / Left Arrow  : Pan left<br>"
        "  D / Right Arrow : Pan right<br>"
        "<br>"
        "<b>TR stepping</b><br>"
        "  Alt+Q           : Decrease TR start/end (step = |TR Inc|, default 1)<br>"
        "  Alt+W           : Increase TR start/end (step = |TR Inc|, default 1)<br>"
        "<br>"
        "<b>Time window</b><br>"
        "  Alt+E           : Decrease Time start/end (step = |Time Inc|, or ~10% of window)<br>"
        "  Alt+R           : Increase Time start/end (same step as Alt+E)<br>"
        "  Ctrl+Left/Right : Same as Alt+E / Alt+R");
    m_shortcutInfoLabel->setToolTip(
        "TR stepping:\n"
        "  • If TR Inc is empty or 0, the step size is 1 TR.\n"
        "  • If TR Inc = x ≠ 0, the step size is |x| TR:\n"
        "      Alt+Q always moves backward (-|x|), Alt+W always moves forward (+|x|).\n"
        "\n"
        "Time window stepping:\n"
        "  • If Time Inc is empty or 0, the step size is ~10% of the current Time window (at least 1 ms).\n"
        "  • If Time Inc = y ≠ 0, the step size is |y| ms:\n"
        "      Alt+E always moves backward (-|y|), Alt+R always moves forward (+|y|).\n"
        "  • Ctrl+Left / Ctrl+Right are equivalent shortcuts to Alt+E / Alt+R.");

    QWidget* kbWidget = new QWidget(interactionsTab);
    QVBoxLayout* kbLayout = new QVBoxLayout(kbWidget);
    kbLayout->setContentsMargins(0, 0, 0, 0);
    kbLayout->addWidget(m_shortcutInfoLabel);

    interactionsLayout->addWidget(interactionsGroup);
    interactionsLayout->addWidget(kbWidget);
    interactionsLayout->addStretch();

    m_tabWidget->addTab(interactionsTab, "Interactions");

    // ============================================================================
    // Layout Tab
    // ============================================================================
    QWidget* layoutTab = new QWidget();
    QVBoxLayout* layoutLayout = new QVBoxLayout(layoutTab);

    QLabel* layoutHint = new QLabel(
        "Use drag and drop or the Top/Up/Down/Bottom buttons to adjust subplot order.",
        layoutTab);
    layoutHint->setWordWrap(true);
    layoutLayout->addWidget(layoutHint);

    QWidget* axisOrderWidget = new QWidget(layoutTab);
    QHBoxLayout* axisOrderLayout = new QHBoxLayout(axisOrderWidget);
    axisOrderLayout->setContentsMargins(0, 0, 0, 0);

    m_axisOrderList = new QListWidget(layoutTab);
    m_axisOrderList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_axisOrderList->setDragDropMode(QAbstractItemView::InternalMove);
    m_axisOrderList->setDefaultDropAction(Qt::MoveAction);
    m_axisOrderList->setDragEnabled(true);
    m_axisOrderList->setAcceptDrops(true);
    m_axisOrderList->setDropIndicatorShown(true);
    axisOrderLayout->addWidget(m_axisOrderList, 1);

    QWidget* axisButtonsWidget = new QWidget(layoutTab);
    QVBoxLayout* axisButtonsLayout = new QVBoxLayout(axisButtonsWidget);
    axisButtonsLayout->setContentsMargins(0, 0, 0, 0);
    m_moveAxisTopButton = new QPushButton("Top", layoutTab);
    m_moveAxisUpButton = new QPushButton("Up", layoutTab);
    m_moveAxisDownButton = new QPushButton("Down", layoutTab);
    m_moveAxisBottomButton = new QPushButton("Bottom", layoutTab);
    axisButtonsLayout->addWidget(m_moveAxisTopButton);
    axisButtonsLayout->addWidget(m_moveAxisUpButton);
    axisButtonsLayout->addWidget(m_moveAxisDownButton);
    axisButtonsLayout->addWidget(m_moveAxisBottomButton);
    axisButtonsLayout->addStretch();
    axisOrderLayout->addWidget(axisButtonsWidget);

    layoutLayout->addWidget(axisOrderWidget);
    layoutLayout->addStretch();

    m_tabWidget->addTab(layoutTab, "Layout");

    // ============================================================================
    // Extensions Tab (Pulseq labels)
    // ============================================================================
    QWidget* extensionsTab = new QWidget();
    QVBoxLayout* extensionsLayout = new QVBoxLayout(extensionsTab);

    // Checkbox for extension tooltip
    m_showExtensionTooltipCheck = new QCheckBox("Show extension tooltip", extensionsTab);
    extensionsLayout->addWidget(m_showExtensionTooltipCheck);
    m_enableRoosPtxHackAutoDetectionCheck = new QCheckBox("Enable RoosPtxHack auto-detection", extensionsTab);
    m_enableRoosPtxHackAutoDetectionCheck->setChecked(true);
    extensionsLayout->addWidget(m_enableRoosPtxHackAutoDetectionCheck);

    QGroupBox* labelsGroup = new QGroupBox("Plot the following extension labels", extensionsTab);
    QGridLayout* labelsLayout = new QGridLayout(labelsGroup);

    // Build checkboxes for all supported labels (three columns for compact layout)
    const QStringList labels = Settings::getSupportedExtensionLabels();
    const int columns = 3;
    for (int i = 0; i < labels.size(); ++i)
    {
        const QString& lab = labels[i];
        QCheckBox* cb = new QCheckBox(lab, labelsGroup);
        cb->setChecked(true); // default; will be overridden by loadCurrentSettings()
        int row = i / columns;
        int col = i % columns;
        labelsLayout->addWidget(cb, row, col);
        m_extensionLabelCheckboxes.insert(lab, cb);
    }

    extensionsLayout->addWidget(labelsGroup);
    extensionsLayout->addStretch();

    m_tabWidget->addTab(extensionsTab, "Extensions");

    // ============================================================================
    // Safety Tab (System Profiles)
    // ============================================================================
    QWidget* safetyTab = new QWidget();
    QVBoxLayout* safetyLayout = new QVBoxLayout(safetyTab);

    QGroupBox* systemGroup = new QGroupBox("", safetyTab);
    QVBoxLayout* systemGroupLayout = new QVBoxLayout(systemGroup);

    QWidget* systemMetaWidget = new QWidget(systemGroup);
    QFormLayout* systemForm = new QFormLayout(systemMetaWidget);

    QWidget* systemSelectorRow = new QWidget(systemGroup);
    QHBoxLayout* systemSelectorLayout = new QHBoxLayout(systemSelectorRow);
    systemSelectorLayout->setContentsMargins(0, 0, 0, 0);
    systemSelectorLayout->setSpacing(6);
    m_systemProfileCombo = new QComboBox(systemGroup);
    m_addSystemProfileButton = new QPushButton("Add", systemGroup);
    m_removeSystemProfileButton = new QPushButton("Remove", systemGroup);
    systemSelectorLayout->addWidget(m_systemProfileCombo, 1);
    systemSelectorLayout->addWidget(m_addSystemProfileButton);
    systemSelectorLayout->addWidget(m_removeSystemProfileButton);
    systemForm->addRow("System Profile:", systemSelectorRow);

    m_systemAliasEdit = new QLineEdit(systemGroup);
    m_systemAliasEdit->setPlaceholderText("Optional, auto-generated as systemN");
    systemForm->addRow("Alias:", m_systemAliasEdit);

    m_systemMaxGradEdit = new QLineEdit(systemGroup);
    m_systemMaxGradEdit->setPlaceholderText("Optional, mT/m");
    systemForm->addRow("maxGrad (mT/m):", m_systemMaxGradEdit);

    m_systemMaxSlewEdit = new QLineEdit(systemGroup);
    m_systemMaxSlewEdit->setPlaceholderText("Optional, T/m/s");
    systemForm->addRow("maxSlew (T/m/s):", m_systemMaxSlewEdit);

    m_systemMaxB1Edit = new QLineEdit(systemGroup);
    m_systemMaxB1Edit->setPlaceholderText("Optional, uT");
    systemForm->addRow("maxB1 (uT):", m_systemMaxB1Edit);

    systemGroupLayout->addWidget(systemMetaWidget);

    QGroupBox* pnsGroup = new QGroupBox("PNS (Siemens only)", systemGroup);
    QFormLayout* pnsForm = new QFormLayout(pnsGroup);

    QWidget* systemAscRow = new QWidget(pnsGroup);
    QHBoxLayout* systemAscLayout = new QHBoxLayout(systemAscRow);
    systemAscLayout->setContentsMargins(0, 0, 0, 0);
    systemAscLayout->setSpacing(6);
    m_systemAscPathEdit = new QLineEdit(pnsGroup);
    m_systemAscBrowseButton = new QPushButton("Browse...", pnsGroup);
    m_systemAscClearButton = new QPushButton("Clear", pnsGroup);
    systemAscLayout->addWidget(m_systemAscPathEdit, 1);
    systemAscLayout->addWidget(m_systemAscBrowseButton);
    systemAscLayout->addWidget(m_systemAscClearButton);
    pnsForm->addRow("Siemens ASC Path:", systemAscRow);

    QWidget* pnsChannelsRow = new QWidget(pnsGroup);
    QHBoxLayout* pnsChannelsLayout = new QHBoxLayout(pnsChannelsRow);
    pnsChannelsLayout->setContentsMargins(0, 0, 0, 0);
    pnsChannelsLayout->setSpacing(10);
    m_pnsShowXCheck = new QCheckBox("X", pnsGroup);
    m_pnsShowYCheck = new QCheckBox("Y", pnsGroup);
    m_pnsShowZCheck = new QCheckBox("Z", pnsGroup);
    m_pnsShowNormCheck = new QCheckBox("Norm", pnsGroup);
    pnsChannelsLayout->addWidget(m_pnsShowXCheck);
    pnsChannelsLayout->addWidget(m_pnsShowYCheck);
    pnsChannelsLayout->addWidget(m_pnsShowZCheck);
    pnsChannelsLayout->addWidget(m_pnsShowNormCheck);
    pnsChannelsLayout->addStretch();
    pnsForm->addRow("Display:", pnsChannelsRow);

    systemGroupLayout->addWidget(pnsGroup);

    safetyLayout->addWidget(systemGroup);
    safetyLayout->addStretch();

    m_tabWidget->addTab(safetyTab, "Safety");
    
    // Add tab widget to main layout
    mainLayout->addWidget(m_tabWidget);
    
    // ============================================================================
    // Button Layout
    // ============================================================================
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum));
    
    m_resetButton = new QPushButton("Reset to Default", this);
    m_cancelButton = new QPushButton("Cancel", this);
    m_applyButton = new QPushButton("Apply", this);
    m_okButton = new QPushButton("OK", this);
    
    buttonLayout->addWidget(m_resetButton);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(m_okButton);
    
    mainLayout->addLayout(buttonLayout);
    
    // Connect signals
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsDialog::onApplyClicked);
    connect(m_okButton, &QPushButton::clicked, this, &SettingsDialog::onOKClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &SettingsDialog::onCancelClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &SettingsDialog::onResetClicked);
    connect(m_gammaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &SettingsDialog::onGammaComboChanged);
    connect(m_zoomModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onZoomModeChanged);
    connect(m_panWheelCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onPanWheelToggled);
    connect(m_systemProfileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onSystemProfileChanged);
    connect(m_addSystemProfileButton, &QPushButton::clicked,
            this, &SettingsDialog::onAddSystemProfileClicked);
    connect(m_removeSystemProfileButton, &QPushButton::clicked,
            this, &SettingsDialog::onRemoveSystemProfileClicked);
    connect(m_systemAscBrowseButton, &QPushButton::clicked,
            this, &SettingsDialog::onBrowseSystemAscPath);
    connect(m_systemAscClearButton, &QPushButton::clicked,
            this, &SettingsDialog::onClearSystemAscPath);
    connect(m_systemAliasEdit, &QLineEdit::textChanged,
            this, [this]() { syncCurrentSystemProfileDraft(true); });
    connect(m_systemAscPathEdit, &QLineEdit::textChanged,
            this, [this]() { syncCurrentSystemProfileDraft(false); });
    connect(m_systemMaxGradEdit, &QLineEdit::textChanged,
            this, [this]() { syncCurrentSystemProfileDraft(false); });
    connect(m_systemMaxSlewEdit, &QLineEdit::textChanged,
            this, [this]() { syncCurrentSystemProfileDraft(false); });
    connect(m_systemMaxB1Edit, &QLineEdit::textChanged,
            this, [this]() { syncCurrentSystemProfileDraft(false); });
    connect(m_axisOrderList, &QListWidget::itemSelectionChanged,
            this, &SettingsDialog::onAxisOrderSelectionChanged);
    connect(m_axisOrderList->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() { onAxisOrderRowsMoved(); });
    connect(m_moveAxisTopButton, &QPushButton::clicked,
            this, &SettingsDialog::onMoveAxisTopClicked);
    connect(m_moveAxisUpButton, &QPushButton::clicked,
            this, &SettingsDialog::onMoveAxisUpClicked);
    connect(m_moveAxisDownButton, &QPushButton::clicked,
            this, &SettingsDialog::onMoveAxisDownClicked);
    connect(m_moveAxisBottomButton, &QPushButton::clicked,
            this, &SettingsDialog::onMoveAxisBottomClicked);
}

void SettingsDialog::loadCurrentSettings()
{
    Settings& settings = Settings::getInstance();
    WaveformDrawer* waveformDrawer = nullptr;
    if (MainWindow* mainWindow = qobject_cast<MainWindow*>(parentWidget()))
        waveformDrawer = mainWindow->getWaveformDrawer();
    
    // Store original settings
    m_originalGradientUnit = settings.getGradientUnit();
    m_originalSlewUnit = settings.getSlewUnit();
    m_originalTimeUnit = settings.getTimeUnit();
    m_originalTrajectoryUnit = settings.getTrajectoryUnit();
    m_originalTrajectoryColormap = settings.getTrajectoryColormap();
    m_originalGamma = settings.getGamma();
    m_originalLogLevel = settings.getLogLevel();
    m_originalZoomInputMode = settings.getZoomInputMode();
    m_originalZoomInputMode = settings.getZoomInputMode();
    m_originalPanWheelEnabled = settings.getPanWheelEnabled();
    m_originalShowExtensionTooltip = settings.getShowExtensionTooltip();
    m_originalEnableRoosPtxHackAutoDetection = settings.getEnableRoosPtxHackAutoDetection();
    m_originalSystemProfiles = settings.getSystemProfiles();
    m_originalActiveSystemProfileAlias = settings.getActiveSystemProfileAlias();
    m_systemProfilesDraft = m_originalSystemProfiles;
    m_originalPnsShowX = settings.getPnsChannelVisibleX();
    m_originalPnsShowY = settings.getPnsChannelVisibleY();
    m_originalPnsShowZ = settings.getPnsChannelVisibleZ();
    m_originalPnsShowNorm = settings.getPnsChannelVisibleNorm();

    // Store original extension label states
    m_originalExtensionLabelStates.clear();
    const QStringList labels = Settings::getSupportedExtensionLabels();
    for (const QString& lab : labels)
    {
        bool enabled = settings.isExtensionLabelEnabled(lab);
        m_originalExtensionLabelStates.insert(lab, enabled);
    }
    
    // Load current settings into UI
    int timeIdx = m_timeUnitCombo->findData(static_cast<int>(m_originalTimeUnit));
    if (timeIdx >= 0) {
        m_timeUnitCombo->setCurrentIndex(timeIdx);
    }

    m_gradientUnitCombo->setCurrentIndex(static_cast<int>(m_originalGradientUnit));
    m_slewUnitCombo->setCurrentIndex(static_cast<int>(m_originalSlewUnit));
    m_trajectoryUnitCombo->setCurrentIndex(static_cast<int>(m_originalTrajectoryUnit));
    // Trajectory colormap (select by enum ordinal)
    {
        int idx = m_trajectoryColormapCombo->findData(static_cast<int>(m_originalTrajectoryColormap));
        if (idx >= 0)
            m_trajectoryColormapCombo->setCurrentIndex(idx);
    }
    // Select by value (data), not by index, since combo order is not enum ordinal
    {
        int idx = m_logLevelCombo->findData(static_cast<int>(m_originalLogLevel));
        if (idx >= 0) m_logLevelCombo->setCurrentIndex(idx);
    }
    // Settings path
    m_settingsPathValue->setText(Settings::getInstance().getSettingsFilePath());

    if (waveformDrawer)
        populateAxisOrderList(waveformDrawer->getAxesOrder());
    else
        populateAxisOrderList(QStringList() << "RF mag" << "PNS" << "GZ" << "GY" << "GX" << "RF/ADC ph" << "ADC/labels");

    // Extensions: sync checkboxes from settings
    if (m_showExtensionTooltipCheck)
        m_showExtensionTooltipCheck->setChecked(settings.getShowExtensionTooltip());
    if (m_enableRoosPtxHackAutoDetectionCheck)
        m_enableRoosPtxHackAutoDetectionCheck->setChecked(settings.getEnableRoosPtxHackAutoDetection());

    for (auto it = m_extensionLabelCheckboxes.begin(); it != m_extensionLabelCheckboxes.end(); ++it)
    {
        const QString& lab = it.key();
        QCheckBox* cb = it.value();
        if (!cb) continue;
        bool enabled = settings.isExtensionLabelEnabled(lab);
        cb->setChecked(enabled);
    }

    // Interactions
    int zoomIndex = (m_originalZoomInputMode == Settings::ZoomInputMode::Wheel) ? 1 : 0;
    m_zoomModeCombo->setCurrentIndex(zoomIndex);
    m_panWheelCheck->setChecked(m_originalPanWheelEnabled);
    updateInteractionControlsForExclusivity();

    if (m_systemProfileCombo)
    {
        const QSignalBlocker blocker(m_systemProfileCombo);
        m_systemProfileCombo->clear();
        int selectedIndex = -1;
        for (int i = 0; i < m_systemProfilesDraft.size(); ++i)
        {
            const Settings::SystemProfile& profile = m_systemProfilesDraft[i];
            m_systemProfileCombo->addItem(profile.alias.trimmed().isEmpty() ? QStringLiteral("(auto)") : profile.alias);
            if (profile.alias == m_originalActiveSystemProfileAlias)
                selectedIndex = i;
        }
        if (selectedIndex < 0 && m_systemProfileCombo->count() > 0)
            selectedIndex = 0;
        m_systemProfileCombo->setCurrentIndex(selectedIndex);
    }
    loadCurrentSystemProfileIntoEditor();
    if (m_removeSystemProfileButton)
        m_removeSystemProfileButton->setEnabled(!m_systemProfilesDraft.isEmpty());
    if (m_pnsShowXCheck) m_pnsShowXCheck->setChecked(settings.getPnsChannelVisibleX());
    if (m_pnsShowYCheck) m_pnsShowYCheck->setChecked(settings.getPnsChannelVisibleY());
    if (m_pnsShowZCheck) m_pnsShowZCheck->setChecked(settings.getPnsChannelVisibleZ());
    if (m_pnsShowNormCheck) m_pnsShowNormCheck->setChecked(settings.getPnsChannelVisibleNorm());
    
    // Load gamma - find closest match in combo box
    double currentGamma = settings.getGamma();
    
    // Check if current gamma matches any predefined value
    bool found = false;
    for (int i = 0; i < m_gammaCombo->count() - 1; i++) { // -1 to exclude "Custom..." item
        double itemGamma = m_gammaCombo->itemData(i).toDouble();
        if (qAbs(itemGamma - currentGamma) < 1e3) { // Within 1000 Hz/T tolerance
            m_gammaCombo->setCurrentIndex(i);
            found = true;
            break;
        }
    }
    
    // If no match found, create a custom entry for the current value
    if (!found) {
        double currentGammaMHz = currentGamma / 1e6; // Convert to MHz/T
        QString customDisplayText = QString("Custom - %1 MHz/T").arg(currentGammaMHz, 0, 'f', 3);
        
        // Insert custom item before "Custom..." item
        int insertIndex = m_gammaCombo->count() - 1;
        m_gammaCombo->insertItem(insertIndex, customDisplayText, currentGamma);
        m_gammaCombo->setCurrentIndex(insertIndex);
    }

    // Old time-based LOD settings removed - replaced with complexity-based LOD system
}

bool SettingsDialog::applySettings()
{
    Settings& settings = Settings::getInstance();
    WaveformDrawer* waveformDrawer = nullptr;
    if (MainWindow* mainWindow = qobject_cast<MainWindow*>(parentWidget()))
        waveformDrawer = mainWindow->getWaveformDrawer();
    
    // Apply gradient unit
    Settings::GradientUnit gradientUnit = static_cast<Settings::GradientUnit>(m_gradientUnitCombo->currentIndex());
    settings.setGradientUnit(gradientUnit);
    
    // Apply slew unit
    Settings::SlewUnit slewUnit = static_cast<Settings::SlewUnit>(m_slewUnitCombo->currentIndex());
    settings.setSlewUnit(slewUnit);

    // Apply trajectory unit
    Settings::TrajectoryUnit trajUnit = static_cast<Settings::TrajectoryUnit>(m_trajectoryUnitCombo->currentIndex());
    settings.setTrajectoryUnit(trajUnit);

    // Apply trajectory colormap
    Settings::TrajectoryColormap trajMap =
        static_cast<Settings::TrajectoryColormap>(m_trajectoryColormapCombo->currentData().toInt());
    settings.setTrajectoryColormap(trajMap);
    
    // Apply time unit
    Settings::TimeUnit timeUnit = static_cast<Settings::TimeUnit>(m_timeUnitCombo->currentData().toInt());
    settings.setTimeUnit(timeUnit);
    
    // Apply gamma
    double gamma = m_gammaCombo->currentData().toDouble();
    settings.setGamma(gamma);
    
    // Apply log level
    Settings::LogLevel logLevel = static_cast<Settings::LogLevel>(m_logLevelCombo->currentData().toInt());
    settings.setLogLevel(logLevel);

    // Apply extension tooltip setting
    if (m_showExtensionTooltipCheck)
        settings.setShowExtensionTooltip(m_showExtensionTooltipCheck->isChecked());
    if (m_enableRoosPtxHackAutoDetectionCheck)
        settings.setEnableRoosPtxHackAutoDetection(m_enableRoosPtxHackAutoDetectionCheck->isChecked());

    // Apply extension label visibility
    for (auto it = m_extensionLabelCheckboxes.begin(); it != m_extensionLabelCheckboxes.end(); ++it)
    {
        const QString& lab = it.key();
        QCheckBox* cb = it.value();
        if (!cb) continue;
        settings.setExtensionLabelEnabled(lab, cb->isChecked());
    }

    // Apply interactions
    Settings::ZoomInputMode zoomMode = static_cast<Settings::ZoomInputMode>(m_zoomModeCombo->currentData().toInt());
    settings.setZoomInputMode(zoomMode);
    // Enforce exclusivity: if zoom is Wheel, pan wheel must be false
    bool panWheel = (zoomMode == Settings::ZoomInputMode::Wheel) ? false : m_panWheelCheck->isChecked();
    settings.setPanWheelEnabled(panWheel);

    QVector<Settings::SystemProfile> nextProfiles = m_systemProfilesDraft;
    const int idx = currentSystemProfileIndex();
    if (idx >= 0 && idx < nextProfiles.size())
    {
        bool ok = false;
        QString error;
        Settings::SystemProfile profile = buildCurrentSystemProfileFromEditor(&ok, &error);
        if (!ok)
        {
            QMessageBox::warning(this, tr("System profile warning"), error);
            return false;
        }
        nextProfiles[idx] = profile;
    }

    QSet<QString> aliases;
    for (int i = 0; i < nextProfiles.size(); ++i)
    {
        Settings::SystemProfile& profile = nextProfiles[i];
        profile.alias = resolvedAliasForInput(profile.alias, i);
        const QString aliasKey = profile.alias.toCaseFolded();
        if (aliases.contains(aliasKey))
        {
            QMessageBox::warning(this,
                                 tr("Duplicate system alias"),
                                 tr("System alias \"%1\" is used more than once. Please choose unique aliases.")
                                     .arg(profile.alias));
            return false;
        }
        aliases.insert(aliasKey);

        if (!profile.ascPath.trimmed().isEmpty())
        {
            PnsCalculator::Hardware hw;
            QString parseError;
            if (!QFileInfo::exists(profile.ascPath))
            {
                QMessageBox::warning(this, tr("System profile warning"),
                                     tr("ASC path does not exist for system \"%1\".\nPlease select a valid ASC file before applying.")
                                         .arg(profile.alias));
                return false;
            }
            if (!PnsCalculator::parseAscFile(profile.ascPath, hw, &parseError))
            {
                QMessageBox::warning(this, tr("System profile warning"),
                                     tr("ASC file is invalid for system \"%1\":\n%2\n\nPlease select a valid ASC file before applying.")
                                         .arg(profile.alias, parseError));
                return false;
            }
        }
    }

    settings.setSystemProfiles(nextProfiles);
    if (m_systemProfileCombo && m_systemProfileCombo->currentIndex() >= 0 && m_systemProfileCombo->currentIndex() < nextProfiles.size())
        settings.setActiveSystemProfileAlias(nextProfiles[m_systemProfileCombo->currentIndex()].alias);
    if (m_pnsShowXCheck) settings.setPnsChannelVisibleX(m_pnsShowXCheck->isChecked());
    if (m_pnsShowYCheck) settings.setPnsChannelVisibleY(m_pnsShowYCheck->isChecked());
    if (m_pnsShowZCheck) settings.setPnsChannelVisibleZ(m_pnsShowZCheck->isChecked());
    if (m_pnsShowNormCheck) settings.setPnsChannelVisibleNorm(m_pnsShowNormCheck->isChecked());
    if (waveformDrawer)
        waveformDrawer->applyAxesOrderAndSave(currentAxisOrderFromList());

    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    
    qDebug() << "Settings applied:";
    qDebug() << "  Gradient Unit:" << settings.getGradientUnitString();
    qDebug() << "  Slew Unit:" << settings.getSlewUnitString();
    qDebug() << "  Time Unit:" << settings.getTimeUnitString();
    qDebug() << "  Gamma:" << gamma << "Hz/T";
    qDebug() << "  Log Level:" << settings.getLogLevelString();
    qDebug() << "  Zoom Input Mode:" << settings.getZoomInputModeString();
    qDebug() << "  Pan Wheel Enabled:" << settings.getPanWheelEnabled();
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    return true;
}

void SettingsDialog::onApplyClicked()
{
    if (applySettings())
        loadCurrentSettings();
}

void SettingsDialog::onOKClicked()
{
    if (applySettings())
        accept();
}

void SettingsDialog::onCancelClicked()
{
    Settings& settings = Settings::getInstance();
    settings.setGradientUnit(m_originalGradientUnit);
    settings.setSlewUnit(m_originalSlewUnit);
    settings.setTimeUnit(m_originalTimeUnit);
    settings.setTrajectoryUnit(m_originalTrajectoryUnit);
    settings.setTrajectoryColormap(m_originalTrajectoryColormap);
    settings.setGamma(m_originalGamma);
    settings.setLogLevel(m_originalLogLevel);
    settings.setShowExtensionTooltip(m_originalShowExtensionTooltip);
    settings.setEnableRoosPtxHackAutoDetection(m_originalEnableRoosPtxHackAutoDetection);
    settings.setSystemProfiles(m_originalSystemProfiles);
    settings.setActiveSystemProfileAlias(m_originalActiveSystemProfileAlias);
    settings.setPnsChannelVisibleX(m_originalPnsShowX);
    settings.setPnsChannelVisibleY(m_originalPnsShowY);
    settings.setPnsChannelVisibleZ(m_originalPnsShowZ);
    settings.setPnsChannelVisibleNorm(m_originalPnsShowNorm);
    // Restore original extension label states
    for (auto it = m_originalExtensionLabelStates.constBegin(); it != m_originalExtensionLabelStates.constEnd(); ++it)
    {
        settings.setExtensionLabelEnabled(it.key(), it.value());
    }
    
    reject();
}

void SettingsDialog::onResetClicked()
{
    int ret = QMessageBox::question(this, "Reset Settings", 
                                   "Are you sure you want to reset all settings to default values?",
                                   QMessageBox::Yes | QMessageBox::No);
    
    if (ret == QMessageBox::Yes) {
        // Reset settings to defaults
        Settings& settings = Settings::getInstance();
        settings.resetToDefaults();
        
        // Reload UI with default values
        loadCurrentSettings();
        
        QMessageBox::information(this, "Settings Reset", 
                               "All settings have been reset to default values and saved to settings.json");
    }
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    loadCurrentSettings();
}

void SettingsDialog::onGammaComboChanged(int index)
{
    // If "Custom..." is selected, show input dialog
    if (index == m_gammaCombo->count() - 1) {
        showCustomGammaDialog();
        
        // Always reset to first item after showing custom dialog
        // This prevents the dialog from staying on "Custom..." option
        m_gammaCombo->setCurrentIndex(0);
    }
}

void SettingsDialog::showCustomGammaDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Add Custom Nucleus");
    dialog.setModal(true);
    dialog.resize(400, 200);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    // Instructions
    QLabel* instructionLabel = new QLabel("Enter nucleus name and gyromagnetic ratio:", &dialog);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);
    
    // Form layout for inputs
    QFormLayout* formLayout = new QFormLayout();
    
    QLineEdit* nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText("e.g., ¹⁶O (Oxygen-16)");
    formLayout->addRow(new QLabel("Nucleus Name:", &dialog), nameEdit);
    
    QLineEdit* valueEdit = new QLineEdit(&dialog);
    valueEdit->setPlaceholderText("e.g., 5.771");
    formLayout->addRow(new QLabel("Gyromagnetic Ratio (MHz/T):", &dialog), valueEdit);
    
    layout->addLayout(formLayout);
    
    // Example
    QLabel* exampleLabel = new QLabel("Example: ¹⁶O (Oxygen-16) - 5.771 MHz/T", &dialog);
    exampleLabel->setStyleSheet("color: gray; font-style: italic;");
    layout->addWidget(exampleLabel);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    
    QPushButton* cancelButton = new QPushButton("Cancel", &dialog);
    QPushButton* okButton = new QPushButton("Add", &dialog);
    
    buttonLayout->addWidget(cancelButton);
    buttonLayout->addWidget(okButton);
    
    layout->addLayout(buttonLayout);
    
    // Connect buttons
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    
    // Set focus and return key handling
    nameEdit->setFocus();
    connect(nameEdit, &QLineEdit::returnPressed, [valueEdit]() { valueEdit->setFocus(); });
    connect(valueEdit, &QLineEdit::returnPressed, [&]() {
        // Same logic as OK button
        QString nucleusName = nameEdit->text().trimmed();
        QString valueText = valueEdit->text().trimmed();
        
        if (!nucleusName.isEmpty() && !valueText.isEmpty()) {
            bool ok;
            double gammaMHz = valueText.toDouble(&ok);
            
            if (ok && gammaMHz > 0) {
                // Create display text
                QString displayText = QString("%1 - %2 MHz/T").arg(nucleusName).arg(gammaMHz, 0, 'f', 3);
                double gammaHz = gammaMHz * 1e6;
                
                // Insert new item before "Custom..." item
                int insertIndex = m_gammaCombo->count() - 1;
                
                // Temporarily disconnect the signal to prevent recursive calls
                disconnect(m_gammaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
                          this, &SettingsDialog::onGammaComboChanged);
                
                m_gammaCombo->insertItem(insertIndex, displayText, gammaHz);
                m_gammaCombo->setCurrentIndex(insertIndex);
                
                // Reconnect the signal
                connect(m_gammaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
                        this, &SettingsDialog::onGammaComboChanged);
                
                // Close the dialog immediately
                dialog.accept();
            } else {
                QMessageBox::warning(this, "Invalid Input", 
                    "Please enter a valid positive number for the gyromagnetic ratio.");
            }
        } else {
            QMessageBox::warning(this, "Invalid Input", 
                "Please enter both nucleus name and gyromagnetic ratio.");
        }
    });
    
    // Connect OK button to handle the input
    connect(okButton, &QPushButton::clicked, [&]() {
        QString nucleusName = nameEdit->text().trimmed();
        QString valueText = valueEdit->text().trimmed();
        
        if (!nucleusName.isEmpty() && !valueText.isEmpty()) {
            bool ok;
            double gammaMHz = valueText.toDouble(&ok);
            
            if (ok && gammaMHz > 0) {
                // Create display text
                QString displayText = QString("%1 - %2 MHz/T").arg(nucleusName).arg(gammaMHz, 0, 'f', 3);
                double gammaHz = gammaMHz * 1e6;
                
                // Insert new item before "Custom..." item
                int insertIndex = m_gammaCombo->count() - 1;
                
                // Temporarily disconnect the signal to prevent recursive calls
                disconnect(m_gammaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
                          this, &SettingsDialog::onGammaComboChanged);
                
                m_gammaCombo->insertItem(insertIndex, displayText, gammaHz);
                m_gammaCombo->setCurrentIndex(insertIndex);
                
                // Reconnect the signal
                connect(m_gammaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
                        this, &SettingsDialog::onGammaComboChanged);
                
                // Close the dialog immediately
                dialog.accept();
            } else {
                QMessageBox::warning(this, "Invalid Input", 
                    "Please enter a valid positive number for the gyromagnetic ratio.");
            }
        } else {
            QMessageBox::warning(this, "Invalid Input", 
                "Please enter both nucleus name and gyromagnetic ratio.");
        }
    });
    
    // Show dialog
    dialog.exec();
}

void SettingsDialog::populateAxisOrderList(const QStringList& order)
{
    if (!m_axisOrderList)
        return;

    const QSignalBlocker blocker(m_axisOrderList);
    const QString selectedText = m_axisOrderList->currentItem() ? m_axisOrderList->currentItem()->text() : QString();

    m_axisOrderList->clear();
    for (const QString& axisName : order)
        m_axisOrderList->addItem(axisName);

    int selectedRow = -1;
    if (!selectedText.isEmpty())
    {
        for (int row = 0; row < m_axisOrderList->count(); ++row)
        {
            if (m_axisOrderList->item(row)->text() == selectedText)
            {
                selectedRow = row;
                break;
            }
        }
    }
    if (selectedRow < 0 && m_axisOrderList->count() > 0)
        selectedRow = 0;

    if (selectedRow >= 0)
        m_axisOrderList->setCurrentRow(selectedRow);

    updateAxisOrderButtons();
}

QStringList SettingsDialog::currentAxisOrderFromList() const
{
    QStringList order;
    if (!m_axisOrderList)
        return order;

    for (int row = 0; row < m_axisOrderList->count(); ++row)
        order.append(m_axisOrderList->item(row)->text());
    return order;
}

void SettingsDialog::moveSelectedAxisItem(int delta)
{
    if (!m_axisOrderList || delta == 0)
        return;

    const int currentRow = m_axisOrderList->currentRow();
    if (currentRow < 0)
        return;

    const int targetRow = currentRow + delta;
    if (targetRow < 0 || targetRow >= m_axisOrderList->count())
        return;

    QListWidgetItem* item = m_axisOrderList->takeItem(currentRow);
    m_axisOrderList->insertItem(targetRow, item);
    m_axisOrderList->setCurrentRow(targetRow);
    updateAxisOrderButtons();
}

void SettingsDialog::updateAxisOrderButtons()
{
    if (!m_axisOrderList || !m_moveAxisTopButton || !m_moveAxisUpButton ||
        !m_moveAxisDownButton || !m_moveAxisBottomButton)
        return;

    const int currentRow = m_axisOrderList->currentRow();
    const int lastRow = m_axisOrderList->count() - 1;
    const bool hasSelection = currentRow >= 0;
    m_moveAxisTopButton->setEnabled(hasSelection && currentRow > 0);
    m_moveAxisUpButton->setEnabled(hasSelection && currentRow > 0);
    m_moveAxisDownButton->setEnabled(hasSelection && currentRow >= 0 && currentRow < lastRow);
    m_moveAxisBottomButton->setEnabled(hasSelection && currentRow >= 0 && currentRow < lastRow);
}

void SettingsDialog::onAxisOrderSelectionChanged()
{
    updateAxisOrderButtons();
}

void SettingsDialog::onAxisOrderRowsMoved()
{
    updateAxisOrderButtons();
}

void SettingsDialog::onMoveAxisTopClicked()
{
    if (!m_axisOrderList)
        return;

    const int currentRow = m_axisOrderList->currentRow();
    if (currentRow <= 0)
        return;

    QListWidgetItem* item = m_axisOrderList->takeItem(currentRow);
    m_axisOrderList->insertItem(0, item);
    m_axisOrderList->setCurrentRow(0);
    updateAxisOrderButtons();
}

void SettingsDialog::onMoveAxisUpClicked()
{
    moveSelectedAxisItem(-1);
}

void SettingsDialog::onMoveAxisDownClicked()
{
    moveSelectedAxisItem(1);
}

void SettingsDialog::onMoveAxisBottomClicked()
{
    if (!m_axisOrderList)
        return;

    const int currentRow = m_axisOrderList->currentRow();
    const int lastRow = m_axisOrderList->count() - 1;
    if (currentRow < 0 || currentRow >= lastRow)
        return;

    QListWidgetItem* item = m_axisOrderList->takeItem(currentRow);
    m_axisOrderList->insertItem(lastRow, item);
    m_axisOrderList->setCurrentRow(lastRow);
    updateAxisOrderButtons();
}

void SettingsDialog::onZoomModeChanged(int index)
{
    Q_UNUSED(index);
    updateInteractionControlsForExclusivity();
}

void SettingsDialog::onPanWheelToggled(bool checked)
{
    Q_UNUSED(checked);
    updateInteractionControlsForExclusivity();
}

void SettingsDialog::updateInteractionControlsForExclusivity()
{
    // If Zoom uses Mouse wheel, Pan:Mouse wheel must be disabled and unchecked
    bool zoomIsWheel = (static_cast<Settings::ZoomInputMode>(m_zoomModeCombo->currentData().toInt()) == Settings::ZoomInputMode::Wheel);
    if (zoomIsWheel)
    {
        if (m_panWheelCheck->isChecked())
            m_panWheelCheck->setChecked(false);
        m_panWheelCheck->setEnabled(false);
    }
    else
    {
        m_panWheelCheck->setEnabled(true);
    }

    // If Pan:Mouse wheel is checked, ensure Zoom cannot be "Mouse wheel"
    if (m_panWheelCheck->isChecked())
    {
        if (static_cast<Settings::ZoomInputMode>(m_zoomModeCombo->currentData().toInt()) == Settings::ZoomInputMode::Wheel)
        {
            // Force to Ctrl+Mouse wheel
            m_zoomModeCombo->setCurrentIndex(0);
        }
        // Optionally disable the "Mouse wheel" option visually
        if (auto* model = qobject_cast<QStandardItemModel*>(m_zoomModeCombo->model()))
        {
            if (QStandardItem* item = model->item(1))
                item->setEnabled(false);
        }
    }
    else
    {
        if (auto* model = qobject_cast<QStandardItemModel*>(m_zoomModeCombo->model()))
        {
            if (QStandardItem* item = model->item(1))
                item->setEnabled(true);
        }
    }
}

void SettingsDialog::syncCurrentSystemProfileDraft(bool updateComboText)
{
    const int idx = currentSystemProfileIndex();
    if (idx < 0 || idx >= m_systemProfilesDraft.size())
        return;

    bool ok = false;
    Settings::SystemProfile profile = buildCurrentSystemProfileFromEditor(&ok, nullptr);
    if (!ok)
        return;

    m_systemProfilesDraft[idx] = profile;
    if (updateComboText && m_systemProfileCombo)
    {
        const QString label = profile.alias.trimmed().isEmpty()
            ? QStringLiteral("(auto)")
            : profile.alias.trimmed();
        m_systemProfileCombo->setItemText(idx, label);
    }
}

int SettingsDialog::currentSystemProfileIndex() const
{
    return m_systemProfileCombo ? m_systemProfileCombo->currentIndex() : -1;
}

QString SettingsDialog::resolvedAliasForInput(const QString& alias, int profileIndex) const
{
    QString trimmed = alias.trimmed();
    if (trimmed.isEmpty())
    {
        QSet<QString> used;
        for (int i = 0; i < m_systemProfilesDraft.size(); ++i)
        {
            if (i == profileIndex)
                continue;
            used.insert(m_systemProfilesDraft[i].alias.toCaseFolded());
        }
        for (int i = 1; ; ++i)
        {
            const QString candidate = QStringLiteral("system%1").arg(i);
            if (!used.contains(candidate.toCaseFolded()))
                return candidate;
        }
    }
    return trimmed;
}

Settings::SystemProfile SettingsDialog::buildCurrentSystemProfileFromEditor(bool* ok, QString* error) const
{
    if (ok) *ok = false;
    Settings::SystemProfile profile;
    profile.alias = m_systemAliasEdit ? m_systemAliasEdit->text().trimmed() : QString();
    profile.ascPath = m_systemAscPathEdit ? m_systemAscPathEdit->text().trimmed() : QString();
    profile.maxGrad = std::numeric_limits<double>::quiet_NaN();
    profile.maxSlew = std::numeric_limits<double>::quiet_NaN();
    profile.maxB1 = std::numeric_limits<double>::quiet_NaN();

    auto parseOptionalPositive = [&](QLineEdit* edit, const QString& label, double& out) -> bool {
        if (!edit)
            return true;
        const QString text = edit->text().trimmed();
        if (text.isEmpty())
            return true;
        bool okLocal = false;
        const double value = text.toDouble(&okLocal);
        if (!okLocal || !std::isfinite(value) || value <= 0.0)
        {
            if (error)
                *error = tr("%1 must be a positive number or left empty.").arg(label);
            return false;
        }
        out = value;
        return true;
    };

    if (!parseOptionalPositive(m_systemMaxGradEdit, tr("maxGrad"), profile.maxGrad))
        return profile;
    if (!parseOptionalPositive(m_systemMaxSlewEdit, tr("maxSlew"), profile.maxSlew))
        return profile;
    if (!parseOptionalPositive(m_systemMaxB1Edit, tr("maxB1"), profile.maxB1))
        return profile;

    if (ok) *ok = true;
    return profile;
}

void SettingsDialog::loadCurrentSystemProfileIntoEditor()
{
    const int idx = currentSystemProfileIndex();
    const bool hasProfile = idx >= 0 && idx < m_systemProfilesDraft.size();
    const Settings::SystemProfile profile = hasProfile ? m_systemProfilesDraft[idx] : Settings::SystemProfile{};

    if (m_systemAliasEdit) m_systemAliasEdit->setText(hasProfile ? profile.alias : QString());
    if (m_systemAscPathEdit) m_systemAscPathEdit->setText(hasProfile ? profile.ascPath : QString());
    if (m_systemMaxGradEdit)
        m_systemMaxGradEdit->setText(std::isfinite(profile.maxGrad) ? QString::number(profile.maxGrad) : QString());
    if (m_systemMaxSlewEdit)
        m_systemMaxSlewEdit->setText(std::isfinite(profile.maxSlew) ? QString::number(profile.maxSlew) : QString());
    if (m_systemMaxB1Edit)
        m_systemMaxB1Edit->setText(std::isfinite(profile.maxB1) ? QString::number(profile.maxB1) : QString());

    if (m_systemAliasEdit) m_systemAliasEdit->setEnabled(hasProfile);
    if (m_systemAscPathEdit) m_systemAscPathEdit->setEnabled(hasProfile);
    if (m_systemAscBrowseButton) m_systemAscBrowseButton->setEnabled(hasProfile);
    if (m_systemAscClearButton) m_systemAscClearButton->setEnabled(hasProfile);
    if (m_systemMaxGradEdit) m_systemMaxGradEdit->setEnabled(hasProfile);
    if (m_systemMaxSlewEdit) m_systemMaxSlewEdit->setEnabled(hasProfile);
    if (m_systemMaxB1Edit) m_systemMaxB1Edit->setEnabled(hasProfile);
    if (m_removeSystemProfileButton) m_removeSystemProfileButton->setEnabled(hasProfile);
}

void SettingsDialog::onSystemProfileChanged(int index)
{
    Q_UNUSED(index);
    loadCurrentSystemProfileIntoEditor();
}

void SettingsDialog::onAddSystemProfileClicked()
{
    Settings::SystemProfile profile;
    profile.alias.clear();
    profile.maxGrad = std::numeric_limits<double>::quiet_NaN();
    profile.maxSlew = std::numeric_limits<double>::quiet_NaN();
    profile.maxB1 = std::numeric_limits<double>::quiet_NaN();
    m_systemProfilesDraft.append(profile);
    if (m_systemProfileCombo)
    {
        m_systemProfileCombo->addItem(QStringLiteral("(auto)"));
        m_systemProfileCombo->setCurrentIndex(m_systemProfileCombo->count() - 1);
    }
    loadCurrentSystemProfileIntoEditor();
}

void SettingsDialog::onRemoveSystemProfileClicked()
{
    const int idx = currentSystemProfileIndex();
    if (idx < 0 || idx >= m_systemProfilesDraft.size())
        return;

    m_systemProfilesDraft.removeAt(idx);
    if (m_systemProfileCombo)
    {
        const QSignalBlocker blocker(m_systemProfileCombo);
        m_systemProfileCombo->removeItem(idx);
        if (m_systemProfileCombo->count() > 0)
            m_systemProfileCombo->setCurrentIndex(std::min(idx, m_systemProfileCombo->count() - 1));
    }
    loadCurrentSystemProfileIntoEditor();
}

void SettingsDialog::onBrowseSystemAscPath()
{
    const QString currentPath = m_systemAscPathEdit ? m_systemAscPathEdit->text().trimmed() : QString();
    const QString startDir = QFileInfo(currentPath).exists()
        ? QFileInfo(currentPath).absolutePath()
        : QDir::homePath();

    QFileDialog::Options options;
    QWidget* parentForDialog = this;
#ifdef Q_OS_MAC
    options |= QFileDialog::DontUseNativeDialog;
    parentForDialog = nullptr;
#endif

    const QString selected = QFileDialog::getOpenFileName(
        parentForDialog,
        tr("Select ASC file"),
        startDir,
        tr("ASC files (*.asc);;All files (*.*)"),
        nullptr,
        options);
    if (!selected.isEmpty() && m_systemAscPathEdit)
        m_systemAscPathEdit->setText(selected);
}

void SettingsDialog::onClearSystemAscPath()
{
    if (m_systemAscPathEdit)
        m_systemAscPathEdit->clear();
}
