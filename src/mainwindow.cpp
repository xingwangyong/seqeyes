#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "InteractionHandler.h"
#include "PulseqLoader.h"
#include "TRManager.h"
#include "WaveformDrawer.h"
#include "SettingsDialog.h"
#include "LogTableDialog.h"
#include <QCommandLineParser>
#include "Settings.h"
#include "TrajectoryColormap.h"
#include "LogManager.h"

#include <QProgressBar>
#include <QFileInfo>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QDebug>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QWheelEvent> // For event overrides
#include <QMenuBar>
#include <QAction>
#include <QFontDatabase>
#include <QSplitter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QDir>
#include <QResizeEvent>
#include <QTimer>
#include <QImage>
#include <QVector>
#include <cmath>
#include <limits>
#include <algorithm>
#include <QPainter>
#include <QCoreApplication>

// Lightweight overlay widget for drawing trajectory crosshair without forcing full plot replots
class TrajectoryCrosshairOverlay : public QWidget
{
public:
    explicit TrajectoryCrosshairOverlay(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_enabled(false)
        , m_hasPos(false)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setVisible(false);
    }

    void setEnabledFlag(bool enabled)
    {
        m_enabled = enabled;
        if (!enabled)
        {
            m_hasPos = false;
            update();
            setVisible(false);
        }
    }

    void setCrosshairPos(const QPoint& pos)
    {
        if (!m_enabled)
            return;
        m_pos = pos;
        m_hasPos = true;
        setVisible(true);
        update();
    }

    void clearCrosshair()
    {
        m_hasPos = false;
        update();
        setVisible(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!m_enabled || !m_hasPos)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        QPen pen(QColor(80, 80, 80));
        pen.setStyle(Qt::DashLine);
        pen.setWidth(1);
        p.setPen(pen);
        const int x = m_pos.x();
        const int y = m_pos.y();
        if (x >= 0 && x < width())
        {
            p.drawLine(x, 0, x, height() - 1);
        }
        if (y >= 0 && y < height())
        {
            p.drawLine(0, y, width() - 1, y);
        }
    }

private:
    bool m_enabled;
    bool m_hasPos;
    QPoint m_pos;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      ui(new Ui::MainWindow),
      m_interactionHandler(nullptr),
      m_pulseqLoader(nullptr),
      m_trManager(nullptr),
      m_waveformDrawer(nullptr),
      m_pVersionLabel(nullptr),
      m_pProgressBar(nullptr),
      m_pCoordLabel(nullptr),
      m_settingsDialog(nullptr)
{
    ui->setupUi(this);
    setAcceptDrops(true);
    // Keep a simple default window title; show file name only after a sequence is loaded.
    setWindowTitle("SeqEyes");

    // Hide the top toolbar by default to save vertical space (especially for small tiled windows like --layout 211).
    // File/View menus already contain the core actions, and Measure Δt is added to View below.
    if (ui->toolBar)
        ui->toolBar->setVisible(false);

    // 1. Instantiate handlers
    m_interactionHandler = new InteractionHandler(this);
    m_pulseqLoader = new PulseqLoader(this);
    m_trManager = new TRManager(this);
    m_waveformDrawer = new WaveformDrawer(this);

    // 2. Create UI widgets managed by TRManager
    m_trManager->createWidgets();

    // 3. Initialize the plot figure
    m_waveformDrawer->InitSequenceFigure();

    // 4. Set up the main layout
    // The original layout from the UI file is removed and replaced programmatically
    QLayout* existingLayout = ui->centralwidget->layout();
    if (existingLayout) {
        delete existingLayout;
    }
    QVBoxLayout* mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(5, 5, 5, 5);

    // TRManager adds its widgets to the layout
    m_trManager->setupLayouts(mainLayout);

    // Add the plot area at the bottom
    setupPlotArea(mainLayout);
    ui->centralwidget->setLayout(mainLayout);

    // 5. Initialize other UI components
    InitStatusBar();
    m_pCoordLabel = new QLabel(this);
    // Use a pleasant monospace font for fixed-width alignment (fallback to system fixed font)
    {
        QStringList preferred = {
            "JetBrains Mono", "Fira Code", "Cascadia Mono", "Consolas",
            "Menlo", "DejaVu Sans Mono", "Source Code Pro", "SF Mono"
        };
        QFont chosen;
        bool found=false;
        const QStringList families = QFontDatabase().families();
        for (const QString& fam : preferred) {
            if (families.contains(fam)) { chosen = QFont(fam); found=true; break; }
        }
        if (!found) {
            chosen = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            chosen.setStyleHint(QFont::Monospace);
        }
        chosen.setStyleStrategy(QFont::PreferAntialias);
        m_pCoordLabel->setFont(chosen);
    }
    ui->statusbar->addWidget(m_pCoordLabel);

    // This needs to be called after the plot rects are created in InitSequenceFigure
    m_waveformDrawer->InitTracers();

    // 6. Connect all signals to slots
    InitSlots();
    m_trManager->connectSignals();

    // 6b. Install InteractionHandler as event filter now that it's constructed
    // Allow global key handling (e.g., Esc to exit measurement, pan/TR shortcuts) and plot-level events
    installEventFilter(m_interactionHandler);
    // customPlot is created by ui; ensure it receives key events when focused
    ui->customPlot->installEventFilter(m_interactionHandler);

    // 6b. Forward new log lines to the optional Log dialog, if open
    connect(&LogManager::getInstance(), &LogManager::logEntryAppended,
            this, [this](const QString& ts,
                         const QString& level,
                         const QString& category,
                         const QString& message,
                         const QString& origin) {
                // Lazy-created; only update if user has opened the log window
                if (auto* dlg = qobject_cast<LogTableDialog*>(findChild<LogTableDialog*>("__SeqEyesLogDialog")))
                {
                    LogManager::LogEntry e;
                    e.timestamp = ts;
                    e.level = level;
                    e.category = category;
                    e.message = message;
                    e.origin = origin;
                    dlg->appendEntry(e);
                }
            });
    
    // 7. Initialize settings dialog and menu
    m_settingsDialog = new SettingsDialog(this);
    setupSettingsMenu();
    
    // Connect settings change signal to update axis labels and trajectory view
    connect(&Settings::getInstance(), &Settings::settingsChanged, 
            m_waveformDrawer, &WaveformDrawer::updateAxisLabels);
    connect(&Settings::getInstance(), &Settings::settingsChanged,
            this, &MainWindow::onSettingsChanged);
    connect(&Settings::getInstance(), &Settings::timeUnitChanged,
            this, &MainWindow::onTimeUnitChanged);

    // 7. Install event filters
    m_trManager->installEventFilters();
}

void MainWindow::setLoadedFileTitle(const QString& filePath)
{
    m_loadedSeqFilePath = filePath;
    const QString base = "SeqEyes";
    QString name;
    
    // Priority 1: Command line --name option (highest priority)
    if (!m_customWindowTitle.isEmpty())
    {
        name = m_customWindowTitle;
    }
    // Priority 2: Name from [DEFINITIONS] section in .seq file
    else if (m_pulseqLoader)
    {
        auto seq = m_pulseqLoader->getSequence();
        if (seq)
        {
            std::string nameDef = seq->GetDefinitionStr("Name");
            if (!nameDef.empty())
            {
                name = QString::fromStdString(nameDef).trimmed();
            }
        }
    }
    
    // Priority 3: File name (fallback)
    if (name.isEmpty())
    {
        QFileInfo fi(filePath);
        name = fi.fileName();
    }
    
    if (name.isEmpty())
    {
        setWindowTitle(base);
        return;
    }
    setWindowTitle(QString("%1 - %2").arg(base, name));
}

void MainWindow::clearLoadedFileTitle()
{
    m_loadedSeqFilePath.clear();
    // If custom title is set, use it; otherwise use default
    if (!m_customWindowTitle.isEmpty())
    {
        setWindowTitle(m_customWindowTitle);
    }
    else
    {
        setWindowTitle("SeqEyes");
    }
}

MainWindow::~MainWindow()
{
    // Ensure cleanup order: delete PulseqLoader before UI widgets it references
    // to avoid accessing destroyed UI elements during loader's ClearPulseqCache.
    SAFE_DELETE(m_pulseqLoader);

    // Handlers are QObjects parented to MainWindow and will be deleted automatically.
    SAFE_DELETE(m_pVersionLabel);
    SAFE_DELETE(m_pProgressBar);
    SAFE_DELETE(m_pCoordLabel);
    SAFE_DELETE(m_settingsDialog);
    delete ui;
}

void MainWindow::Init()
{
    InitSlots();
    InitStatusBar();
}

void MainWindow::InitSlots()
{
    // File Menu
    connect(ui->actionOpen, &QAction::triggered, m_pulseqLoader, &PulseqLoader::OpenPulseqFile);
    connect(ui->actionReopen, &QAction::triggered, m_pulseqLoader, &PulseqLoader::ReOpenPulseqFile);
    connect(ui->actionCloseFile, &QAction::triggered, m_pulseqLoader, &PulseqLoader::ClosePulseqFile);

    // View Menu
    connect(ui->actionResetView, &QAction::triggered, m_waveformDrawer, &WaveformDrawer::ResetView);
    // Rename and repurpose to a single entry: "Undersample curves" (checked = downsampling ON)
    ui->actionShowFullDetail->setText("Undersample curves");
    ui->actionShowFullDetail->setToolTip("Downsample curves for performance");
    ui->actionShowFullDetail->setChecked(true); // default: undersampling enabled
    connect(ui->actionShowFullDetail, &QAction::toggled, this, &MainWindow::onShowFullDetailToggled);

    // View → Log
    if (ui->menuView)
    {
        QAction* logAction = new QAction(tr("Log"), this);
        ui->menuView->addSeparator();
        ui->menuView->addAction(logAction);
        connect(logAction, &QAction::triggered, this, &MainWindow::openLogWindow);
    }
    // Tools
    connect(ui->actionMeasureDt, &QAction::triggered, m_interactionHandler, &InteractionHandler::toggleMeasureDtMode);

    // Help Menu
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAbout);
    connect(ui->actionUsage, &QAction::triggered, this, &MainWindow::showUsage);
    // Hide "Contact" entry in Help (do not remove/delete to avoid dangling pointers)
    for (QAction* top : menuBar()->actions()) {
        QString txt = top->text(); QString norm = txt; norm.remove('&');
        if (norm.compare("Help", Qt::CaseInsensitive) != 0) continue;
        if (QMenu* helpMenu = top->menu()) {
            for (QAction* a : helpMenu->actions()) {
                QString t = a->text(); QString n = t; n.remove('&');
                if (n.contains("Contact", Qt::CaseInsensitive)) {
                    a->setVisible(false);
                    break;
                }
            }
        }
        break;
    }

    // QCustomPlot signals
    connect(ui->customPlot, &QCustomPlot::mouseMove, m_interactionHandler, &InteractionHandler::onMouseMove);
    connect(ui->customPlot, &QCustomPlot::mouseWheel, m_interactionHandler, &InteractionHandler::onMouseWheel);
    connect(ui->customPlot, &QCustomPlot::mousePress, m_interactionHandler, &InteractionHandler::onMousePress);
    connect(ui->customPlot, &QCustomPlot::mouseRelease, m_interactionHandler, &InteractionHandler::onMouseRelease);
    connect(ui->customPlot, &QCustomPlot::customContextMenuRequested, m_interactionHandler, &InteractionHandler::showContextMenu);
    ui->customPlot->setContextMenuPolicy(Qt::CustomContextMenu);
}

void MainWindow::InitStatusBar()
{
    m_pVersionLabel = new QLabel(this);
    ui->statusbar->addWidget(m_pVersionLabel);

    m_pProgressBar = new QProgressBar(this);
    m_pProgressBar->setMaximumWidth(200);
    m_pProgressBar->setMinimumWidth(0);
    m_pProgressBar->hide();
    m_pProgressBar->setRange(0, 100);
    m_pProgressBar->setValue(0);
    ui->statusbar->addWidget(m_pProgressBar);
}

// Event handlers are now delegated to the InteractionHandler
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    m_interactionHandler->dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    m_interactionHandler->dropEvent(event);
}

void MainWindow::wheelEvent(QWheelEvent* event)
{
    // The plotter has its own wheel event handling, so check if the mouse is over it.
    // If not, pass to the base class to handle normal window scrolling.
    if (ui->customPlot && !ui->customPlot->rect().contains(event->position().toPoint())) {
         QMainWindow::wheelEvent(event);
         return;
    }
    m_interactionHandler->wheelEvent(event);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // Delegate to InteractionHandler
    if (m_interactionHandler && m_interactionHandler->eventFilter(obj, event))
    {
        return true; // Event was handled
    }
    // If not handled, pass to base class
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    scheduleTrajectoryAspectUpdate();
    refreshTrajectoryCursor();
}

void MainWindow::setupSettingsMenu()
{
    // Hide top-level "Analysis" menu if present (do not remove/delete to avoid dangling pointers)
    for (QAction* act : menuBar()->actions()) {
        QString txt = act->text(); QString norm = txt; norm.remove('&');
        if (norm.compare("Analysis", Qt::CaseInsensitive) == 0) {
            act->setVisible(false);
            break;
        }
    }

    // Add a top-level Settings action that opens the dialog directly, placed before Help
    QAction* settingsAction = new QAction("&Settings", this);
    settingsAction->setShortcut(QKeySequence("Ctrl+,"));
    settingsAction->setStatusTip("Open application settings");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    // Try to insert before the Help menu so Help stays rightmost
    QAction* helpTop = nullptr;
    for (QAction* act : menuBar()->actions())
    {
        QString txt = act->text();
        QString norm = txt; norm.remove('&');
        if (norm.compare("Help", Qt::CaseInsensitive) == 0)
        {
            helpTop = act;
            break;
        }
    }
    if (helpTop)
        menuBar()->insertAction(helpTop, settingsAction);
    else
        menuBar()->addAction(settingsAction);
}

void MainWindow::onTimeUnitChanged()
{
    if (!m_pulseqLoader) return;

    // Remember the current viewport range and tFactor before the reload.
    double oldFactor = m_pulseqLoader->getTFactor();
    QCPRange savedRange;
    bool hasRange = false;
    if (m_waveformDrawer && !m_waveformDrawer->getRects().isEmpty()
        && m_waveformDrawer->getRects()[0])
    {
        savedRange = m_waveformDrawer->getRects()[0]->axis(QCPAxis::atBottom)->range();
        hasRange = true;
    }

    m_pulseqLoader->ReOpenPulseqFile();

    // Restore the viewport so the user keeps seeing the same physical time span
    // (e.g. 0-200 ms → 0-200000 us).
    if (hasRange && oldFactor != 0.0)
    {
        double newFactor = m_pulseqLoader->getTFactor();
        double ratio = newFactor / oldFactor;
        QCPRange newRange(savedRange.lower * ratio, savedRange.upper * ratio);
        if (auto* ih = getInteractionHandler())
            ih->synchronizeXAxes(newRange);
    }
}

void MainWindow::openSettings()
{
    if (m_settingsDialog) {
        m_settingsDialog->show();
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
    }
}

void MainWindow::setupPlotArea(QVBoxLayout* mainLayout)
{
    m_plotSplitter = new QSplitter(Qt::Horizontal, this);
    m_plotSplitter->setChildrenCollapsible(false);
    m_plotSplitter->addWidget(ui->customPlot);

    m_pTrajectoryPanel = new QWidget(this);
    m_pTrajectoryPanel->setVisible(false);
    QVBoxLayout* trajectoryLayout = new QVBoxLayout(m_pTrajectoryPanel);
    trajectoryLayout->setContentsMargins(0, 0, 0, 0);
    trajectoryLayout->setSpacing(6);

    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(6);
    m_pShowTrajectoryCursorCheckBox = new QCheckBox(tr("Show current position"), m_pTrajectoryPanel);
    m_pShowTrajectoryCursorCheckBox->setChecked(true);
    controlLayout->addWidget(m_pShowTrajectoryCursorCheckBox);
    m_pTrajectoryCrosshairCheckBox = new QCheckBox(tr("Crosshair"), m_pTrajectoryPanel);
    m_pTrajectoryCrosshairCheckBox->setChecked(false);
    controlLayout->addWidget(m_pTrajectoryCrosshairCheckBox);
    m_pTrajectoryRangeCombo = new QComboBox(m_pTrajectoryPanel);
    m_pTrajectoryRangeCombo->addItem(tr("Current window"));
    m_pTrajectoryRangeCombo->addItem(tr("Whole sequence"));
    m_pTrajectoryRangeCombo->addItem(tr("Current window + color"));
    m_pTrajectoryRangeCombo->setCurrentIndex(0);
    controlLayout->addWidget(m_pTrajectoryRangeCombo);
    m_pShowKtrajCheckBox = new QCheckBox(tr("ktraj"), m_pTrajectoryPanel);
    m_pShowKtrajCheckBox->setChecked(false);
    controlLayout->addWidget(m_pShowKtrajCheckBox);
    m_pShowKtrajAdcCheckBox = new QCheckBox(tr("ktraj_adc"), m_pTrajectoryPanel);
    m_pShowKtrajAdcCheckBox->setChecked(true);
    controlLayout->addWidget(m_pShowKtrajAdcCheckBox);
    m_pTrajectoryCrosshairLabel = new QLabel(m_pTrajectoryPanel);
    m_pTrajectoryCrosshairLabel->setMinimumWidth(180);
    controlLayout->addWidget(m_pTrajectoryCrosshairLabel);
    controlLayout->addStretch();
    m_pResetTrajectoryButton = new QPushButton(tr("Reset view"), m_pTrajectoryPanel);
    m_pResetTrajectoryButton->setEnabled(true);
    controlLayout->addWidget(m_pResetTrajectoryButton);
    m_pExportTrajectoryButton = new QPushButton(tr("Export trajectory"), m_pTrajectoryPanel);
    m_pExportTrajectoryButton->setEnabled(false);
    controlLayout->addWidget(m_pExportTrajectoryButton);
    trajectoryLayout->addLayout(controlLayout);

    m_pTrajectoryPlot = new QCustomPlot(m_pTrajectoryPanel);
    m_pTrajectoryPlot->setVisible(false);
    m_pTrajectoryPlot->setMouseTracking(true);
    m_pTrajectoryPlot->setNoAntialiasingOnDrag(true);
    m_pTrajectoryPlot->setPlottingHints(QCP::phFastPolylines | QCP::phCacheLabels);
    m_pTrajectoryPlot->setNotAntialiasedElements(QCP::aePlottables);
    m_pTrajectoryPlot->setInteractions(QCP::iRangeDrag);
    if (m_pTrajectoryPlot->axisRect())
    {
        auto* rect = m_pTrajectoryPlot->axisRect();
        rect->setRangeDrag(Qt::Horizontal | Qt::Vertical);
        rect->setRangeZoom(Qt::Horizontal | Qt::Vertical);
        rect->setRangeZoomFactor(0.95, 0.95); // retains pinch zoom if available
    }
    // Initialize trajectory axis labels and remember current trajectory unit
    m_lastTrajectoryUnit = Settings::getInstance().getTrajectoryUnit();
    updateTrajectoryAxisLabels();
    // Continuous trajectory curve (blue)
    m_pTrajectoryCurve = new QCPCurve(m_pTrajectoryPlot->xAxis, m_pTrajectoryPlot->yAxis);
    m_pTrajectoryCurve->setLineStyle(QCPCurve::lsLine);
    m_pTrajectoryCurve->setScatterStyle(QCPScatterStyle::ssNone);
    m_pTrajectoryCurve->setAntialiased(false);
    QPen trajPen(Qt::blue);
    trajPen.setWidthF(1.5);
    m_pTrajectoryCurve->setPen(trajPen);
    // ADC sampling points — QCPCurve kept hidden as data container;
    // actual rendering uses QImage rasterizer (see renderTrajectoryScatter).
    m_pTrajectorySamplesGraph = new QCPCurve(m_pTrajectoryPlot->xAxis, m_pTrajectoryPlot->yAxis);
    m_pTrajectorySamplesGraph->setVisible(false);
    // Rasterized scatter pixmap — replaces QCPCurve scatter for performance
    m_pTrajectoryScatterItem = new QCPItemPixmap(m_pTrajectoryPlot);
    m_pTrajectoryScatterItem->setVisible(false);
    m_pTrajectoryScatterItem->setScaled(false);
    // Keep trajectory axis equal after any internal replot/layout
    connect(m_pTrajectoryPlot, &QCustomPlot::afterReplot, this, [this]() {
        if (!m_pTrajectoryPlot || !m_pTrajectoryPlot->isVisible())
            return;
        if (!m_pendingTrajectoryAspectUpdate)
            enforceTrajectoryAspect(false);
        // Keep crosshair overlay aligned with axis rect
        if (m_pTrajectoryCrosshairOverlay && m_pTrajectoryPlot->axisRect())
        {
            m_pTrajectoryCrosshairOverlay->setGeometry(m_pTrajectoryPlot->axisRect()->rect());
        }
    });
    connect(m_pTrajectoryPlot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange&){
                if (!m_inTrajectoryRangeAdjust)
                    scheduleTrajectoryAspectUpdate();
                refreshTrajectoryCursor();
                renderTrajectoryScatter();
            });
    connect(m_pTrajectoryPlot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange&){
                if (!m_inTrajectoryRangeAdjust)
                    scheduleTrajectoryAspectUpdate();
                refreshTrajectoryCursor();
                renderTrajectoryScatter();
            });
    connect(m_pTrajectoryPlot, &QCustomPlot::mouseWheel,
            this, &MainWindow::onTrajectoryWheel);
    connect(m_pShowTrajectoryCursorCheckBox, &QCheckBox::toggled,
            this, &MainWindow::onShowTrajectoryCursorToggled);
    connect(m_pTrajectoryCrosshairCheckBox, &QCheckBox::toggled,
            this, &MainWindow::onTrajectoryCrosshairToggled);
    connect(m_pTrajectoryRangeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTrajectoryRangeModeChanged);
    connect(m_pShowKtrajCheckBox, &QCheckBox::toggled,
            this, &MainWindow::onTrajectorySeriesToggled);
    connect(m_pShowKtrajAdcCheckBox, &QCheckBox::toggled,
            this, &MainWindow::onTrajectorySeriesToggled);
    connect(m_pTrajectoryPlot, &QCustomPlot::mouseMove,
            this, &MainWindow::onTrajectoryMouseMove);
    m_pTrajectoryCursorMarker = new QWidget(m_pTrajectoryPlot);
    m_pTrajectoryCursorMarker->setFixedSize(14, 14);
    m_pTrajectoryCursorMarker->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_pTrajectoryCursorMarker->setAttribute(Qt::WA_StyledBackground, true);
    m_pTrajectoryCursorMarker->setAutoFillBackground(true);
    m_pTrajectoryCursorMarker->setStyleSheet("background-color: rgba(255,215,0,0.9);"
                                             "border: 2px solid rgba(90,70,0,0.9);"
                                             "border-radius: 7px;");
    m_pTrajectoryCursorMarker->setVisible(false);
    // Overlay for drawing crosshair without triggering full plot replot
    m_pTrajectoryCrosshairOverlay = new TrajectoryCrosshairOverlay(m_pTrajectoryPlot);
    if (m_pTrajectoryPlot->axisRect())
        m_pTrajectoryCrosshairOverlay->setGeometry(m_pTrajectoryPlot->axisRect()->rect());
    trajectoryLayout->addWidget(m_pTrajectoryPlot, 1);
    connect(m_pExportTrajectoryButton, &QPushButton::clicked, this, &MainWindow::exportTrajectory);
    connect(m_pResetTrajectoryButton, &QPushButton::clicked, this, &MainWindow::onResetTrajectoryRange);

    refreshTrajectoryPlotData();
    m_plotSplitter->addWidget(m_pTrajectoryPanel);

    mainLayout->addWidget(m_plotSplitter, 1);
    connect(m_plotSplitter, &QSplitter::splitterMoved, this, &MainWindow::onPlotSplitterMoved);

    QList<int> sizes;
    sizes << 1 << 0;
    m_plotSplitter->setSizes(sizes);
}

void MainWindow::setTrajectoryVisible(bool show)
{
    if (!m_pTrajectoryPlot || !m_plotSplitter || !m_pTrajectoryPanel)
        return;
    if (m_showTrajectory == show)
        return;
    m_showTrajectory = show;
    m_pTrajectoryPanel->setVisible(show);
    m_pTrajectoryPlot->setVisible(show);
    // Reset initial-range flag when opening the trajectory panel
    if (show) m_trajectoryRangeInitialized = false;
    PulseqLoader* loader = getPulseqLoader();
    if (show)
    {
        if (loader)
        {
            loader->ensureTrajectoryPrepared();
            if (loader->needsRfUseGuessWarning())
            {
                Settings& s = Settings::getInstance();
                if (s.getShowTrajectoryApproximateDialog())
                {
                    QMessageBox msg(this);
                    msg.setIcon(QMessageBox::Warning);
                    msg.setWindowTitle(tr("Trajectory Warning"));
                    msg.setText(loader->getRfUseGuessWarning());
                    QCheckBox* cb = new QCheckBox(tr("Do not show this warning again"), &msg);
                    msg.setCheckBox(cb);
                    msg.addButton(QMessageBox::Ok);
                    msg.exec();
                    if (cb->isChecked())
                    {
                        s.setShowTrajectoryApproximateDialog(false);
                    }
                }
                loader->markRfUseGuessWarningShown();
            }
        }
        refreshTrajectoryPlotData();
    }
    QList<int> sizes;
    if (show)
    {
        int total = qMax(1, m_plotSplitter->width());
        int left = static_cast<int>(total * 0.65);
        sizes << left << (total - left);
    }
    else
    {
        sizes << m_plotSplitter->width() << 0;
    }
    m_plotSplitter->setSizes(sizes);
    scheduleTrajectoryAspectUpdate();
    refreshTrajectoryCursor();
}

// Rasterized scatter renderer: paints ADC dots directly into a QImage via scanLine
// pixel writes and displays via QCPItemPixmap. This is ~50x faster than QCPCurve
// scatter (which calls QPainter::drawEllipse per point). Every data point is rendered
// — no downsampling, no visual loss. Re-called on every axis range change (drag/zoom).
void MainWindow::renderTrajectoryScatter()
{
    if (!m_pTrajectoryPlot || !m_pTrajectoryScatterItem || !m_showKtrajAdc)
    {
        if (m_pTrajectoryScatterItem) m_pTrajectoryScatterItem->setVisible(false);
        return;
    }
    QCPAxisRect* axRect = m_pTrajectoryPlot->axisRect();
    if (!axRect) return;
    int w = axRect->width();
    int h = axRect->height();
    if (w <= 0 || h <= 0) return;

    int N = m_trajScatterKx.size();
    if (N <= 0 || m_trajScatterKy.size() != N)
    {
        m_pTrajectoryScatterItem->setVisible(false);
        return;
    }

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QCPRange xRange = m_pTrajectoryPlot->xAxis->range();
    QCPRange yRange = m_pTrajectoryPlot->yAxis->range();
    double xSize = xRange.size();
    double ySize = yRange.size();
    if (xSize <= 0 || ySize <= 0) return;

    const double* kxD = m_trajScatterKx.constData();
    const double* kyD = m_trajScatterKy.constData();
    const bool hasColors = (m_trajScatterColors.size() == N);
    const QRgb defaultColor = qRgba(255, 0, 0, 255);

    // Paint 3x3 pixel dots using direct scanLine access (no QPainter overhead)
    for (int i = 0; i < N; ++i)
    {
        int px = static_cast<int>((kxD[i] - xRange.lower) / xSize * w);
        int py = h - 1 - static_cast<int>((kyD[i] - yRange.lower) / ySize * h);
        if (px < -1 || px > w || py < -1 || py > h) continue; // quick reject
        QRgb c = hasColors ? m_trajScatterColors[i] : defaultColor;
        for (int dy = -1; dy <= 1; ++dy)
        {
            int y = py + dy;
            if (y < 0 || y >= h) continue;
            QRgb* scanline = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int dx = -1; dx <= 1; ++dx)
            {
                int x = px + dx;
                if (x >= 0 && x < w) scanline[x] = c;
            }
        }
    }

    m_pTrajectoryScatterItem->setPixmap(QPixmap::fromImage(img));
    m_pTrajectoryScatterItem->topLeft->setType(QCPItemPosition::ptAbsolute);
    m_pTrajectoryScatterItem->topLeft->setCoords(axRect->left(), axRect->top());
    m_pTrajectoryScatterItem->setVisible(true);
}

void MainWindow::refreshTrajectoryPlotData()
{
    if (!m_pTrajectoryCurve)
    {
        updateTrajectoryExportState();
        refreshTrajectoryCursor();
        return;
    }

    PulseqLoader* loader = getPulseqLoader();
    if (!loader)
    {
        m_pTrajectoryCurve->data()->clear();
        if (m_pTrajectorySamplesGraph)
            m_pTrajectorySamplesGraph->data()->clear();
        if (!m_trajectoryRangeInitialized)
        {
            m_trajectoryBaseXRange = QCPRange(-1.0, 1.0);
            m_trajectoryBaseYRange = QCPRange(-1.0, 1.0);
            if (m_pTrajectoryPlot)
            {
                m_pTrajectoryPlot->xAxis->setRange(m_trajectoryBaseXRange);
                m_pTrajectoryPlot->yAxis->setRange(m_trajectoryBaseYRange);
            }
            m_trajectoryRangeInitialized = true;
        }
        updateTrajectoryExportState();
        refreshTrajectoryCursor();
        return;
    }

    loader->ensureTrajectoryPrepared();
    const QVector<double>& kx = loader->getTrajectoryKx();
    const QVector<double>& ky = loader->getTrajectoryKy();
    const QVector<double>& t = loader->getTrajectoryTimeSec();

    const QVector<double>& kxAdc = loader->getTrajectoryKxAdc();
    const QVector<double>& kyAdc = loader->getTrajectoryKyAdc();
    const QVector<double>& tAdc = loader->getTrajectoryTimeAdcSec();

    const int sampleCount = std::min(kx.size(), ky.size());
    if (sampleCount <= 0)
    {
        m_pTrajectoryCurve->data()->clear();
        if (m_pTrajectorySamplesGraph)
            m_pTrajectorySamplesGraph->data()->clear();
        updateTrajectoryExportState();
        refreshTrajectoryCursor();
        return;
    }

    // Determine trajectory display scale based on settings and sequence FOV
    Settings& settings = Settings::getInstance();
    Settings::TrajectoryUnit trajUnit = settings.getTrajectoryUnit();
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
            settings.setTrajectoryUnit(Settings::TrajectoryUnit::PerM);
            trajUnit = settings.getTrajectoryUnit();
            trajScale = 1.0;
            // Update axis labels immediately to reflect fallback
            updateTrajectoryAxisLabels();
        }
    }

    bool limitToView = !m_showWholeTrajectory;
    double filterStartSec = 0.0;
    double filterEndSec = 0.0;
    if (limitToView)
    {
        double tFactor = loader->getTFactor();
        if (!ui || !ui->customPlot || tFactor <= 0.0)
        {
            limitToView = false;
        }
        else
        {
            QCPRange viewRange = ui->customPlot->xAxis->range();
            double denom = tFactor * 1e6;
            if (denom <= 0.0)
            {
                limitToView = false;
            }
            else
            {
                filterStartSec = viewRange.lower / denom;
                filterEndSec = viewRange.upper / denom;
                if (filterEndSec < filterStartSec)
                    std::swap(filterStartSec, filterEndSec);
            }
        }
    }

    auto filterCurve = [&](const QVector<double>& timeSec,
                           const QVector<double>& srcX,
                           const QVector<double>& srcY,
                           bool applyFilter,
                           QVector<double>& outParam,
                           QVector<double>& outX,
                           QVector<double>& outY)
    {
        outParam.clear();
        outX.clear();
        outY.clear();
        qsizetype limit = std::min(srcX.size(), srcY.size());
        if (limit <= 0)
            return;
        bool useTime = !timeSec.isEmpty();
        if (useTime)
            limit = std::min(limit, static_cast<qsizetype>(timeSec.size()));
        outParam.reserve(static_cast<int>(limit));
        outX.reserve(static_cast<int>(limit));
        outY.reserve(static_cast<int>(limit));
        for (qsizetype i = 0; i < limit; ++i)
        {
            double paramVal = useTime ? timeSec[i] : static_cast<double>(i);
            if (applyFilter && (paramVal < filterStartSec || paramVal > filterEndSec))
                continue;
            outParam.append(paramVal);
            outX.append(srcX[i]);
            outY.append(srcY[i]);
        }
        if (!useTime)
        {
            for (qsizetype i = 0; i < outParam.size(); ++i)
                outParam[i] = static_cast<double>(i);
        }
    };

    auto filterScatter = [&](const QVector<double>& timeSec,
                             const QVector<double>& srcX,
                             const QVector<double>& srcY,
                             bool applyFilter,
                             QVector<double>& outX,
                             QVector<double>& outY)
    {
        outX.clear();
        outY.clear();
        qsizetype limit = std::min(srcX.size(), srcY.size());
        if (limit <= 0)
            return;
        bool useTime = !timeSec.isEmpty();
        if (useTime)
            limit = std::min(limit, static_cast<qsizetype>(timeSec.size()));
        outX.reserve(static_cast<int>(limit));
        outY.reserve(static_cast<int>(limit));
        for (qsizetype i = 0; i < limit; ++i)
        {
            double paramVal = useTime ? timeSec[i] : static_cast<double>(i);
            if (applyFilter && (paramVal < filterStartSec || paramVal > filterEndSec))
                continue;
            outX.append(srcX[i]);
            outY.append(srcY[i]);
        }
    };

    const bool canFilterCurve = limitToView && !t.isEmpty();
    QVector<double> curveParam;
    QVector<double> kxSubset;   // always in base units 1/m
    QVector<double> kySubset;   // always in base units 1/m
    filterCurve(t, kx, ky, canFilterCurve, curveParam, kxSubset, kySubset);

    if (curveParam.isEmpty() && kxSubset.isEmpty() && !canFilterCurve)
    {
        // fallback to complete dataset when filtering disabled but lambda removed everything (shouldn't happen)
        curveParam.resize(sampleCount);
        kxSubset = kx.mid(0, sampleCount);
        kySubset = ky.mid(0, sampleCount);
        for (int i = 0; i < sampleCount; ++i)
            curveParam[i] = !t.isEmpty() ? t[i] : static_cast<double>(i);
    }
    // Apply trajectory scaling only for plotting; kxSubset/kySubset remain in base units.
    if (trajScale != 1.0)
    {
        double scaleAbs = std::abs(trajScale);
        QVector<double> kxScaled = kxSubset;
        QVector<double> kyScaled = kySubset;
        for (auto& v : kxScaled) v *= scaleAbs;
        for (auto& v : kyScaled) v *= scaleAbs;
        m_pTrajectoryCurve->setData(curveParam, kxScaled, kyScaled, true);
    }
    else
    {
        m_pTrajectoryCurve->setData(curveParam, kxSubset, kySubset, true);
    }
    m_pTrajectoryCurve->setVisible(m_showKtraj);

    QVector<double> kxAdcSubset;  // always in base units 1/m
    QVector<double> kyAdcSubset;  // always in base units 1/m
    filterScatter(tAdc, kxAdc, kyAdc, limitToView && !tAdc.isEmpty(), kxAdcSubset, kyAdcSubset);

    // Store scatter data for the QImage rasterizer (renderTrajectoryScatter).
    // No downsampling needed — scanLine pixel writes are fast enough for any point count.
    auto scaleVec = [&](QVector<double>& v) {
        if (trajScale != 1.0) {
            double s = std::abs(trajScale);
            for (auto& x : v) x *= s;
        }
    };

    m_trajScatterColors.clear();
    if (m_colorCurrentWindow && limitToView && !tAdc.isEmpty())
    {
        // Colored mode: compute per-point colors from time-based colormap
        double tAdcMin = std::numeric_limits<double>::infinity();
        double tAdcMax = -std::numeric_limits<double>::infinity();
        int limit = std::min({ tAdc.size(), kxAdc.size(), kyAdc.size() });
        for (int i = 0; i < limit; ++i) {
            double tt = tAdc[i];
            if (tt < filterStartSec || tt > filterEndSec) continue;
            if (tt < tAdcMin) tAdcMin = tt;
            if (tt > tAdcMax) tAdcMax = tt;
        }
        double denom = (tAdcMax > tAdcMin) ? (tAdcMax - tAdcMin) : 1.0;
        bool validRange = std::isfinite(tAdcMin) && std::isfinite(tAdcMax) && (tAdcMax > tAdcMin);

        m_trajScatterKx.clear();
        m_trajScatterKy.clear();
        if (validRange) {
            m_trajScatterKx.reserve(limit);
            m_trajScatterKy.reserve(limit);
            m_trajScatterColors.reserve(limit);
            Settings::TrajectoryColormap cmap = Settings::getInstance().getTrajectoryColormap();
            for (int i = 0; i < limit; ++i) {
                double tt = tAdc[i];
                if (tt < filterStartSec || tt > filterEndSec) continue;
                m_trajScatterKx.append(kxAdc[i]);
                m_trajScatterKy.append(kyAdc[i]);
                double norm = (tt - tAdcMin) / denom;
                QColor c = sampleTrajectoryColormap(cmap, norm);
                m_trajScatterColors.append(c.rgba());
            }
            scaleVec(m_trajScatterKx);
            scaleVec(m_trajScatterKy);
        } else {
            // Fallback to uniform red
            m_trajScatterKx = kxAdcSubset;
            m_trajScatterKy = kyAdcSubset;
            scaleVec(m_trajScatterKx);
            scaleVec(m_trajScatterKy);
        }
    }
    else
    {
        // Non-colored mode: uniform red (m_trajScatterColors stays empty)
        m_trajScatterKx = kxAdcSubset;
        m_trajScatterKy = kyAdcSubset;
        scaleVec(m_trajScatterKx);
        scaleVec(m_trajScatterKy);
    }

    // Hide legacy QCPCurve scatter objects (kept for API compat but not rendered)
    if (m_pTrajectorySamplesGraph) m_pTrajectorySamplesGraph->setVisible(false);
    for (QCPCurve* g : m_trajColorGraphs)
        if (g) g->setVisible(false);

    // Render via QImage rasterizer
    renderTrajectoryScatter();
    if (m_pTrajectoryPlot) m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);

    // Helper: set range only once (initialization), never override user interaction
    auto setRangeIfUninitialized = [&](const QCPRange& rx, const QCPRange& ry){
        if (m_trajectoryRangeInitialized)
            return false;
        m_trajectoryBaseXRange = rx;
        m_trajectoryBaseYRange = ry;
        if (m_pTrajectoryPlot)
        {
            m_pTrajectoryPlot->xAxis->setRange(m_trajectoryBaseXRange);
            m_pTrajectoryPlot->yAxis->setRange(m_trajectoryBaseYRange);
        }
        m_trajectoryRangeInitialized = true;
        return true;
    };

    // If ADC samples exist, set default symmetric range based on global ktraj_adc (only once),
    // using base 1/m values and then scaling the final range by trajScale:
    // a_base = max(abs(kx_adc), abs(ky_adc)) across all samples; ranges = [-a_base, a_base] in 1/m,
    // then multiplied by |trajScale| for display units.
    if (!kxAdc.isEmpty() && !kyAdc.isEmpty())
    {
        double aBase = 0.0; // in 1/m
        int n = std::min(kxAdc.size(), kyAdc.size());
        for (int i = 0; i < n; ++i)
        {
            double ax = std::abs(kxAdc[i]);
            double ay = std::abs(kyAdc[i]);
            if (std::isfinite(ax)) aBase = std::max(aBase, ax);
            if (std::isfinite(ay)) aBase = std::max(aBase, ay);
        }
        if (!(aBase > 0.0)) aBase = 1.0; // fallback in base units
        double scaleAbs = std::abs(trajScale);
        double aDisplay = aBase * (scaleAbs > 0.0 ? scaleAbs : 1.0);
        bool changed = setRangeIfUninitialized(QCPRange(-aDisplay, aDisplay),
                                               QCPRange(-aDisplay, aDisplay));
        if (changed)
        {
            updateTrajectoryExportState();
            scheduleTrajectoryAspectUpdate();
            refreshTrajectoryCursor();
        }
        return;
    }

    // Use base 1/m data for bounds; only the final ranges are scaled by trajScale.
    const QVector<double>& boundsX = !kxSubset.isEmpty() ? kxSubset : kxAdcSubset;
    const QVector<double>& boundsY = !kySubset.isEmpty() ? kySubset : kyAdcSubset;
    const int boundCount = std::min(boundsX.size(), boundsY.size());
    if (boundCount < 2)
    {
        if (m_pTrajectoryPlot)
        {
            m_trajectoryBaseXRange = QCPRange(-1.0, 1.0);
            m_trajectoryBaseYRange = QCPRange(-1.0, 1.0);
            m_pTrajectoryPlot->xAxis->setRange(m_trajectoryBaseXRange);
            m_pTrajectoryPlot->yAxis->setRange(m_trajectoryBaseYRange);
        }
        updateTrajectoryExportState();
        refreshTrajectoryCursor();
        return;
    }

    double minX = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < boundCount; ++i)
    {
        double x = boundsX[i];
        double y = boundsY[i];
        if (std::isfinite(x))
        {
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
        if (std::isfinite(y))
        {
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }

    if (!std::isfinite(minX) || !std::isfinite(maxX) || std::abs(maxX - minX) < 1e-6)
    {
        minX = -1.0;
        maxX = 1.0;
    }
    if (!std::isfinite(minY) || !std::isfinite(maxY) || std::abs(maxY - minY) < 1e-6)
    {
        minY = -1.0;
        maxY = 1.0;
    }

    double padX = (maxX - minX) * 0.05;
    double padY = (maxY - minY) * 0.05;
    if (padX == 0.0) padX = 0.1;
    if (padY == 0.0) padY = 0.1;

    // Scale final ranges by |trajScale| so the viewport matches the display units.
    double scaleAbs = std::abs(trajScale);
    if (scaleAbs <= 0.0) scaleAbs = 1.0;
    QCPRange rxDisplay((minX - padX) * scaleAbs, (maxX + padX) * scaleAbs);
    QCPRange ryDisplay((minY - padY) * scaleAbs, (maxY + padY) * scaleAbs);

    bool changed = setRangeIfUninitialized(rxDisplay, ryDisplay);
    if (changed)
    {
        updateTrajectoryExportState();
        scheduleTrajectoryAspectUpdate();
    }
    refreshTrajectoryCursor();
}

void MainWindow::enforceTrajectoryAspect(bool queueReplot)
{
    m_pendingTrajectoryAspectUpdate = false;
    if (!m_pTrajectoryPlot || !m_pTrajectoryPlot->isVisible())
        return;
    QCPAxisRect* rect = m_pTrajectoryPlot->axisRect();
    if (!rect)
        return;
    double width = rect->width();
    double height = rect->height();
    if (width <= 0 || height <= 0)
        return;
    QCPRange currentX = m_pTrajectoryPlot->xAxis->range();
    QCPRange currentY = m_pTrajectoryPlot->yAxis->range();
    double spanX = currentX.size();
    double spanY = currentY.size();
    if (spanX <= 0 || spanY <= 0)
        return;

    double pixelsPerUnitX = width / spanX;
    double pixelsPerUnitY = height / spanY;
    if (pixelsPerUnitX <= 0 || pixelsPerUnitY <= 0)
        return;
    double centerX = currentX.center();
    double centerY = currentY.center();
    double newSpanX = spanX;
    double newSpanY = spanY;

    const double ratio = pixelsPerUnitX / pixelsPerUnitY;
    constexpr double kAspectTolerance = 0.03; // allow ±3% mismatch before correcting
    if (std::abs(ratio - 1.0) <= kAspectTolerance)
    {
        if (queueReplot)
            m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);
        return;
    }

    if (pixelsPerUnitX > pixelsPerUnitY)
    {
        // X axis has more pixels per unit -> expand X span
        double targetPixelsPerUnit = pixelsPerUnitY;
        newSpanX = width / targetPixelsPerUnit;
    }
    else
    {
        double targetPixelsPerUnit = pixelsPerUnitX;
        newSpanY = height / targetPixelsPerUnit;
    }

    m_inTrajectoryRangeAdjust = true;
    m_pTrajectoryPlot->xAxis->setRange(centerX - newSpanX / 2.0, centerX + newSpanX / 2.0);
    m_pTrajectoryPlot->yAxis->setRange(centerY - newSpanY / 2.0, centerY + newSpanY / 2.0);
    m_inTrajectoryRangeAdjust = false;
    if (queueReplot)
        m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::onPlotSplitterMoved(int, int)
{
    scheduleTrajectoryAspectUpdate();
    refreshTrajectoryCursor();
}

void MainWindow::scheduleTrajectoryAspectUpdate()
{
    if (m_pendingTrajectoryAspectUpdate)
        return;
    if (!m_pTrajectoryPlot || !m_pTrajectoryPlot->isVisible())
        return;
    m_pendingTrajectoryAspectUpdate = true;
    // Throttle to ~60 FPS to avoid excessive replots during drag/zoom
    QTimer::singleShot(16, this, [this]() {
        enforceTrajectoryAspect(true);
    });
}

void MainWindow::onResetTrajectoryRange()
{
    if (!m_pTrajectoryPlot)
        return;
    PulseqLoader* loader = getPulseqLoader();
    // Initialize base ranges if never computed
    if (!m_trajectoryRangeInitialized)
    {
        refreshTrajectoryPlotData();
    }
    // Apply stored base ranges
    m_inTrajectoryRangeAdjust = true;
    m_pTrajectoryPlot->xAxis->setRange(m_trajectoryBaseXRange);
    m_pTrajectoryPlot->yAxis->setRange(m_trajectoryBaseYRange);
    m_inTrajectoryRangeAdjust = false;
    scheduleTrajectoryAspectUpdate();
    m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);
}

void MainWindow::onTrajectoryCrosshairToggled(bool checked)
{
    m_showTrajectoryCrosshair = checked;
    if (!m_pTrajectoryPlot)
        return;

    auto* overlay = static_cast<TrajectoryCrosshairOverlay*>(m_pTrajectoryCrosshairOverlay);

    if (checked && m_showTrajectory && m_pTrajectoryPlot->isVisible())
    {
        m_pTrajectoryPlot->setCursor(Qt::CrossCursor);
        if (overlay)
            overlay->setEnabledFlag(true), overlay->clearCrosshair();
    }
    else
    {
        m_pTrajectoryPlot->unsetCursor();
        if (overlay)
            overlay->setEnabledFlag(false);
    }

    if (!checked && m_pTrajectoryCrosshairLabel)
    {
        m_pTrajectoryCrosshairLabel->clear();
    }
}

void MainWindow::onTrajectoryMouseMove(QMouseEvent* event)
{
    if (!event || !m_pTrajectoryPlot || !m_pTrajectoryPlot->isVisible())
        return;

    if (!m_showTrajectoryCrosshair || !m_pTrajectoryCrosshairCheckBox || !m_pTrajectoryCrosshairCheckBox->isChecked())
        return;

    const QPoint pos = event->pos();
    QCPAxisRect* rect = m_pTrajectoryPlot->axisRect();
    auto* overlay = static_cast<TrajectoryCrosshairOverlay*>(m_pTrajectoryCrosshairOverlay);
    if (!overlay)
        return;

    if (!rect || m_pTrajectoryPlot->axisRectAt(pos) != rect)
    {
        m_pTrajectoryPlot->unsetCursor();
        overlay->clearCrosshair();
        return;
    }

    m_pTrajectoryPlot->setCursor(Qt::CrossCursor);

    double kxDisplay = m_pTrajectoryPlot->xAxis->pixelToCoord(pos.x());
    double kyDisplay = m_pTrajectoryPlot->yAxis->pixelToCoord(pos.y());

    // Update overlay-local crosshair position (within axis rect)
    QPoint topLeft = rect->rect().topLeft();
    QPoint localPos = pos - topLeft;
    overlay->setEnabledFlag(true);
    overlay->setGeometry(rect->rect());
    overlay->setCrosshairPos(localPos);

    QString unit = Settings::getInstance().getTrajectoryUnitString();
    if (m_pTrajectoryCrosshairLabel)
    {
        m_pTrajectoryCrosshairLabel->setText(
            QStringLiteral("kx = %1, ky = %2 %3")
                .arg(kxDisplay, 0, 'g', 5)
                .arg(kyDisplay, 0, 'g', 5)
                .arg(unit));
    }
}

void MainWindow::onTrajectoryWheel(QWheelEvent* event)
{
    if (!m_pTrajectoryPlot || !m_pTrajectoryPlot->isVisible())
        return;
    // Honor Settings: ZoomInputMode (Wheel or CtrlWheel)
    Settings& appSettings = Settings::getInstance();
    Settings::ZoomInputMode zoomMode = appSettings.getZoomInputMode();
    bool requireCtrl = (zoomMode == Settings::ZoomInputMode::CtrlWheel);
    if (requireCtrl && !(event->modifiers() & Qt::ControlModifier))
        return;
    QCPAxisRect* rect = m_pTrajectoryPlot->axisRect();
    if (!rect)
        return;

    int delta = event->angleDelta().y();
    if (delta == 0)
        delta = event->pixelDelta().y();
    if (delta == 0)
        return;

    // Normalize to typical wheel "steps"
    double steps = static_cast<double>(delta) / 120.0;
    double base = 0.9; // close to MATLAB default zoom cadence
    double scale = std::pow(base, steps);
    if (!std::isfinite(scale) || scale <= 0.0)
        return;

    auto clampScale = [](double value) {
        const double minScale = 0.02;
        const double maxScale = 50.0;
        return std::clamp(value, minScale, maxScale);
    };
    scale = clampScale(scale);

    const QPointF pos = event->position();
    double targetX = m_pTrajectoryPlot->xAxis->pixelToCoord(pos.x());
    double targetY = m_pTrajectoryPlot->yAxis->pixelToCoord(pos.y());

    auto zoomAxis = [&](QCPAxis* axis, double anchor) {
        QCPRange range = axis->range();
        double lower = anchor + (range.lower - anchor) * scale;
        double upper = anchor + (range.upper - anchor) * scale;
        if (std::abs(upper - lower) < 1e-12)
            return;
        axis->setRange(lower, upper);
    };

    zoomAxis(m_pTrajectoryPlot->xAxis, targetX);
    zoomAxis(m_pTrajectoryPlot->yAxis, targetY);

    event->accept();
    scheduleTrajectoryAspectUpdate();
    m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);
}

bool MainWindow::sampleTrajectoryPosition(double timeSec,
                                          double& kxOut,
                                          double& kyOut,
                                          double& kzOut) const
{
    PulseqLoader* loader = m_pulseqLoader;
    if (!loader)
        return false;
    const QVector<double>& times = loader->getTrajectoryTimeSec();
    const QVector<double>& kx = loader->getTrajectoryKx();
    const QVector<double>& ky = loader->getTrajectoryKy();
    const QVector<double>& kz = loader->getTrajectoryKz();
    const int limit = std::min({ times.size(), kx.size(), ky.size(), kz.size() });
    if (limit <= 0)
        return false;

    auto isValid = [&](int idx) -> bool {
        return idx >= 0 && idx < limit &&
               std::isfinite(kx[idx]) &&
               std::isfinite(ky[idx]) &&
               std::isfinite(kz[idx]);
    };
    auto findForward = [&](int start) -> int {
        int idx = qMax(0, start);
        for (; idx < limit; ++idx)
        {
            if (isValid(idx))
                return idx;
        }
        return -1;
    };
    auto findBackward = [&](int start) -> int {
        int idx = qMin(limit - 1, start);
        for (; idx >= 0; --idx)
        {
            if (isValid(idx))
                return idx;
        }
        return -1;
    };
    auto useIndex = [&](int idx) -> bool {
        if (!isValid(idx))
            return false;
        kxOut = kx[idx];
        kyOut = ky[idx];
        kzOut = kz[idx];
        return true;
    };

    if (timeSec <= times.first())
        return useIndex(findForward(0));
    if (timeSec >= times[limit - 1])
        return useIndex(findBackward(limit - 1));

    auto begin = times.constBegin();
    auto it = std::lower_bound(begin, begin + limit, timeSec);
    int upper = static_cast<int>(it - begin);
    if (upper <= 0)
        upper = 1;
    if (upper >= limit)
        upper = limit - 1;
    int lower = upper - 1;

    int left = findBackward(lower);
    int right = findForward(upper);
    if (left < 0 && right < 0)
        return false;
    if (left < 0)
        left = right;
    if (right < 0)
        right = left;
    if (left == right)
        return useIndex(left);

    double tLeft = times[left];
    double tRight = times[right];
    if (!std::isfinite(tLeft) || !std::isfinite(tRight) || std::abs(tRight - tLeft) < 1e-12)
        return useIndex(left);

    double alpha = (timeSec - tLeft) / (tRight - tLeft);
    alpha = std::clamp(alpha, 0.0, 1.0);
    kxOut = kx[left] + (kx[right] - kx[left]) * alpha;
    kyOut = ky[left] + (ky[right] - ky[left]) * alpha;
    kzOut = kz[left] + (kz[right] - kz[left]) * alpha;
    return true;
}

void MainWindow::refreshTrajectoryCursor()
{
    if (!m_pTrajectoryPlot || !m_pTrajectoryCursorMarker)
        return;

    bool shouldDisplay = m_showTrajectory && m_showTrajectoryCursor && m_hasTrajectoryCursorTime;
    PulseqLoader* loader = m_pulseqLoader;
    double kx = 0.0;
    double ky = 0.0;
    double kz = 0.0;
    bool havePosition = false;

    if (shouldDisplay && loader && loader->hasTrajectoryData())
    {
        double tFactor = loader->getTFactor();
        if (tFactor > 0.0)
        {
            double timeSec = (m_currentTrajectoryTimeInternal / tFactor) * 1e-6;
            havePosition = sampleTrajectoryPosition(timeSec, kx, ky, kz);
        }
    }

    if (!havePosition)
    {
        m_pTrajectoryCursorMarker->setVisible(false);
        return;
    }

    // Apply the same trajectory unit scaling as the trajectory plot, so the
    // cursor marker stays consistent with the displayed units.
    double scale = 1.0;
    {
        Settings& settings = Settings::getInstance();
        Settings::TrajectoryUnit unit = settings.getTrajectoryUnit();
        if (unit == Settings::TrajectoryUnit::RadPerM)
        {
            scale = 2.0 * M_PI;
        }
        else if (unit == Settings::TrajectoryUnit::InvFov)
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
                scale = fovMeters;
            }
            else
            {
                // Fallback: keep scale=1.0; Settings logic will already have
                // reverted to 1/m during trajectory preparation if needed.
                scale = 1.0;
            }
        }
    }
    double scaleAbs = std::abs(scale);
    if (scaleAbs <= 0.0) scaleAbs = 1.0;

    const double kxDisplay = kx * scaleAbs;
    const double kyDisplay = ky * scaleAbs;

    const double px = m_pTrajectoryPlot->xAxis->coordToPixel(kxDisplay);
    const double py = m_pTrajectoryPlot->yAxis->coordToPixel(kyDisplay);
    const QRect plotRect = m_pTrajectoryPlot->rect();
    if (!plotRect.contains(QPoint(static_cast<int>(std::round(px)),
                                  static_cast<int>(std::round(py)))))
    {
        m_pTrajectoryCursorMarker->setVisible(false);
        return;
    }

    const int size = m_pTrajectoryCursorMarker->width();
    const int markerX = static_cast<int>(std::round(px)) - size / 2;
    const int markerY = static_cast<int>(std::round(py)) - size / 2;
    m_pTrajectoryCursorMarker->move(markerX, markerY);
    m_pTrajectoryCursorMarker->setVisible(true);
    m_pTrajectoryCursorMarker->raise();
}

bool MainWindow::sampleTrajectoryAtInternalTime(double internalTime,
                                                double& kxOut,
                                                double& kyOut,
                                                double& kzOut) const
{
    PulseqLoader* loader = m_pulseqLoader;
    if (!loader || !loader->hasTrajectoryData())
        return false;
    double tFactor = loader->getTFactor();
    if (tFactor <= 0.0)
        return false;
    double timeSec = (internalTime / tFactor) * 1e-6;
    return sampleTrajectoryPosition(timeSec, kxOut, kyOut, kzOut);
}

void MainWindow::exportTrajectory()
{
    PulseqLoader* loader = getPulseqLoader();
    if (!loader)
    {
        QMessageBox::warning(this, tr("No sequence loaded"),
                             tr("Load a Pulseq file before exporting the trajectory."));
        return;
    }

    loader->ensureTrajectoryPrepared();
    const QVector<double>& ktrajX = loader->getTrajectoryKx();
    const QVector<double>& ktrajY = loader->getTrajectoryKy();
    const QVector<double>& ktrajZ = loader->getTrajectoryKz();
    const QVector<double>& ktrajXAdc = loader->getTrajectoryKxAdc();
    const QVector<double>& ktrajYAdc = loader->getTrajectoryKyAdc();
    const QVector<double>& ktrajZAdc = loader->getTrajectoryKzAdc();

    if (ktrajX.isEmpty() || ktrajY.isEmpty())
    {
        QMessageBox::warning(this, tr("No trajectory data"),
                             tr("The current sequence has no computed k-space trajectory."));
        updateTrajectoryExportState();
        return;
    }
    if (ktrajXAdc.isEmpty() || ktrajYAdc.isEmpty())
    {
        QMessageBox::warning(this, tr("Missing ADC trajectory"),
                             tr("ADC sample trajectory is not available for this sequence."));
        updateTrajectoryExportState();
        return;
    }

    QString exportDir = QFileDialog::getExistingDirectory(
        this, tr("Select export folder"), QDir::currentPath());
    if (exportDir.isEmpty())
        return;

    QDir dir(exportDir);
    const QString ktrajPath = dir.filePath("ktraj.txt");
    const QString ktrajAdcPath = dir.filePath("ktraj_adc.txt");

    if (!writeTrajectoryFile(ktrajPath, ktrajX, ktrajY, ktrajZ))
    {
        QMessageBox::critical(this, tr("Export failed"),
                              tr("Unable to write %1.")
                                  .arg(QDir::toNativeSeparators(ktrajPath)));
        return;
    }
    if (!writeTrajectoryFile(ktrajAdcPath, ktrajXAdc, ktrajYAdc, ktrajZAdc))
    {
        QMessageBox::critical(this, tr("Export failed"),
                              tr("Unable to write %1.")
                                  .arg(QDir::toNativeSeparators(ktrajAdcPath)));
        return;
    }

    QMessageBox::information(
        this, tr("Trajectory exported"),
        tr("Saved trajectory files:\n%1\n%2")
            .arg(QDir::toNativeSeparators(ktrajPath))
            .arg(QDir::toNativeSeparators(ktrajAdcPath)));
}

void MainWindow::updateTrajectoryExportState()
{
    if (!m_pExportTrajectoryButton)
        return;
    PulseqLoader* loader = getPulseqLoader();
    bool hasData = loader && loader->hasTrajectoryData() &&
                   !loader->getTrajectoryKx().isEmpty() &&
                   !loader->getTrajectoryKxAdc().isEmpty() &&
                   !loader->getTrajectoryKy().isEmpty() &&
                   !loader->getTrajectoryKyAdc().isEmpty();
    m_pExportTrajectoryButton->setEnabled(hasData);
    if (hasData)
    {
        m_pExportTrajectoryButton->setToolTip(
            tr("Write ktraj.txt and ktraj_adc.txt for the current sequence."));
    }
    else
    {
        m_pExportTrajectoryButton->setToolTip(
            tr("Load a sequence and compute its trajectory to export."));
    }
}

void MainWindow::updateTrajectoryAxisLabels()
{
    if (!m_pTrajectoryPlot)
        return;
    Settings& settings = Settings::getInstance();
    const QString unit = settings.getTrajectoryUnitString();
    m_pTrajectoryPlot->xAxis->setLabel(QStringLiteral("k_x (%1)").arg(unit));
    m_pTrajectoryPlot->yAxis->setLabel(QStringLiteral("k_y (%1)").arg(unit));
}

void MainWindow::onSettingsChanged()
{
    Settings& s = Settings::getInstance();
    Settings::TrajectoryUnit currentUnit = s.getTrajectoryUnit();
    // If trajectory unit changed, drop cached base range so it will be recomputed in new units
    if (currentUnit != m_lastTrajectoryUnit)
    {
        m_lastTrajectoryUnit = currentUnit;
        m_trajectoryRangeInitialized = false;
    }

    updateTrajectoryAxisLabels();
    refreshTrajectoryPlotData();
    refreshTrajectoryCursor();
}

bool MainWindow::writeTrajectoryFile(const QString& path,
                                     const QVector<double>& kx,
                                     const QVector<double>& ky,
                                     const QVector<double>& kz)
{
    int count = std::min(kx.size(), ky.size());
    if (count <= 0)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    QTextStream out(&file);
    out.setRealNumberNotation(QTextStream::ScientificNotation);
    out.setRealNumberPrecision(12);

    for (int i = 0; i < count; ++i)
    {
        double kzVal = (i < kz.size()) ? kz[i] : 0.0;
        out << kx[i] << ' ' << ky[i] << ' ' << kzVal << '\n';
    }
    file.close();
    return true;
}

void MainWindow::updateTrajectoryCursorTime(double internalTime)
{
    m_currentTrajectoryTimeInternal = internalTime;
    m_hasTrajectoryCursorTime = true;
    refreshTrajectoryCursor();
}

void MainWindow::openLogWindow()
{
    // Reuse an existing dialog if it is already created; otherwise create one lazily.
    LogTableDialog* dlg = findChild<LogTableDialog*>("__SeqEyesLogDialog");
    if (!dlg)
    {
        dlg = new LogTableDialog(this);
        dlg->setObjectName("__SeqEyesLogDialog");
    }

    // Populate with current buffered log entries
    dlg->setInitialContent(LogManager::getInstance().getBufferedEntries());

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::onShowTrajectoryCursorToggled(bool checked)
{
    m_showTrajectoryCursor = checked;
    refreshTrajectoryCursor();
}

void MainWindow::onTrajectoryRangeModeChanged(int index)
{
    bool showWhole = (index == 1);
    bool colorWin = (index == 2);
    bool changed = (m_showWholeTrajectory != showWhole) || (m_colorCurrentWindow != colorWin);
    m_showWholeTrajectory = showWhole;
    m_colorCurrentWindow = colorWin;
    if (!changed) return;
    refreshTrajectoryPlotData();
}

void MainWindow::onTrajectorySeriesToggled()
{
    bool curveEnabled = m_pShowKtrajCheckBox ? m_pShowKtrajCheckBox->isChecked() : true;
    bool adcEnabled = m_pShowKtrajAdcCheckBox ? m_pShowKtrajAdcCheckBox->isChecked() : true;
    bool changed = (curveEnabled != m_showKtraj) || (adcEnabled != m_showKtrajAdc);
    m_showKtraj = curveEnabled;
    m_showKtrajAdc = adcEnabled;
    if (m_pTrajectoryCurve)
        m_pTrajectoryCurve->setVisible(m_showKtraj);
    if (m_pTrajectorySamplesGraph)
        m_pTrajectorySamplesGraph->setVisible(m_showKtrajAdc);
    if (changed && m_pTrajectoryPlot)
        m_pTrajectoryPlot->replot(QCustomPlot::rpQueuedReplot);
}

// Version info is auto-generated from Git metadata via CMake (see version_autogen.h).
#include <version_autogen.h>
// Manual app semantic version
#include "seqeyes_version.h"

void MainWindow::showAbout()
{
    QString versionHtml = QString(
        "<h3>SeqEyes</h3>"
        "<p>For viewing Pulseq sequence file, modified from <a href='https://github.com/xpjiang/PulseqViewer'>PulseqViewer</a></p>"
        "<p>See <a href='https://github.com/xingwangyong/seqeyes'>https://github.com/xingwangyong/seqeyes</a></p>"
        "<p><b>Version:</b> %1, %2, %3<br></p>")
        .arg(QString::fromUtf8(SEQEYES_APP_VERSION))
        .arg(QString::fromUtf8(SEQEYE_GIT_DATE))
        .arg(QString::fromUtf8(SEQEYE_GIT_HASH));

    QMessageBox::about(this, "About SeqEyes", versionHtml);
}

void MainWindow::showUsage()
{
    QMessageBox::information(this, "Usage Guide",
        "<h3>SeqEyes Usage Guide</h3>"

        "<p><b>Navigation & Viewing:</b><br>"
        "• <b>Zoom / Pan:</b> Controlled by Settings → Interactions.<br>"
        "&nbsp;&nbsp;Default: Zoom = Mouse wheel; Pan = Drag.<br>"
        "• <b>Reset View:</b> View → Reset View<br>"
        "• <b>Update Displayed Region:</b><br>"
        "&nbsp;&nbsp;Adjust the <b>Time</b> window, the <b>TR</b> range, or the <b>Block index</b> (Start–End/Inc) to change the visible portion of the sequence.</p>"
    );
}


void MainWindow::onShowFullDetailToggled(bool checked)
{
    // Toggle undersampling mode via single menu entry "Undersample curves"
    if (m_waveformDrawer) {
        // checked = true -> enable downsampling; checked = false -> full detail
        m_waveformDrawer->setUseDownsampling(checked);
        // Feedback
        statusBar()->showMessage(checked ? "Downsampling enabled" : "Full detail rendering enabled", 2000);
    }
}

void MainWindow::openFileFromCommandLine(const QString& filePath)
{
    // Validate file path
    if (filePath.isEmpty()) {
        qWarning() << "Empty file path provided";
        return;
    }
    
    // Check if file exists
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        qWarning() << "File does not exist:" << filePath;
        QMessageBox::warning(this, "File Error", 
            QString("File does not exist:\n%1").arg(filePath));
        return;
    }
    
    // Check if it's a .seq file
    if (!filePath.endsWith(".seq", Qt::CaseInsensitive)) {
        qWarning() << "File is not a .seq file:" << filePath;
        QMessageBox::warning(this, "File Error", 
            QString("Please select a .seq file:\n%1").arg(filePath));
        return;
    }
    
    // Use PulseqLoader to open the file
    if (m_pulseqLoader) {
        qDebug() << "Opening file from command line:" << filePath;
        // Set the file path cache and load the file
        m_pulseqLoader->setPulseqFilePathCache(filePath);
        if (!m_pulseqLoader->LoadPulseqFile(filePath)) {
            qWarning() << "Failed to load file:" << filePath;
            QMessageBox::critical(this, "File Error", 
                QString("Failed to load file:\n%1").arg(filePath));
        }
    } else {
        qWarning() << "PulseqLoader not available";
    }
}

void MainWindow::applyCommandLineOptions(const QCommandLineParser& parser)
{
    // Custom window title
    if (parser.isSet("name"))
    {
        m_customWindowTitle = parser.value("name");
        // Update window title immediately if a file is already loaded
        if (!m_loadedSeqFilePath.isEmpty())
        {
            setLoadedFileTitle(m_loadedSeqFilePath);
        }
        else
        {
            setWindowTitle(m_customWindowTitle);
        }
    }

    // Axis visibility
    if (m_trManager && m_waveformDrawer)
    {
        if (parser.isSet("no-ADC"))      { m_trManager->setShowADC(false); }
        if (parser.isSet("no-RFmag"))    { m_trManager->setShowRFMag(false); }
        if (parser.isSet("no-RFphase"))  { m_trManager->setShowRFPhase(false); }
        if (parser.isSet("no-Gx"))       { m_trManager->setShowGx(false); }
        if (parser.isSet("no-Gy"))       { m_trManager->setShowGy(false); }
        if (parser.isSet("no-Gz"))       { m_trManager->setShowGz(false); }
    }

    // Render mode
    if (m_trManager)
    {
        if (parser.isSet("TR-segmented"))
        {
            m_trManager->setRenderModeTrSegmented();
        }
        else if (parser.isSet("Whole-sequence"))
        {
            m_trManager->setRenderModeWholeSequence();
        }
    }

    // TR-range start~end
    if (m_trManager && parser.isSet("TR-range"))
    {
        const QString spec = parser.value("TR-range");
        auto parts = spec.split("~");
        if (parts.size() == 2)
        {
            bool ok1=false, ok2=false; int start = parts[0].toInt(&ok1); int end = parts[1].toInt(&ok2);
            if (ok1 && ok2 && start > 0 && end >= start)
            {
                m_trManager->getTrStartInput()->setText(QString::number(start));
                m_trManager->onTrStartInputChanged();
                m_trManager->getTrEndInput()->setText(QString::number(end));
                m_trManager->onTrEndInputChanged();
            }
        }
    }

    // time-range start~end (ms) for Whole-Sequence
    if (m_trManager && parser.isSet("time-range"))
    {
        const QString spec = parser.value("time-range");
        auto parts = spec.split("~");
        if (parts.size() == 2)
        {
            bool ok1=false, ok2=false; double start = parts[0].toDouble(&ok1); double end = parts[1].toDouble(&ok2);
            if (ok1 && ok2 && start >= 0 && end >= start)
            {
                // Force Whole-Sequence to make semantics consistent
                m_trManager->setRenderModeWholeSequence();
                m_trManager->getTimeStartInput()->setText(QString::number(static_cast<int>(std::round(start))));
                m_trManager->onTimeStartInputChanged();
                m_trManager->getTimeEndInput()->setText(QString::number(static_cast<int>(std::round(end))));
                m_trManager->onTimeEndInputChanged();
            }
        }
    }

    // layout abc (Matlab subplot style). E.g., 211 => rows=2, cols=1, index=1
    if (parser.isSet("layout"))
    {
        const QString spec = parser.value("layout").trimmed();
        if (spec.size() == 3 && spec[0].isDigit() && spec[1].isDigit() && spec[2].isDigit())
        {
            int rows = spec[0].digitValue();
            int cols = spec[1].digitValue();
            int index = spec[2].digitValue();
            // Position entire application window in the specified grid cell, spanning full screen
            // Compute screen geometry
            QRect screenGeom = screen()->availableGeometry();
            if (rows < 1) rows = 1; if (cols < 1) cols = 1; if (index < 1) index = 1;
            if (index > rows*cols) index = rows*cols;
            int r = (index-1) / cols; // 0-based row
            int c = (index-1) % cols; // 0-based col
            int cellW = screenGeom.width() / cols;
            int cellH = screenGeom.height() / rows;
            int x = screenGeom.x() + c * cellW;
            int y = screenGeom.y() + r * cellH;
            setGeometry(x, y, cellW, cellH);
        }
    }
}

void MainWindow::captureSnapshotsAndExit(const QString& outDir)
{
    // Ensure the window has a deterministic size and is shown so QCustomPlot layouts correctly
    this->resize(1280, 800);
    this->show();

    QTimer::singleShot(200, this, [this, outDir]() {
        QDir dir(outDir);
        if (!dir.exists()) {
            dir.mkpath(".");
        }

        QString baseName = QFileInfo(m_loadedSeqFilePath).baseName();
        if (baseName.isEmpty()) baseName = "unnamed";

        // Load deterministic font only for snapshot capture path.
        auto resolveSnapshotFont = []() -> QString {
            const int qrcFontId = QFontDatabase::addApplicationFont(":/fonts/NotoSans-Variable.ttf");
            if (qrcFontId >= 0) {
                const QStringList qrcFamilies = QFontDatabase::applicationFontFamilies(qrcFontId);
                if (!qrcFamilies.isEmpty()) {
                    qInfo() << "Deterministic snapshot font loaded from qrc family:" << qrcFamilies.first();
                    return qrcFamilies.first();
                }
            }

            const QString rel = "test/assets/fonts/Noto_Sans/NotoSans-VariableFont_wdth,wght.ttf";
            QStringList candidates;
            candidates << QDir::current().absoluteFilePath(rel);

            const QDir appDir(QCoreApplication::applicationDirPath());
            candidates << appDir.absoluteFilePath("../" + rel);
            candidates << appDir.absoluteFilePath("../../" + rel);
            candidates << appDir.absoluteFilePath("../../../" + rel);

            for (const QString& p : candidates) {
                QFileInfo fi(QDir::cleanPath(p));
                if (!fi.exists()) {
                    continue;
                }
                const int fontId = QFontDatabase::addApplicationFont(fi.absoluteFilePath());
                if (fontId < 0) {
                    continue;
                }
                const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
                if (!families.isEmpty()) {
                    qInfo() << "Deterministic snapshot font loaded from" << fi.absoluteFilePath()
                            << "family:" << families.first();
                    return families.first();
                }
            }
            qWarning() << "Deterministic snapshot font not found, fallback to Segoe UI.";
            return "Segoe UI";
        };

        const QString snapshotFontFamily = resolveSnapshotFont();
        const QFont snapshotFont(snapshotFontFamily, 9);

        auto applySnapshotFont = [snapshotFont](QCustomPlot* plot) {
            if (!plot) {
                return;
            }
            plot->setFont(snapshotFont);
            const auto axisRects = plot->axisRects();
            for (QCPAxisRect* rect : axisRects) {
                if (!rect) continue;
                const QList<QCPAxis*> axes{
                    rect->axis(QCPAxis::atLeft),
                    rect->axis(QCPAxis::atRight),
                    rect->axis(QCPAxis::atTop),
                    rect->axis(QCPAxis::atBottom)
                };
                for (QCPAxis* axis : axes) {
                    if (!axis) continue;
                    axis->setLabelFont(snapshotFont);
                    axis->setTickLabelFont(snapshotFont);
                }
            }
            if (plot->legend) {
                plot->legend->setFont(snapshotFont);
            }
        };

        auto savePlotViaPainter = [](QCustomPlot* plot, const QString& path, int width, int height) -> bool {
            if (!plot) {
                return false;
            }
            QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(1.0);
            image.fill(Qt::white);

            QCPPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            plot->toPainter(&painter, width, height);

            return image.save(path);
        };

        auto savePlotDeterministic = [savePlotViaPainter](QCustomPlot* plot, const QString& path, int width, int height) -> bool {
            if (!plot) {
                return false;
            }

            QVector<QCPLayer::LayerMode> originalModes;
            originalModes.reserve(plot->layerCount());
            for (int i = 0; i < plot->layerCount(); ++i) {
                QCPLayer* layer = plot->layer(i);
                if (!layer) {
                    originalModes.push_back(QCPLayer::lmLogical);
                    continue;
                }
                originalModes.push_back(layer->mode());
                layer->setMode(QCPLayer::lmLogical);
            }

            plot->replot(QCustomPlot::rpImmediateRefresh);
            const bool ok = savePlotViaPainter(plot, path, width, height);

            for (int i = 0; i < plot->layerCount() && i < originalModes.size(); ++i) {
                QCPLayer* layer = plot->layer(i);
                if (layer) {
                    layer->setMode(originalModes[i]);
                }
            }
            plot->replot(QCustomPlot::rpImmediateRefresh);
            return ok;
        };

        // 1. Sequence Diagram Snapshot
        // Re-apply the stored time range immediately before rendering.
        ui->customPlot->axisRect()->setAutoMargins(QCP::msNone);
        ui->customPlot->axisRect()->setMargins(QMargins(80, 20, 20, 40));
        if (m_trManager && m_interactionHandler && m_pulseqLoader) {
            bool ok1 = false, ok2 = false;
            double startMs = m_trManager->getTimeStartInput()->text().toDouble(&ok1);
            double endMs   = m_trManager->getTimeEndInput()->text().toDouble(&ok2);
            if (ok1 && ok2 && endMs > startMs) {
                double tf = m_pulseqLoader->getTFactor();
                m_interactionHandler->synchronizeXAxes(QCPRange(startMs * tf * 1000.0, endMs * tf * 1000.0));
            }
        }
        applySnapshotFont(ui->customPlot);
        ui->customPlot->replot(QCustomPlot::rpImmediateRefresh);

        QString seqPath = dir.absoluteFilePath(baseName + "_seq.png");
        if (savePlotDeterministic(ui->customPlot, seqPath, 1000, 600)) {
            qInfo() << "Saved sequence snapshot to" << seqPath;
        } else {
            qWarning() << "Failed to save sequence snapshot to" << seqPath;
        }

        // 2. Trajectory Diagram Snapshot
        setTrajectoryVisible(true);
        // We use a small delay to let the initial rendering and aspect ratio correction kick in
        QTimer::singleShot(300, this, [this, dir, baseName, applySnapshotFont, savePlotDeterministic]() {
            if (m_pTrajectoryPlot) {
                applySnapshotFont(m_pTrajectoryPlot);
                if (QCPAxisRect* axisRect = m_pTrajectoryPlot->axisRect()) {
                    axisRect->setAutoMargins(QCP::msNone);
                    axisRect->setMargins(QMargins(70, 20, 20, 50));
                }
                m_pTrajectoryPlot->replot(QCustomPlot::rpImmediateRefresh);
                QString trajPath = dir.absoluteFilePath(baseName + "_traj.png");
                if (savePlotDeterministic(m_pTrajectoryPlot, trajPath, 1000, 600)) {
                    qInfo() << "Saved trajectory snapshot to" << trajPath;
                } else {
                    qWarning() << "Failed to save trajectory snapshot to" << trajPath;
                }
            }
            // Done capturing
            QApplication::quit();
        });
    });
}
