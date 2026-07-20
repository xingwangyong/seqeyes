#include "PulseqLoader.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "WaveformDrawer.h"
#include "TRManager.h"
#include "SeriesBuilder.h"
#include "KSpaceTrajectory.h"
#include "InteractionHandler.h"
#include "LogManager.h"
#include "Settings.h"
#include <QCryptographicHash>

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <iostream>
#include <sstream>
#include <complex>
#include <cmath>
#include <array>
#include <algorithm>
#include <utility>
#include <QSet>
#include <thread>

#define SAFE_DELETE(p) { if(p) { delete p; p = nullptr; } }

namespace
{
struct KnownExtensionSpec
{
    const char* name;
    bool isFlag;
    int id;
};

const QVector<KnownExtensionSpec>& knownExtensionSpecs()
{
    static const QVector<KnownExtensionSpec> specs = {
        {"SLC", false, SLC}, {"SEG", false, SEG}, {"REP", false, REP}, {"AVG", false, AVG},
        {"SET", false, SET}, {"ECO", false, ECO}, {"PHS", false, PHS}, {"LIN", false, LIN},
        {"PAR", false, PAR}, {"ACQ", false, ACQ}, {"ONCE", false, ONCE},
        {"NAV", true, NAV}, {"REV", true, REV}, {"SMS", true, SMS}, {"REF", true, REF},
        {"IMA", true, IMA}, {"OFF", true, OFF}, {"NOISE", true, NOISE},
        {"PMC", true, PMC}, {"NOROT", true, NOROT}, {"NOPOS", true, NOPOS}, {"NOSCL", true, NOSCL},
    };
    return specs;
}

QString normalizedExtensionName(const std::string& name)
{
    return QString::fromStdString(name).trimmed().toUpper();
}

QString fallbackExtensionName(const QString& prefix, int id)
{
    return QString("%1[%2]").arg(prefix).arg(id).toUpper();
}

qint64 quantizeKey(double value, double scale)
{
    return qRound64(value * scale);
}

double wrapPhase0ToTau(double phase)
{
    const double tau = 2.0 * M_PI;
    double wrapped = std::fmod(phase, tau);
    if (wrapped < 0.0) {
        wrapped += tau;
    }
    return wrapped;
}

qint64 quantizedPhaseKey(double phase)
{
    return quantizeKey(wrapPhase0ToTau(phase), 1e6);
}

QString makeRfRoosTemplateKey(const RFEvent& rf, int rfLength, float dwellUs)
{
    return QStringLiteral("rf:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
        .arg(rfLength)
        .arg(quantizeKey(dwellUs, 1e3))
        .arg(rf.magShape)
        .arg(rf.phaseShape)
        .arg(rf.timeShape)
        .arg(quantizeKey(rf.amplitude, 1e6))
        .arg(rf.delay)
        .arg(quantizeKey(rf.freqOffset, 1e6))
        .arg(quantizeKey(rf.freqPPM, 1e6))
        .arg(quantizeKey(rf.phasePPM, 1e6));
}

QString makeAdcRoosTemplateKey(const ADCEvent& adc)
{
    return QStringLiteral("adc:%1:%2:%3:%4:%5:%6:%7")
        .arg(adc.numSamples)
        .arg(adc.dwellTime)
        .arg(adc.delay)
        .arg(adc.phaseModulationShape)
        .arg(quantizeKey(adc.freqOffset, 1e6))
        .arg(quantizeKey(adc.freqPPM, 1e6))
        .arg(quantizeKey(adc.phasePPM, 1e6));
}

QVector<int> detectRoosTimeShapeBoundaries(ExternalSequence* seq, int timeShapeId, int expectedLen = -1)
{
    QVector<int> boundaries;
    if (!seq || timeShapeId <= 0 || expectedLen <= 0) {
        if (!seq || timeShapeId <= 0) {
            return boundaries;
        }
    }

    std::vector<float> timeShape;
    if (!seq->getDecompressedShapeByID(timeShapeId, timeShape)) {
        return boundaries;
    }
    if (timeShape.empty()) {
        return boundaries;
    }
    if (expectedLen > 0 && int(timeShape.size()) != expectedLen) {
        return boundaries;
    }

    boundaries.append(0);
    const double eps = 1e-6;
    for (int i = 1; i < int(timeShape.size()); ++i) {
        if (double(timeShape[i]) + eps < double(timeShape[i - 1])) {
            boundaries.append(i);
        }
    }
    boundaries.append(int(timeShape.size()));
    return boundaries;
}

bool areUniformRoosSegments(const QVector<int>& boundaries, int* segmentLen = nullptr)
{
    if (boundaries.size() < 3) {
        return false;
    }

    const int len = boundaries[1] - boundaries[0];
    if (len <= 0) {
        return false;
    }
    for (int i = 1; i + 1 < boundaries.size(); ++i) {
        if ((boundaries[i + 1] - boundaries[i]) != len) {
            return false;
        }
    }
    if (segmentLen) {
        *segmentLen = len;
    }
    return true;
}

bool getRoosRawRfShapes(ExternalSequence* seq,
                        const RFEvent& rf,
                        std::vector<float>& amp,
                        std::vector<float>& phase)
{
    amp.clear();
    phase.clear();
    if (!seq || rf.magShape <= 0 || rf.phaseShape <= 0) {
        return false;
    }
    if (!seq->getDecompressedShapeByID(rf.magShape, amp)) {
        return false;
    }
    if (!seq->getDecompressedShapeByID(rf.phaseShape, phase)) {
        amp.clear();
        return false;
    }
    if (amp.size() != phase.size() || amp.empty()) {
        amp.clear();
        phase.clear();
        return false;
    }
    for (float& value : phase) {
        value *= float(2.0 * M_PI);
    }
    return true;
}
}

PulseqLoader::PulseqLoader(MainWindow* mainWindow)
    : QObject(mainWindow),
      m_mainWindow(mainWindow),
      m_sPulseqFilePath(""),
      m_sPulseqFilePathCache(""),
      m_sLastOpenDirectory(""),
      m_spPulseqSeq(nullptr), // Will be created based on file version
      m_dTotalDuration_us(0.),
      m_bHasRepetitionTime(false),
      m_dRepetitionTime_us(0.0),
      m_nTrCount(0),
      nBlockRangeStart(0),
      nBlockRangeEnd(0),
      tFactor(1e-3)
{
    m_listRecentPulseqFilePaths.resize(10);
    updateTimeUnitFromSettings();
    
    // Load last open directory from settings
    loadLastOpenDirectory();
    loadRecentFiles();
    updateRecentFilesMenu();
}

PulseqLoader::~PulseqLoader()
{
    // Destruction can happen during MainWindow teardown; avoid touching UI here.
    ClearPulseqCache(false);
}

void PulseqLoader::OpenPulseqFile()
{
#ifdef Q_OS_MAC
    qCritical() << "[MENU TRACE] PulseqLoader::OpenPulseqFile enter"
                << "mainEnabled=" << (m_mainWindow ? m_mainWindow->isEnabled() : false);
#endif
    // Use last open directory if available, otherwise use current path
    QString startDir = m_sLastOpenDirectory.isEmpty() ? QDir::currentPath() : m_sLastOpenDirectory;
    if (!QDir(startDir).exists())
    {
        startDir = QDir::homePath();
    }
    
    QFileDialog::Options options;
#ifdef Q_OS_MAC
    // macOS native panel can sporadically reject immediately in this app context.
    // Use Qt's dialog implementation for stable behavior.
    options |= QFileDialog::DontUseNativeDialog;
#endif
    QWidget* parentForDialog = m_mainWindow;
#ifdef Q_OS_MAC
    parentForDialog = nullptr;
#endif
    m_sPulseqFilePath = QFileDialog::getOpenFileName(
        parentForDialog,
        "Select a Pulseq File",
        startDir,
        "Text Files (*.seq);;All Files (*)",
        nullptr,
        options
    );

#ifdef Q_OS_MAC
    if (!m_sPulseqFilePath.isEmpty())
        qCritical() << "[MENU TRACE] PulseqLoader::OpenPulseqFile accepted" << m_sPulseqFilePath;
    else
        qCritical() << "[MENU TRACE] PulseqLoader::OpenPulseqFile canceled";
#endif

    if (!m_sPulseqFilePath.isEmpty())
    {
        // Save the directory of the selected file
        QFileInfo fileInfo(m_sPulseqFilePath);
        m_sLastOpenDirectory = fileInfo.absolutePath();
        saveLastOpenDirectory();
        
        if (!LoadPulseqFile(m_sPulseqFilePath))
        {
            m_sPulseqFilePath.clear();
            m_sPulseqFilePathCache.clear();
            std::cout << "LoadPulseqFile failed!\n";
        }
    }
}

void PulseqLoader::ReOpenPulseqFile()
{
    if (m_sPulseqFilePathCache.size() > 0)
    {
        // ClearPulseqCache(); Now LoadPulseqFile() already calls ClearPulseqCache()
        LoadPulseqFile(m_sPulseqFilePathCache);
    }
}

void PulseqLoader::ClearPulseqCache(bool withUi)
{
    m_sequenceLoadState = SequenceLoadState::Blank;
    ++m_trajectorySequenceGeneration;
    m_activeTrajectoryRequestId = 0;
    m_activePnsRequestId = 0;

    if (withUi && m_mainWindow)
    {
        if (auto* ih = m_mainWindow->getInteractionHandler())
            ih->cancelPendingViewportRenders();
        m_mainWindow->clearLoadedFileTitle();
        if (auto lbl = m_mainWindow->getVersionLabel()) { lbl->setText(""); lbl->setVisible(false); }
        if (auto pb = m_mainWindow->getProgressBar()) { pb->hide(); }
        if (auto* drawer = m_mainWindow->getWaveformDrawer())
            drawer->clearAllWaveformData();
        if (m_mainWindow->ui && m_mainWindow->ui->customPlot)
        {
            m_mainWindow->ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
        }
    }

    m_dTotalDuration_us = 0.;
    m_sPulseqFilePath.clear();
    m_bHasRepetitionTime = false;
    m_dRepetitionTime_us = 0.0;
    m_nTrCount = 0;
    m_vecTrBlockIndices.clear();

    // Clear RF/Gradient shape caches
    m_rfAmpCache.clear();
    m_rfPhCache.clear();
    m_gradShapeCache.clear();
    m_unifiedRfBlocks.clear();
    m_unifiedRfChannelCount = 1;
    m_detectedRoosPtxHack = false;
    m_unifiedRfStatusMessage.clear();
    m_supportsRfUseMetadata = false;
    m_hasEchoTimeDefinition = false;
    m_teTime_us = 0.0;
    m_teDurationAxis = 0.0;
    m_teDurationsAxis.clear();
    m_excitationCentersAxis.clear();
    m_refocusingCentersAxis.clear();
    m_rfUseGuessed = false;
    m_warnedRfUseGuess = false;
    m_rfGuessWarning.clear();
    m_kTrajectoryReady = false;
    m_kTrajectoryX.clear();
    m_kTrajectoryY.clear();
    m_kTrajectoryZ.clear();
    m_kTimeSec.clear();
    m_kTrajectoryXAdc.clear();
    m_kTrajectoryYAdc.clear();
    m_kTrajectoryZAdc.clear();
    m_kTimeAdcSec.clear();
    m_pnsResult = PnsCalculator::Result{};
    m_pnsState = PnsState::NotStarted;
    m_pnsDirty = true;
    m_lastPnsComputedSequenceGeneration = 0;
    m_lastPnsComputedAscPath.clear();
    m_lastPnsComputedGammaHzPerT = 0.0;
    m_pnsAscPath.clear();
    m_pnsStatusMessage.clear();
    m_safetyResult = SafetyResult{};
    // M1 (first gradient moment) cache reset alongside PNS.
    m_m1Result = M1Calculator::Result{};
    m_m1State = M1State::NotStarted;
    m_m1RequestSerial = 0;
    m_usedExtensions.clear();
    m_tridIdNames.clear();
    m_adcPhaseCache.valid = false;

    if (m_mainWindow && m_mainWindow->getTRManager())
    {
        m_mainWindow->getTRManager()->resetTimeWindow();
    }
    
    // Reset B0 to default
    m_b0Tesla = 0.0;

    if (nullptr != m_spPulseqSeq.get())
    {
        m_spPulseqSeq->reset();
        // m_spPulseqSeq will be recreated based on file version.
        // Release the loader's ownership of the decoded blocks. If an async
        // trajectory/PNS task is still running it holds its own shared_ptr to
        // the same bundle, so the blocks survive until that task finishes;
        // otherwise the bundle's refcount drops to zero here and the blocks are
        // freed immediately. This replaces the old single-slot handoff, which
        // could only protect one of two concurrent async tasks.
        if (m_blockBundle)
        {
            m_blockBundle.reset();
        }
        else
        {
            // Loose blocks not yet wrapped in a bundle (e.g. a partial decode
            // failure mid-load): free them directly.
            for (SeqBlock* block : m_vecDecodeSeqBlocks)
            {
                delete block;
            }
        }
        m_vecDecodeSeqBlocks.clear();
        std::cout << m_sPulseqFilePath.toStdString() << " Closed\n";
    }
    setTrajectoryState(TrajectoryState::NotStarted);
    emit pnsStateChanged();
    if (m_mainWindow) { m_mainWindow->setWindowFilePath(""); }
    emit pnsDataUpdated();
}

/**
 * @brief Read version information from Pulseq file without loading the full file
 * @param filename Path to the .seq file
 * @return Pair of (major, minor) version numbers, or (-1, -1) on error
 */
std::pair<int, int> PulseqLoader::ReadFileVersion(const std::string& filename)
{
    std::ifstream file(filename, std::ios::in);
    if (!file.is_open())
    {
        return std::make_pair(-1, -1);
    }

    std::string line;
    bool inVersionSection = false;
    int version_major = -1;
    int version_minor = -1;

    while (std::getline(file, line))
    {
        // Check for [VERSION] section
        if (line.find("[VERSION]") != std::string::npos)
        {
            inVersionSection = true;
            continue;
        }

        if (inVersionSection)
        {
            // Look for major and minor version lines
            if (line.find("major") != std::string::npos)
            {
                std::istringstream iss(line);
                std::string key, value;
                if (std::getline(iss, key, ' ') && std::getline(iss, value))
                {
                    try
                    {
                        version_major = std::stoi(value);
                    }
                    catch (const std::exception&)
                    {
                        return std::make_pair(-1, -1);
                    }
                }
            }
            else if (line.find("minor") != std::string::npos)
            {
                std::istringstream iss(line);
                std::string key, value;
                if (std::getline(iss, key, ' ') && std::getline(iss, value))
                {
                    try
                    {
                        version_minor = std::stoi(value);
                    }
                    catch (const std::exception&)
                    {
                        return std::make_pair(-1, -1);
                    }
                }
            }

            // If we found both major and minor versions, we can stop reading
            if (version_major >= 0 && version_minor >= 0)
            {
                break;
            }
        }

        // Stop if we encounter another section
        if (line.find("[") != std::string::npos && line.find("]") != std::string::npos && inVersionSection)
        {
            break;
        }
    }

    file.close();

    if (version_major >= 0 && version_minor >= 0)
    {
        return std::make_pair(version_major, version_minor);
    }

    return std::make_pair(-1, -1);
}

bool PulseqLoader::LoadPulseqFile(const QString& sPulseqFilePath)
{
    m_mainWindow->setEnabled(false);
    m_sequenceLoadState = SequenceLoadState::Loading;

    // Opening any sequence must invalidate all cached waveform/trajectory state
    // from the previous file before the new load begins. Clear unconditionally
    // instead of trying to detect "is there stale state": a missed condition is
    // exactly how reopen state leaks (the bug this path keeps regressing into).
    // On a blank loader ClearPulseqCache() is a cheap no-op, and it always bumps
    // the trajectory generation so any in-flight async task from the previous
    // file is discarded. Every entry point (Open / Reopen / Recent / drag-drop /
    // CLI) funnels through here, so this is the single authoritative reset point.
    ClearPulseqCache();

    m_sequenceLoadState = SequenceLoadState::Loading;

    // Keep the canonical loaded path for all loading entry points
    // (file dialog, drag/drop, command line, reopen).
    m_sPulseqFilePath = sPulseqFilePath;

    // First, read version information without loading the full file
    std::pair<int, int> version = ReadFileVersion(sPulseqFilePath.toStdString());
    if (version.first == -1 || version.second == -1)
    {
        m_sequenceLoadState = SequenceLoadState::Blank;
        m_mainWindow->setEnabled(true);
        std::stringstream sLog;
        sLog << "Failed to read version information from: " << sPulseqFilePath.toStdString();
        if (m_silentMode) { qWarning() << sLog.str().c_str(); }
        else { QMessageBox::critical(m_mainWindow, "Load Error", sLog.str().c_str()); }
        return false;
    }

    int version_major = version.first;
    int version_minor = version.second;

    // Create appropriate loader based on file version
    m_spPulseqSeq = CreateLoaderForVersion(version_major, version_minor);
    if (!m_spPulseqSeq)
    {
        m_sequenceLoadState = SequenceLoadState::Blank;
        m_mainWindow->setEnabled(true);
        std::stringstream sLog;
        sLog << "Unsupported Pulseq file version " << version_major << "." << version_minor << " for: " << sPulseqFilePath.toStdString();
        if (m_silentMode) { qWarning() << sLog.str().c_str(); }
        else { QMessageBox::critical(m_mainWindow, "Load Error", sLog.str().c_str()); }
        return false;
    }

    // ============================================================================
    // PULSEQ FILE LOADING WITH ERROR HANDLING
    // ============================================================================
    // Load the Pulseq file using the appropriate version-specific loader.
    // 
    // PULSEQ V1.4.X MANDATORY DEFINITIONS:
    // For Pulseq v1.4.x and later, the following definitions are MANDATORY
    // and must be present in the [DEFINITIONS] section of the .seq file:
    //
    // 1. AdcRasterTime - ADC sampling raster time (seconds)
    //    Example: AdcRasterTime = 1e-7  (100ns)
    //    Used for: ADC readout timing calculations
    //
    // 2. GradientRasterTime - Gradient raster time (seconds)  
    //    Example: GradientRasterTime = 1e-5  (10μs)
    //    Used for: Gradient timing and waveform calculations
    //
    // 3. RadiofrequencyRasterTime - RF raster time (seconds)
    //    Example: RadiofrequencyRasterTime = 1e-6  (1μs) 
    //    Used for: RF pulse timing calculations
    //
    // 4. BlockDurationRaster - Block duration raster time (seconds)
    //    Example: BlockDurationRaster = 1e-5  (10μs)
    //    Used for: Block duration calculations
    //
    // If any of these definitions are missing, the loader will fail with
    // detailed error messages indicating which definition is missing.
    // ============================================================================
    // Setup time units and factor before loading
    updateTimeUnitFromSettings();

    if (!m_spPulseqSeq->load(sPulseqFilePath.toStdString()))
    {
        m_sequenceLoadState = SequenceLoadState::Blank;
        m_mainWindow->setEnabled(true);
        std::stringstream sLog;
        sLog << "Failed to load Pulseq file: " << sPulseqFilePath.toStdString() << "\n\n";
        sLog << "Possible causes:\n";
        sLog << "1. Missing required definitions for:\n";
        sLog << "   - AdcRasterTime (ADC sampling raster time)\n";
        sLog << "   - GradientRasterTime (Gradient raster time)\n";
        sLog << "   - RadiofrequencyRasterTime (RF raster time)\n";
        sLog << "   - BlockDurationRaster (Block duration raster time)\n\n";
        sLog << "2. File format issues or corruption\n";
        sLog << "3. Unsupported Pulseq version\n\n";
        sLog << "Please check the console output for detailed error messages.";
        
        if (m_silentMode) { qWarning() << sLog.str().c_str(); }
        else { QMessageBox::critical(m_mainWindow, "Pulseq Load Error", sLog.str().c_str()); }
        return false;
    }
    // Do not use setWindowFilePath for the main window title, because it can auto-compose
    // "file - AppName" which conflicts with our explicit "SeqEyes - file.seq" title.
    if (m_mainWindow) { m_mainWindow->setWindowFilePath(QString()); }

    // Enforce presence of GradientRasterTime. If missing, abort load and inform user.
    {
        std::vector<double> gradDef = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        bool ok = !gradDef.empty() && std::isfinite(gradDef[0]) && gradDef[0] > 0.0;
        if (!ok)
        {
            m_sequenceLoadState = SequenceLoadState::Blank;
            m_mainWindow->setEnabled(true);
            const char* msg = "Missing required definition: GradientRasterTime (seconds)\n\n"
                              "The sequence lacks GradientRasterTime in [DEFINITIONS].\n"
                              "Please add e.g. 'GradientRasterTime 1e-05' and reload.";
            if (m_silentMode) { qWarning() << msg; }
            else { QMessageBox::critical(m_mainWindow, "Missing Definition", msg); }
            ClearPulseqCache();
            return false;
        }
    }

    LogManager::getInstance().appendStructured(
        QtInfoMsg,
        QStringLiteral("PulseqLoader"),
        QStringLiteral("Decoding blocks..."));
    qDebug() << "Total blocks:" << m_spPulseqSeq->GetNumberOfBlocks();
    
    // Debug: Check gradient library loading
    if (WaveformDrawer::DEBUG_GRADIENT_LIBRARY) {
        qDebug() << "=== GRADIENT LIBRARY DEBUG ===";
        qDebug() << "Checking gradient library contents...";
        
        // Check gradient raster time
        // For v151 version, use GetDefinition to get gradient raster time
        auto def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        if (!def.empty()) {
            double gradRaster_us = 1e6 * def[0]; // Convert from seconds to microseconds
            qDebug() << "Gradient raster time:" << gradRaster_us << "us";
        } else {
            qDebug() << "WARNING: GradientRasterTime definition not found";
        }
        
        // We need to access the gradient library from the sequence
        // Let's check a few sample blocks to see what gradient events are loaded
        int totalBlocks = m_spPulseqSeq->GetNumberOfBlocks();
        const int MAX_DEBUG_BLOCKS = 20; // Maximum number of blocks to check for debugging
        int blocksToCheck = qMin(MAX_DEBUG_BLOCKS, totalBlocks);
        
        qDebug() << "Total blocks in sequence:" << totalBlocks;
        qDebug() << "Checking first" << blocksToCheck << "blocks for gradient library debugging";
        
        for (int i = 0; i < blocksToCheck; i++) {
            auto block = m_spPulseqSeq->GetBlock(i);
            if (block) {
                qDebug() << "Block" << i << "gradient events:";
                for (int ch = 0; ch < 3; ch++) {
                    if (block->isTrapGradient(ch) || block->isArbitraryGradient(ch)) {
                        const auto& grad = block->GetGradEvent(ch);
                        qDebug() << "  Channel" << ch << "- Amplitude:" << grad.amplitude 
                                 << "Delay:" << grad.delay 
                                 << "RampUp:" << grad.rampUpTime 
                                 << "Flat:" << grad.flatTime 
                                 << "RampDown:" << grad.rampDownTime
                                 << "WaveShape:" << grad.waveShape
                                 << "TimeShape:" << grad.timeShape;
                    }
                }
            }
        }
        qDebug() << "=== END GRADIENT LIBRARY DEBUG ===";
    }

    const int& shVersion = m_spPulseqSeq->GetVersion();
    const int& shVersionMajor = shVersion / 1000000L;
    const int& shVersionMinor = (shVersion / 1000L) % 1000L;
    const int& shVersionRevision = shVersion % 1000L;
    QString sVersion = QString::number(shVersionMajor) + "." + QString::number(shVersionMinor) + "." + QString::number(shVersionRevision);
    m_pulseqVersionString = "v" + sVersion;
    // Do not show redundant version label in status bar; keep cached string only
    if (m_mainWindow->getVersionLabel()) m_mainWindow->getVersionLabel()->setVisible(false);

    const int64_t& lSeqBlockNum = m_spPulseqSeq->GetNumberOfBlocks();
    std::cout << lSeqBlockNum << " blocks detected!\n";
    m_vecDecodeSeqBlocks.resize(lSeqBlockNum);
    m_mainWindow->getProgressBar()->show();
    m_mainWindow->getProgressBar()->setValue(0);
    vecBlockEdges.clear();
    vecBlockEdges.resize(lSeqBlockNum + 1, 0);
    for (int64_t ushBlockIndex = 0; ushBlockIndex < lSeqBlockNum; ushBlockIndex++)
    {
        m_vecDecodeSeqBlocks[ushBlockIndex] = m_spPulseqSeq->GetBlock(ushBlockIndex);
        if (!m_spPulseqSeq->decodeBlock(m_vecDecodeSeqBlocks[ushBlockIndex]))
        {
            m_sequenceLoadState = SequenceLoadState::Blank;
            std::stringstream sLog;
            sLog << "Decode SeqBlock failed, block index: " << ushBlockIndex;
            LogManager::getInstance().appendStructured(
                QtCriticalMsg,
                QStringLiteral("PulseqLoader"),
                QString::fromStdString(sLog.str()),
                QStringLiteral("%1:%2").arg(QStringLiteral("PulseqLoader.cpp")).arg(__LINE__));
            if (m_silentMode) { qWarning() << sLog.str().c_str(); }
            else { QMessageBox::critical(m_mainWindow, "File Error", sLog.str().c_str()); }
            ClearPulseqCache();
            m_mainWindow->setEnabled(true);
            return false;
        }
        int progress = (ushBlockIndex + 1) * 100 / lSeqBlockNum;
        m_mainWindow->getProgressBar()->setValue(progress);
        vecBlockEdges[ushBlockIndex + 1] = vecBlockEdges[ushBlockIndex] + m_vecDecodeSeqBlocks[ushBlockIndex]->GetDuration() * tFactor;
    }
    // Decode succeeded: take shared ownership of the blocks. From here on the
    // bundle (not m_vecDecodeSeqBlocks) owns/deletes the SeqBlock*, and async
    // trajectory/PNS tasks keep a copy of this shared_ptr alive for as long as
    // they read the blocks. m_vecDecodeSeqBlocks remains a raw view used by the
    // synchronous code paths.
    m_blockBundle = std::make_shared<BlockBundle>();
    m_blockBundle->blocks = m_vecDecodeSeqBlocks;
    updateEchoAndExcitationMetadata(shVersionMajor, shVersionMinor);
    LogManager::getInstance().appendStructured(
        QtInfoMsg,
        QStringLiteral("PulseqLoader"),
        QStringLiteral("Decoding blocks finished"));

    // Prefer explicit TotalDuration from definitions if available
    // Otherwise, fall back to accumulated block edges
    std::vector<double> totalDurationDef = m_spPulseqSeq->GetDefinition("TotalDuration");
    if (!totalDurationDef.empty())
    {
        // TotalDuration is in seconds → convert to microseconds
        m_dTotalDuration_us = totalDurationDef[0] * 1e6;
    }
    else
    {
        m_dTotalDuration_us = vecBlockEdges[lSeqBlockNum] / tFactor;
    }
    std::cout << "Sequence total duration: " << m_dTotalDuration_us / 1e6 << " seconds" << std::endl;
    m_mainWindow->getProgressBar()->hide();

    // Build merged series once at load time (no zero padding, only NaN on real gaps)
    // Phase 1 optimization: skip building merged RF arrays (expensive for large sequences).
    // RF is rendered on-demand from per-shape cache with per-block scaling.
    m_rfTimeAmp.clear(); m_rfAmp.clear(); m_rfTimePh.clear(); m_rfPh.clear();
    
    // Phase 2: skip building merged gradient series; use on-demand per-shape cache
    m_gxTime.clear(); m_gxValues.clear();
    m_gyTime.clear(); m_gyValues.clear();
    m_gzTime.clear(); m_gzValues.clear();
    
    // Build merged ADC series
    SeriesBuilder::buildADCSeries(m_vecDecodeSeqBlocks, vecBlockEdges, tFactor, m_adcTime, m_adcValues);

    // Cache label/flag values after each block for fast UI queries (Information window).
    buildLabelSnapshotCache();
    parseTridIdNamesDefinition();

    nBlockRangeStart = 0;
    nBlockRangeEnd = std::min(int(lSeqBlockNum - 1), 10);

    // Precompute per-shape scale aggregates for RF/Gradients (single pass over blocks)
    buildShapeScaleAggregates();
    {
        QString unifiedRfError;
        if (!buildUnifiedRfBlocks(&unifiedRfError))
        {
            m_sequenceLoadState = SequenceLoadState::Blank;
            if (m_silentMode) { qWarning().noquote() << unifiedRfError; }
            else { QMessageBox::critical(m_mainWindow, "Pulseq Load Error", unifiedRfError); }
            ClearPulseqCache();
            m_mainWindow->setEnabled(true);
            return false;
        }
    }

    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    // Compute fixed Y-axis ranges based on full-sequence data to avoid per-TR/window autoscale jitter.
    // This keeps comparisons consistent when toggling TRs or panning/zooming.
    if (drawer) drawer->computeAndLockYAxisRanges();
    
        // Simple LOD system - no precomputation needed
    
    // Simple LOD system - no precomputation needed
    
    // Initial draw deferred: avoid duplicate heavy draws; final draw happens after TR setup below

    if (m_mainWindow->getWaveformDrawer()->getShowBlockEdges())
    {
        drawer->DrawBlockEdges();
    }

    double initialStartTime = 0.0;
    double initialEndTime = 1.0;
    if (lSeqBlockNum > 0)
    {
        // Determine initial view range based on render mode
        TRManager* trManager = m_mainWindow->getTRManager();

        if (trManager && hasRepetitionTime() && trManager->isTrBasedMode())
        {
        // TR-Segmented mode: show first TR
            double trDuration_us = getRepetitionTime_us();
            initialStartTime = 0;
            initialEndTime = trDuration_us * getTFactor();
        }
        else
        {
            // Whole-Sequence mode: show entire sequence (ensure non-negative)
            initialStartTime = std::max(vecBlockEdges[0], 0.0);  // Never negative
            initialEndTime = vecBlockEdges[lSeqBlockNum];  // Show entire sequence

            // Ensure valid range
            if (initialEndTime <= initialStartTime) initialEndTime = initialStartTime + 1.0;
            double totalDuration = getTotalDuration_us() * getTFactor();
            if (totalDuration > 0 && initialEndTime > totalDuration)
            {
                initialEndTime = totalDuration;
            }
        }

        // Save initial view state for reset functionality
        if (drawer)
        {
            // Save the calculated initial range (already validated)
            drawer->m_initialViewportLower = initialStartTime;
            drawer->m_initialViewportUpper = initialEndTime;
            drawer->m_initialViewSaved = true;
        }
    }
    // Update time axis label based on current layout - should be on the bottom-most axis
    if (drawer)
    {
        drawer->updateCurveVisibility();
    }

    // TR Detection
    std::vector<double> repTimeDef = m_spPulseqSeq->GetDefinition("RepetitionTime");
    std::vector<double> trDef = m_spPulseqSeq->GetDefinition("TR");

    if (!repTimeDef.empty())
    {
        m_dRepetitionTime_us = repTimeDef[0] * 1e6;
        m_bHasRepetitionTime = true;
    }
    else if (!trDef.empty())
    {
        m_dRepetitionTime_us = trDef[0] * 1e6;
        m_bHasRepetitionTime = true;
    }
    else
    {
        m_bHasRepetitionTime = false;
    }

    if (m_bHasRepetitionTime)
    {
        m_nTrCount = static_cast<int>(std::ceil(m_dTotalDuration_us / m_dRepetitionTime_us));
        m_vecTrBlockIndices.clear();
        for (int tr = 0; tr < m_nTrCount; ++tr)
        {
            double trStartTime = tr * m_dRepetitionTime_us * tFactor;
            int closestBlock = 0;
            double minDistance = std::numeric_limits<double>::max();
            for (int i = 0; i < lSeqBlockNum; ++i)
            {
                double blockStartTime = vecBlockEdges[i];
                double distance = std::abs(blockStartTime - trStartTime);
                if (distance < minDistance)
                {
                    minDistance = distance;
                    closestBlock = i;
                }
            }
            m_vecTrBlockIndices.push_back(closestBlock);
        }
    }
    else
    {
        m_vecTrBlockIndices.clear();
        for (int i = 0; i < lSeqBlockNum; ++i)
        {
            if (m_vecDecodeSeqBlocks[i]->isADC())
            {
                m_vecTrBlockIndices.push_back(i);
            }
        }
        m_nTrCount = m_vecTrBlockIndices.size();
    }

    // Update TR manager with new info
    TRManager* trManager = m_mainWindow->getTRManager();
    trManager->updateTrControls();
    trManager->refreshShowTeOverlay();

    m_sequenceLoadState = SequenceLoadState::Loaded;

    QCPRange finalRange(initialStartTime, initialEndTime);
    if (m_bHasRepetitionTime && m_mainWindow && drawer &&
        !drawer->getRects().isEmpty() && drawer->getRects()[0])
    {
        finalRange = drawer->getRects()[0]->axis(QCPAxis::atBottom)->range();
    }
    if (auto* ih = m_mainWindow->getInteractionHandler())
    {
        ih->synchronizeXAxes(finalRange);
    }
    else if (drawer)
    {
        drawer->ensureRenderedForCurrentViewport();
    }

    computeSafetyAnalysis(true);

    // PNS is always queued automatically after load. It is lower priority than
    // trajectory computation, so startPnsComputationIfEnabled() will defer while
    // trajectory is still calculating.
    startPnsComputationIfEnabled();

    // Keep UI updates only; drawing was already triggered via synchronizeXAxes
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
    {
        m_mainWindow->refreshTrajectoryPlotData();
    }
    if (m_mainWindow)
    {
        // Show "SeqEyes - file.seq" only after a successful load.
        m_mainWindow->setLoadedFileTitle(sPulseqFilePath);

        if (auto* coord = m_mainWindow->getCoordLabel())
        {
            qDebug().noquote()
                << "[UI_GEOM] after-load"
                << "windowSize=" << m_mainWindow->size()
                << "coordTextLen=" << coord->text().size()
                << "coordSizeHintW=" << coord->sizeHint().width()
                << "coordMinW=" << coord->minimumWidth();
        }
    }
    m_sPulseqFilePathCache = sPulseqFilePath;
    addRecentFile(sPulseqFilePath);
    startTrajectoryComputationIfEnabled();
    // M1 (first gradient moment) is computed asynchronously on every sequence
    // load. The result is cached and only displayed if the user enables the
    // M1x/M1y/M1z checkboxes, so there is no cost to running it up front.
    startM1ComputationAsync();
    m_mainWindow->setEnabled(true);
    return true;
}

void PulseqLoader::loadRecentFiles()
{
    QSettings settings;
    const QStringList recent = settings.value("recentPulseqFiles").toStringList();
    m_listRecentPulseqFilePaths = recent;
    while (m_listRecentPulseqFilePaths.size() > 10)
    {
        m_listRecentPulseqFilePaths.removeLast();
    }
}

void PulseqLoader::saveRecentFiles()
{
    QSettings settings;
    settings.setValue("recentPulseqFiles", m_listRecentPulseqFilePaths);
}

void PulseqLoader::addRecentFile(const QString& filePath)
{
    if (filePath.isEmpty())
        return;

    const QString normalized = QFileInfo(filePath).absoluteFilePath();
    if (normalized.isEmpty())
        return;

    m_listRecentPulseqFilePaths.removeAll(normalized);
    m_listRecentPulseqFilePaths.prepend(normalized);
    while (m_listRecentPulseqFilePaths.size() > 10)
    {
        m_listRecentPulseqFilePaths.removeLast();
    }

    saveRecentFiles();
    updateRecentFilesMenu();
}

void PulseqLoader::clearRecentFiles()
{
    m_listRecentPulseqFilePaths.clear();
    saveRecentFiles();
    updateRecentFilesMenu();
}

void PulseqLoader::updateRecentFilesMenu()
{
    if (!m_mainWindow || !m_mainWindow->ui || !m_mainWindow->ui->menuRecent_Files)
        return;

    QMenu* recentMenu = m_mainWindow->ui->menuRecent_Files;
    recentMenu->clear();

    QStringList validFiles;
    for (const QString& path : m_listRecentPulseqFilePaths)
    {
        if (path.isEmpty())
            continue;
        if (QFileInfo::exists(path))
            validFiles << path;
    }
    m_listRecentPulseqFilePaths = validFiles;

    if (m_listRecentPulseqFilePaths.isEmpty())
    {
        QAction* emptyAction = recentMenu->addAction("(No recent files)");
        emptyAction->setEnabled(false);
    }
    else
    {
        for (int i = 0; i < m_listRecentPulseqFilePaths.size(); ++i)
        {
            const QString path = m_listRecentPulseqFilePaths[i];
            QFileInfo fi(path);
            const QString label = QString("%1 %2").arg(i + 1).arg(fi.fileName());
            QAction* recentAction = recentMenu->addAction(label);
            recentAction->setToolTip(path);
            recentAction->setData(path);
            connect(recentAction, &QAction::triggered, this, [this, recentAction]() {
                const QString selectedPath = recentAction->data().toString();
                if (selectedPath.isEmpty())
                    return;
                m_sPulseqFilePath = selectedPath;
                QFileInfo fi(selectedPath);
                m_sLastOpenDirectory = fi.absolutePath();
                saveLastOpenDirectory();
                if (!LoadPulseqFile(selectedPath))
                {
                    m_sPulseqFilePathCache.clear();
                }
            });
        }
    }

    recentMenu->addSeparator();
    QAction* clearAction = recentMenu->addAction("Clear menu");
    connect(clearAction, &QAction::triggered, this, &PulseqLoader::clearRecentFiles);

    saveRecentFiles();
}

void PulseqLoader::buildLabelSnapshotCache()
{
    m_labelSnapshots.clear();
    m_usedExtensions.clear();
    m_maxAccumulatedCounter = 0;
    const int nBlocks = static_cast<int>(m_vecDecodeSeqBlocks.size());
    if (nBlocks <= 0)
        return;

    // Do NOT call pulseq's LabelStateAndBookkeeping::updateLabelValues here because
    // it can crash on unknown label IDs (>=1000) for LABELINC events. We apply events ourselves with bounds checks.
    QVector<int>  counterVal(NUM_LABELS, 0);
    QVector<bool> flagVal(NUM_FLAGS, false);
    QHash<QString, int> customCounterVal;

    m_labelSnapshots.resize(nBlocks);
    for (int i = 0; i < nBlocks; ++i)
    {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (blk && blk->isLabel())
        {
            auto seq = m_spPulseqSeq;
            auto markCounterUsed = [&](int id) {
                if (!seq) return;
                const std::string s = seq->getCounterIdAsString(id);
                if (!s.empty()) { m_usedExtensions.insert(normalizedExtensionName(s)); return; }
                const std::string u = seq->GetUnknownLabelName(id);
                if (!u.empty()) { m_usedExtensions.insert(normalizedExtensionName(u)); return; }
                m_usedExtensions.insert(fallbackExtensionName("LABEL", id));
            };
            auto markFlagUsed = [&](int id) {
                if (!seq) return;
                const std::string s = seq->getFlagIdAsString(id);
                if (!s.empty()) { m_usedExtensions.insert(normalizedExtensionName(s)); return; }
                m_usedExtensions.insert(fallbackExtensionName("FLAG", id));
            };
            auto customCounterName = [&](int id) -> QString {
                if (!seq) return fallbackExtensionName("LABEL", id);
                const std::string u = seq->GetUnknownLabelName(id);
                if (!u.empty()) return normalizedExtensionName(u);
                return fallbackExtensionName("LABEL", id);
            };

            // Apply LABELSET first, then LABELINC (same semantics as SeqPlot.m and pulseq runtime).
            const auto& sets = blk->GetLabelSetEvents();
            for (const auto& e : sets)
            {
                const int lblId = e.numVal.first;
                const int val = e.numVal.second;
                const int flagId = e.flagVal.first;
                const bool fval = e.flagVal.second;

                if (lblId >= 0 && lblId < NUM_LABELS && lblId != LABEL_UNKNOWN)
                {
                    counterVal[lblId] = val;
                    markCounterUsed(lblId);
                }
                else if (lblId != LABEL_UNKNOWN)
                {
                    const QString name = customCounterName(lblId);
                    customCounterVal.insert(name, val);
                    m_usedExtensions.insert(name);
                }
                if (flagId >= 0 && flagId < NUM_FLAGS && flagId != FLAG_UNKNOWN)
                {
                    flagVal[flagId] = fval;
                    markFlagUsed(flagId);
                }
            }
            const auto& incs = blk->GetLabelIncEvents();
            for (const auto& e : incs)
            {
                const int lblId = e.numVal.first;
                const int val = e.numVal.second;
                if (lblId >= 0 && lblId < NUM_LABELS && lblId != LABEL_UNKNOWN)
                {
                    counterVal[lblId] += val;
                    markCounterUsed(lblId);
                }
                else if (lblId != LABEL_UNKNOWN)
                {
                    const QString name = customCounterName(lblId);
                    customCounterVal[name] = customCounterVal.value(name, 0) + val;
                    m_usedExtensions.insert(name);
                }
            }
        }

        LabelSnapshot snap;
        snap.counters = counterVal;
        snap.flags = flagVal;
        snap.customCounters = customCounterVal;
        m_labelSnapshots[i] = snap;

        // Track max accumulated counter value across all blocks (used for ADC Y-range)
        for (int c : counterVal)
            m_maxAccumulatedCounter = std::max(m_maxAccumulatedCounter, std::abs(c));
        for (auto it = customCounterVal.constBegin(); it != customCounterVal.constEnd(); ++it)
            m_maxAccumulatedCounter = std::max(m_maxAccumulatedCounter, std::abs(it.value()));
    }
}

void PulseqLoader::parseTridIdNamesDefinition()
{
    m_tridIdNames.clear();
    if (!m_spPulseqSeq)
        return;

    const std::string raw = m_spPulseqSeq->GetDefinitionStr("TridIdName");
    if (raw.empty())
        return;

    const QStringList parts = QString::fromStdString(raw)
                                  .split(',', Qt::KeepEmptyParts);
    for (QString part : parts)
    {
        part = part.trimmed();
        if (!part.isEmpty())
            m_tridIdNames.append(part);
    }
}

const PulseqLoader::LabelSnapshot* PulseqLoader::labelSnapshotAfterBlock(int blockIdx) const
{
    if (blockIdx < 0 || blockIdx >= m_labelSnapshots.size())
        return nullptr;
    return &m_labelSnapshots[blockIdx];
}

bool PulseqLoader::getCounterValueAfterBlock(int blockIdx, int counterId, int& outVal) const
{
    outVal = 0;
    const LabelSnapshot* s = labelSnapshotAfterBlock(blockIdx);
    if (!s) return false;
    if (counterId < 0 || counterId >= s->counters.size()) return false;
    outVal = s->counters[counterId];
    return true;
}

bool PulseqLoader::getFlagValueAfterBlock(int blockIdx, int flagId, bool& outVal) const
{
    outVal = false;
    const LabelSnapshot* s = labelSnapshotAfterBlock(blockIdx);
    if (!s) return false;
    if (flagId < 0 || flagId >= s->flags.size()) return false;
    outVal = s->flags[flagId];
    return true;
}

bool PulseqLoader::getExtensionValueAfterBlock(int blockIdx, const QString& name, int& outVal, bool& isFlag) const
{
    outVal = 0;
    isFlag = false;

    const LabelSnapshot* snap = labelSnapshotAfterBlock(blockIdx);
    if (!snap)
        return false;

    const QString normalized = name.trimmed().toUpper();
    for (const auto& spec : knownExtensionSpecs())
    {
        if (normalized != QLatin1String(spec.name))
            continue;

        isFlag = spec.isFlag;
        if (spec.isFlag)
        {
            if (spec.id < 0 || spec.id >= snap->flags.size())
                return false;
            outVal = snap->flags[spec.id] ? 1 : 0;
            return true;
        }

        if (spec.id < 0 || spec.id >= snap->counters.size())
            return false;
        outVal = snap->counters[spec.id];
        return true;
    }

    const auto it = snap->customCounters.constFind(normalized);
    if (it == snap->customCounters.constEnd())
        return false;

    outVal = it.value();
    return true;
}

QString PulseqLoader::formatExtensionLabelLine(const QString& label, int value, bool isFlag) const
{
    const QString normalized = label.trimmed().toUpper();
    QString line = QString("%1=%2").arg(normalized).arg(value);

    if (!isFlag && normalized == QStringLiteral("TRID") && value >= 1 && value <= m_tridIdNames.size())
    {
        const QString tridName = m_tridIdNames[value - 1].trimmed();
        if (!tridName.isEmpty())
            line += QString(", %1").arg(tridName);
    }

    return line;
}

QStringList PulseqLoader::getAvailableExtensionLabels() const
{
    QStringList labels = Settings::getSupportedExtensionLabels();
    for (const QString& used : m_usedExtensions)
    {
        if (!labels.contains(used, Qt::CaseInsensitive))
            labels.append(used);
    }
    labels.sort(Qt::CaseInsensitive);
    return labels;
}

QStringList PulseqLoader::getActiveLabelLines(int blockIdx) const
{
    QStringList lines;
    if (!labelSnapshotAfterBlock(blockIdx))
        return lines;

    const QStringList labels = getAvailableExtensionLabels();
    for (const QString& label : labels)
    {
        if (!Settings::getInstance().isExtensionLabelEnabled(label))
            continue;
        if (!m_usedExtensions.contains(label.toUpper()))
            continue;

        int value = 0;
        bool isFlag = false;
        if (!getExtensionValueAfterBlock(blockIdx, label, value, isFlag))
            continue;

        if (isFlag)
        {
            if (value != 0)
                lines.append(formatExtensionLabelLine(label, 1, true));
        }
        else
        {
            lines.append(formatExtensionLabelLine(label, value, false));
        }
    }

    std::sort(lines.begin(), lines.end(), [](const QString& a, const QString& b) {
        return a < b;
    });
    return lines;
}

void PulseqLoader::setBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock)
{
    if (!dialog) return;

    QString blockInfo = QString("/-----------------------------------------------------------------------------------------------/\n");
    blockInfo += QString("Block: %1\nStart Time: %2 %3\nEnd Time: %4 %5\n")
        .arg(currentBlock)
        .arg(vecBlockEdges[currentBlock])
        .arg(TimeUnits)
        .arg(vecBlockEdges[currentBlock + 1])
        .arg(TimeUnits);

    const auto& pSeqBlock = m_vecDecodeSeqBlocks[currentBlock];
    if (pSeqBlock->isRF())
    {
        blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
        const RFEvent& rf = pSeqBlock->GetRFEvent();
        blockInfo += QString("RF Event:\nAmplitude: %1 Hz\nFrequency Offset: %2 Hz\nPhase Offset: %3 rad\nDelay: %4 us\n")
            .arg(rf.amplitude)
            .arg(rf.freqOffset)
            .arg(rf.phaseOffset)
            .arg(rf.delay);
    }

    if (pSeqBlock->isADC())
    {
        blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
        const ADCEvent& adc = pSeqBlock->GetADCEvent();
        blockInfo += QString("ADC Event:\nNumber of Samples: %1\nDwell Time: %2 ns\nDelay: %3 us\nFrequency Offset: %4 Hz\nPhase Offset: %5 rad\n")
            .arg(adc.numSamples)
            .arg(adc.dwellTime)
            .arg(adc.delay)
            .arg(adc.freqOffset)
            .arg(adc.phaseOffset);
    }

    if (pSeqBlock->isTrigger())
    {
        blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
        const TriggerEvent& trg = pSeqBlock->GetTriggerEvent();
        blockInfo += QString("Trigger Event:\nType: %1\nChannel: %2\nDelay: %3 us\nDuration: %4 us\n")
            .arg(trg.triggerType)
            .arg(trg.triggerChannel)
            .arg(trg.delay)
            .arg(trg.duration);
    }

    std::array<QString, 3> gradChannels = { "Gx", "Gy", "Gz" };
    Settings& gradSettings = Settings::getInstance();
    const QString gradDispUnit = gradSettings.getGradientUnitString();
    for (int channel = 0; channel < 3; ++channel)
    {
        if (pSeqBlock->isTrapGradient(channel) || pSeqBlock->isArbitraryGradient(channel) || pSeqBlock->isExtTrapGradient(channel))
        {
            blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
            const GradEvent& grad = pSeqBlock->GetGradEvent(channel);
            double dispAmp = grad.amplitude; // internal unit is Hz/m
            if (gradDispUnit != "Hz/m")
                dispAmp = gradSettings.convertGradient(dispAmp, "Hz/m", gradDispUnit);
            blockInfo += QString("Gradient Event (Channel %1):\nAmplitude: %2 %3\nDelay: %4 us")
                .arg(gradChannels[channel])
                .arg(dispAmp)
                .arg(gradDispUnit)
                .arg(grad.delay);

            if (pSeqBlock->isTrapGradient(channel))
            {
                blockInfo += QString("\nRamp Up Time: %1 us\nFlat Time: %2 us\nRamp Down Time: %3 us")
                    .arg(grad.rampUpTime)
                    .arg(grad.flatTime)
                    .arg(grad.rampDownTime);
            }
            else if (pSeqBlock->isArbitraryGradient(channel))
            {
                blockInfo += QString("\nWave Shape ID: %1\nTime Shape ID: %2")
                    .arg(grad.waveShape)
                    .arg(grad.timeShape);
            }
            blockInfo += QString("\n");
        }
    }

    // Extensions summary (current values, not operations)
    {
        blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
        blockInfo += QString("Extensions (Current values at this block):\n");

        const QStringList activeLabelLines = getActiveLabelLines(currentBlock);
        if (!activeLabelLines.isEmpty())
        {
            for (const QString& line : activeLabelLines)
            {
                blockInfo += QString("  %1\n").arg(line);
            }
        }
        else
        {
            blockInfo += QString("  (No enabled extension labels)\n");
        }
    }

    blockInfo += QString("\\-----------------------------------------------------------------------------------------------\\");
    dialog->setInfoContent(blockInfo);
}

void PulseqLoader::setRawBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock)
{
    if (!dialog) return;
    if (currentBlock < 0 || currentBlock >= static_cast<int>(m_vecDecodeSeqBlocks.size())) return;

    SeqBlock* blk = m_vecDecodeSeqBlocks[currentBlock];
    if (!blk) return;

    QString s;
    s += QString("/-----------------------------------------------------------------------------------------------/\n");
    s += QString("Raw block data (approx):\n");
    s += QString("# Format: NUM DUR RF  GX  GY  GZ  ADC  EXT\n");

    const long durRu = blk->GetStoredDuration_ru();
    const int rf  = blk->GetEventIndex(Event::RF);
    const int gx  = blk->GetEventIndex(Event::GX);
    const int gy  = blk->GetEventIndex(Event::GY);
    const int gz  = blk->GetEventIndex(Event::GZ);
    const int adc = blk->GetEventIndex(Event::ADC);
    const int ext = blk->GetEventIndex(Event::EXT);
    s += QString("%1 %2 %3 %4 %5 %6 %7 %8\n")
        .arg(currentBlock)
        .arg(durRu)
        .arg(rf)
        .arg(gx)
        .arg(gy)
        .arg(gz)
        .arg(adc)
        .arg(ext);

    // Minimal extra hints (can be expanded later).
    if (blk->isRF())
    {
        const RFEvent& rfEv = blk->GetRFEvent();
        s += QString("\nRF: magShape=%1, phaseShape=%2, timeShape=%3\n")
            .arg(rfEv.magShape)
            .arg(rfEv.phaseShape)
            .arg(rfEv.timeShape);
    }
    for (int ch = 0; ch < 3; ++ch)
    {
        if (blk->isArbitraryGradient(ch) || blk->isExtTrapGradient(ch))
        {
            const GradEvent& g = blk->GetGradEvent(ch);
            s += QString("G%1: waveShape=%2, timeShape=%3\n").arg(ch==0?"x":(ch==1?"y":"z")).arg(g.waveShape).arg(g.timeShape);
        }
    }

    s += QString("\\-----------------------------------------------------------------------------------------------\\");
    dialog->setInfoContent(s);
}

void PulseqLoader::setManualRepetitionTime(double trValue)
{
    m_dRepetitionTime_us = trValue * 1e6; // Convert to microseconds
    m_bHasRepetitionTime = true;

    m_nTrCount = static_cast<int>(std::ceil(m_dTotalDuration_us / m_dRepetitionTime_us));

    m_vecTrBlockIndices.clear();
    for (int tr = 0; tr < m_nTrCount; ++tr)
    {
        double trStartTime = tr * m_dRepetitionTime_us * tFactor;
        int closestBlock = 0;
        double minDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < m_vecDecodeSeqBlocks.size(); ++i)
        {
            double blockStartTime = vecBlockEdges[i];
            double distance = std::abs(blockStartTime - trStartTime);
            if (distance < minDistance)
            {
                minDistance = distance;
                closestBlock = i;
            }
        }
        m_vecTrBlockIndices.push_back(closestBlock);
    }
}

bool PulseqLoader::IsBlockRf(const float* fAmp, const float* fPhase, const int& iSamples)
{
    // This function seems to be unused, but I'll keep it for completeness
    for (int i = 0; i < iSamples; i++)
    {
        if (0 != fAmp[i] || 0 != fPhase[i])
        {
            return true;
        }
    }
    return false;
}

void PulseqLoader::updateEchoAndExcitationMetadata(int versionMajor, int versionMinor)
{
    m_excitationCentersAxis.clear();
    m_refocusingCentersAxis.clear();
    m_hasEchoTimeDefinition = false;
    m_teTime_us = 0.0;
    m_teDurationAxis = 0.0;
    m_teDurationsAxis.clear();
    m_supportsRfUseMetadata = (versionMajor > 1) || (versionMajor == 1 && versionMinor >= 5);
    m_rfUseGuessed = false;
    m_rfGuessWarning.clear();

    if (!m_spPulseqSeq)
        return;

    std::vector<double> teDef = m_spPulseqSeq->GetDefinition("EchoTime");
    if (teDef.empty())
        teDef = m_spPulseqSeq->GetDefinition("TE");
    if (!teDef.empty())
    {
        m_hasEchoTimeDefinition = true;
        m_teDurationsAxis.reserve(static_cast<qsizetype>(teDef.size()));
        for (double teSec : teDef)
        {
            const double teUs = teSec * 1e6;
            m_teDurationsAxis.append(teUs * tFactor);
        }
        m_teTime_us = teDef[0] * 1e6;
        m_teDurationAxis = m_teDurationsAxis.first();
    }

    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
        return;
}


void PulseqLoader::setTrajectoryState(TrajectoryState state)
{
    if (m_trajectoryState == state)
        return;
    m_trajectoryState = state;
    emit trajectoryStateChanged();
}

void PulseqLoader::setPnsState(PnsState state)
{
    if (m_pnsState == state)
        return;
    m_pnsState = state;
    emit pnsStateChanged();
}

void PulseqLoader::markPnsDirty()
{
    m_pnsDirty = true;
}

bool PulseqLoader::shouldRecomputePns() const
{
    const QString ascPath = Settings::getInstance().getPnsAscPath().trimmed();
    const double gammaHzPerT = Settings::getInstance().getGamma();
    if (m_pnsDirty)
        return true;
    if (m_lastPnsComputedSequenceGeneration != m_trajectorySequenceGeneration)
        return true;
    if (m_lastPnsComputedAscPath != ascPath)
        return true;
    if (!qFuzzyCompare(m_lastPnsComputedGammaHzPerT + 1.0, gammaHzPerT + 1.0))
        return true;
    return false;
}

KSpaceTrajectory::Input PulseqLoader::buildKSpaceTrajectoryInput() const
{
    double gradRasterUs = -1.0;
    double rfRasterUs = -1.0;
    if (m_spPulseqSeq) {
        std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) {
            gradRasterUs = def[0] * 1e6;
        }
        def = m_spPulseqSeq->GetDefinition("RadiofrequencyRasterTime");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) {
            rfRasterUs = def[0] * 1e6;
        }
    }

    QVector<double> adcEventTimes;
    if (!m_vecDecodeSeqBlocks.empty() && vecBlockEdges.size() >= 2) {
        qsizetype totalSamples = 0;
        for (SeqBlock* blk : m_vecDecodeSeqBlocks) {
            if (!blk || !blk->isADC())
                continue;
            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples > 0)
                totalSamples += adc.numSamples;
        }
        if (totalSamples > 0)
            adcEventTimes.reserve(totalSamples);

        for (int i = 0; i < static_cast<int>(m_vecDecodeSeqBlocks.size()); ++i) {
            SeqBlock* blk = m_vecDecodeSeqBlocks[i];
            if (!blk || !blk->isADC())
                continue;
            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples <= 0 || adc.dwellTime <= 0)
                continue;
            double dwellUs = static_cast<double>(adc.dwellTime) * 1e-3;
            double dwellInternal = dwellUs * tFactor;
            double startInternal = vecBlockEdges[i] + adc.delay * tFactor + 0.5 * dwellInternal;
            for (int sample = 0; sample < adc.numSamples; ++sample) {
                adcEventTimes.append(startInternal + sample * dwellInternal);
            }
        }
    }

    double b0Tesla = 0.0;
    if (m_spPulseqSeq) {
        std::vector<double> defB0 = m_spPulseqSeq->GetDefinition("B0");
        if (!defB0.empty())
            b0Tesla = defB0[0];
    }
    if (b0Tesla == 0.0) {
        b0Tesla = 3.0;
    }

    return KSpaceTrajectory::Input { m_vecDecodeSeqBlocks,
                                     vecBlockEdges,
                                     tFactor,
                                     m_supportsRfUseMetadata,
                                     rfRasterUs,
                                     gradRasterUs,
                                     std::move(adcEventTimes),
                                     b0Tesla };
}

void PulseqLoader::applyTrajectoryResult(const KSpaceTrajectory::Result& result)
{
    m_excitationCentersAxis = result.excitationTimesInternal;
    m_refocusingCentersAxis = result.refocusingTimesInternal;
    m_kTrajectoryX = result.kx;
    m_kTrajectoryY = result.ky;
    m_kTrajectoryZ = result.kz;
    m_kTimeSec = result.t;
    m_kTrajectoryXAdc = result.kx_adc;
    m_kTrajectoryYAdc = result.ky_adc;
    m_kTrajectoryZAdc = result.kz_adc;
    m_kTimeAdcSec = result.t_adc;
    m_rfUseGuessed = result.rfUseGuessed;
    m_rfGuessWarning = result.warning;
    m_rfUsePerBlock = result.rfUsePerBlock;
    m_kTrajectoryReady = true;
    setTrajectoryState(TrajectoryState::Ready);
    emit trajectoryDataUpdated();
}

void PulseqLoader::computeKSpaceTrajectory()
{
    if (!m_spPulseqSeq || m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
    {
        m_kTrajectoryReady = false;
        setTrajectoryState(TrajectoryState::Failed);
        return;
    }

    KSpaceTrajectory::Input input = buildKSpaceTrajectoryInput();
    m_b0Tesla = input.b0Tesla;
    KSpaceTrajectory::Result result = KSpaceTrajectory::compute(input);
    applyTrajectoryResult(result);
}

void PulseqLoader::startTrajectoryComputationAsync()
{
    if (!m_spPulseqSeq || m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
    {
        m_kTrajectoryReady = false;
        setTrajectoryState(TrajectoryState::NotStarted);
        return;
    }

    const KSpaceTrajectory::Input input = buildKSpaceTrajectoryInput();
    m_b0Tesla = input.b0Tesla;
    const std::uint64_t sequenceGeneration = m_trajectorySequenceGeneration;
    const std::uint64_t requestId = ++m_trajectoryRequestSerial;
    m_activeTrajectoryRequestId = requestId;
    m_kTrajectoryReady = false;
    setTrajectoryState(TrajectoryState::Calculating);

    QPointer<PulseqLoader> self(this);
    // Keep the decoded blocks alive for the duration of this task, even if the
    // sequence is reopened/cleared while we are still computing.
    const auto blockBundle = m_blockBundle;
    // KSpaceTrajectory::Input holds *references* to blocks/blockEdges. If they
    // stayed bound to the loader members (m_vecDecodeSeqBlocks/vecBlockEdges),
    // this worker would read them while a concurrent reopen clears/reallocates
    // them on the main thread -> use-after-free. Capture owned copies and rebuild
    // Input from those inside the thread, mirroring the PNS worker. blockBundle
    // keeps the pointed-to SeqBlock objects alive.
    std::thread([self, sequenceGeneration, requestId, blockBundle,
                 blocks = m_vecDecodeSeqBlocks,
                 blockEdges = vecBlockEdges,
                 tFactor = input.tFactor,
                 supportsRfUse = input.supportsRfUseMetadata,
                 rfRasterUs = input.rfRasterUs,
                 gradientRasterUs = input.gradientRasterUs,
                 adcEventTimes = input.adcEventTimesInternal,
                 b0Tesla = input.b0Tesla]() mutable {
        KSpaceTrajectory::Input localInput { blocks, blockEdges, tFactor, supportsRfUse,
                                             rfRasterUs, gradientRasterUs,
                                             std::move(adcEventTimes), b0Tesla };
        KSpaceTrajectory::Result result;
        bool ok = true;
        try {
            result = KSpaceTrajectory::compute(localInput);
        } catch (...) {
            ok = false;
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, result = std::move(result), ok, sequenceGeneration, requestId]() mutable {
            if (!self)
                return;
            if (self->m_trajectorySequenceGeneration != sequenceGeneration)
                return;
            if (self->m_activeTrajectoryRequestId != requestId)
                return;

            if (!ok)
            {
                self->m_kTrajectoryReady = false;
                self->setTrajectoryState(TrajectoryState::Failed);
                self->startPnsComputationIfEnabled();
                return;
            }

            self->applyTrajectoryResult(result);
            self->startPnsComputationIfEnabled();
        }, Qt::QueuedConnection);
    }).detach();
}

void PulseqLoader::startTrajectoryComputationIfEnabled()
{
    if (!m_autoStartTrajectoryAfterLoad)
        return;
    if (m_trajectoryState == TrajectoryState::Calculating || m_trajectoryState == TrajectoryState::Ready)
        return;
    startTrajectoryComputationAsync();
}

void PulseqLoader::computeSafetyAnalysis(bool showWarningDialog)
{
    m_safetyResult = SafetyResult{};
    const Settings::SystemProfile profile = Settings::getInstance().getActiveSystemProfile();
    m_safetyResult.profileAlias = profile.alias.trimmed();

    if (!m_spPulseqSeq || m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
        return;

    auto maxAbsOfRange = [](const QPair<double, double>& range) {
        return std::max(std::abs(range.first), std::abs(range.second));
    };
    auto channelNameFor = [](int channel) -> QString {
        switch (channel)
        {
        case 0: return QStringLiteral("X");
        case 1: return QStringLiteral("Y");
        case 2: return QStringLiteral("Z");
        default: return QStringLiteral("?");
        }
    };
    struct MetricLocation
    {
        double measuredHzUnits {0.0};
        int blockIndex {-1};
        double timeUs {std::numeric_limits<double>::quiet_NaN()};
        double blockStartUs {std::numeric_limits<double>::quiet_NaN()};
        double blockEndUs {std::numeric_limits<double>::quiet_NaN()};
        QString channel;
        bool valid {false};
    };
    auto updateMetricLocation = [&](MetricLocation& location,
                                    double measuredHzUnits,
                                    int blockIndex,
                                    double timeUs,
                                    double blockStartUs,
                                    double blockEndUs,
                                    const QString& channel) {
        if (measuredHzUnits < location.measuredHzUnits)
            return;
        location.measuredHzUnits = measuredHzUnits;
        location.blockIndex = blockIndex;
        location.timeUs = timeUs;
        location.blockStartUs = blockStartUs;
        location.blockEndUs = blockEndUs;
        location.channel = channel;
        location.valid = true;
    };

    double measuredGradHzPerM = 0.0;
    for (int ch = 0; ch < 3; ++ch)
        measuredGradHzPerM = std::max(measuredGradHzPerM, maxAbsOfRange(getGradGlobalRange(ch)));

    double measuredB1Hz = 0.0;
    const QPair<double, double> rfRange = getRfGlobalRangeAmp();
    measuredB1Hz = maxAbsOfRange(rfRange);

    double maxSlewHzPerMPerS = 0.0;
    MetricLocation maxGradLocation;
    MetricLocation maxSlewLocation;
    std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
    const double gradientRasterSec = (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) ? def[0] : 0.0;
    const double gradientRasterUs = gradientRasterSec * 1e6;

    for (int blockIndex = 0; blockIndex < static_cast<int>(m_vecDecodeSeqBlocks.size()); ++blockIndex)
    {
        SeqBlock* blk = m_vecDecodeSeqBlocks[blockIndex];
        if (!blk)
            continue;
        const double blockStartUs = vecBlockEdges[blockIndex] / tFactor;
        const double blockEndUs = vecBlockEdges[blockIndex + 1] / tFactor;
        for (int ch = 0; ch < 3; ++ch)
        {
            if (!(blk->isTrapGradient(ch) || blk->isArbitraryGradient(ch) || blk->isExtTrapGradient(ch)))
                continue;

            const GradEvent& grad = blk->GetGradEvent(ch);
            const QString channel = channelNameFor(ch);
            const double gradStartUs = blockStartUs + double(grad.delay);
            if (blk->isTrapGradient(ch))
            {
                updateMetricLocation(maxGradLocation,
                                     std::abs(double(grad.amplitude)),
                                     blockIndex,
                                     gradStartUs + std::max(0L, grad.rampUpTime),
                                     blockStartUs,
                                     blockEndUs,
                                     channel);
                if (grad.rampUpTime > 0)
                {
                    const double slew = std::abs(double(grad.amplitude)) / (double(grad.rampUpTime) * 1e-6);
                    if (slew >= maxSlewHzPerMPerS)
                    {
                        maxSlewHzPerMPerS = slew;
                        updateMetricLocation(maxSlewLocation,
                                             slew,
                                             blockIndex,
                                             gradStartUs + 0.5 * double(grad.rampUpTime),
                                             blockStartUs,
                                             blockEndUs,
                                             channel);
                    }
                }
                if (grad.rampDownTime > 0)
                {
                    const double slew = std::abs(double(grad.amplitude)) / (double(grad.rampDownTime) * 1e-6);
                    if (slew >= maxSlewHzPerMPerS)
                    {
                        maxSlewHzPerMPerS = slew;
                        updateMetricLocation(maxSlewLocation,
                                             slew,
                                             blockIndex,
                                             gradStartUs + double(grad.rampUpTime) + double(grad.flatTime) + 0.5 * double(grad.rampDownTime),
                                             blockStartUs,
                                             blockEndUs,
                                             channel);
                    }
                }
                continue;
            }

            if (blk->isArbitraryGradient(ch))
            {
                const int n = blk->GetArbGradNumSamples(ch);
                const float* shape = blk->GetArbGradShapePtr(ch);
                if (n <= 0 || !shape || gradientRasterSec <= 0.0)
                    continue;
                const bool oversampled = blk->isArbGradWithOversampling(ch) || (grad.timeShape == -1);
                const double dtSec = oversampled ? (0.5 * gradientRasterSec) : gradientRasterSec;
                const double dtUs = oversampled ? (0.5 * gradientRasterUs) : gradientRasterUs;
                const double tFirstUs = gradStartUs + 0.5 * gradientRasterUs;
                double prev = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double value = double(shape[i]) * double(grad.amplitude);
                    updateMetricLocation(maxGradLocation,
                                         std::abs(value),
                                         blockIndex,
                                         tFirstUs + double(i) * dtUs,
                                         blockStartUs,
                                         blockEndUs,
                                         channel);
                    const double slew = std::abs(value - prev) / dtSec;
                    if (slew >= maxSlewHzPerMPerS)
                    {
                        maxSlewHzPerMPerS = slew;
                        updateMetricLocation(maxSlewLocation,
                                             slew,
                                             blockIndex,
                                             (tFirstUs + double(i) * dtUs) - 0.5 * dtUs,
                                             blockStartUs,
                                             blockEndUs,
                                             channel);
                    }
                    prev = value;
                }
                const double tailSlew = std::abs(prev) / dtSec;
                if (tailSlew >= maxSlewHzPerMPerS)
                {
                    maxSlewHzPerMPerS = tailSlew;
                    updateMetricLocation(maxSlewLocation,
                                         tailSlew,
                                         blockIndex,
                                         tFirstUs + double(n - 1) * dtUs + 0.5 * dtUs,
                                         blockStartUs,
                                         blockEndUs,
                                         channel);
                }
                continue;
            }

            if (blk->isExtTrapGradient(ch))
            {
                const std::vector<long>& times = blk->GetExtTrapGradTimes(ch);
                const std::vector<float>& shape = blk->GetExtTrapGradShape(ch);
                if (times.size() != shape.size() || times.size() < 2)
                    continue;
                for (size_t i = 0; i < times.size(); ++i)
                {
                    const double value = double(shape[i]) * double(grad.amplitude);
                    updateMetricLocation(maxGradLocation,
                                         std::abs(value),
                                         blockIndex,
                                         gradStartUs + double(times[i]),
                                         blockStartUs,
                                         blockEndUs,
                                         channel);
                }
                for (size_t i = 1; i < times.size(); ++i)
                {
                    const double dtSec = (double(times[i]) - double(times[i - 1])) * 1e-6;
                    if (dtSec <= 0.0)
                        continue;
                    const double v0 = double(shape[i - 1]) * double(grad.amplitude);
                    const double v1 = double(shape[i]) * double(grad.amplitude);
                    const double slew = std::abs(v1 - v0) / dtSec;
                    if (slew >= maxSlewHzPerMPerS)
                    {
                        maxSlewHzPerMPerS = slew;
                        updateMetricLocation(maxSlewLocation,
                                             slew,
                                             blockIndex,
                                             gradStartUs + 0.5 * (double(times[i - 1]) + double(times[i])),
                                             blockStartUs,
                                             blockEndUs,
                                             channel);
                    }
                }
            }
        }
    }

    const double gammaHzPerT = Settings::getInstance().getGamma();
    const double measuredGradMtPerM = Settings::getInstance().convertGradient(measuredGradHzPerM, "Hz/m", "mT/m");
    const double measuredSlewTPerMPerS = Settings::getInstance().convertSlew(maxSlewHzPerMPerS, "Hz/m/s", "T/m/s");
    const double measuredB1uT = (gammaHzPerT > 0.0) ? (measuredB1Hz / gammaHzPerT * 1e6) : 0.0;

    auto applyMetricLocation = [&](SafetyMetric& metric, const MetricLocation& location, double scale) {
        if (!location.valid)
            return;
        metric.hasLocation = true;
        metric.blockIndex = location.blockIndex;
        metric.timeUs = location.timeUs;
        metric.blockStartUs = location.blockStartUs;
        metric.blockEndUs = location.blockEndUs;
        metric.channel = location.channel;
        metric.measured = location.measuredHzUnits * scale;
    };
    auto fillMetric = [&](SafetyMetric& metric, double measured, double limit) {
        metric.configured = std::isfinite(limit) && limit > 0.0;
        metric.measured = measured;
        metric.limit = metric.configured ? limit : 0.0;
        metric.passed = !metric.configured || measured <= limit;
        if (metric.configured)
            m_safetyResult.hasAnyChecks = true;
        if (metric.configured && !metric.passed)
            m_safetyResult.hasViolation = true;
    };

    fillMetric(m_safetyResult.maxGrad, measuredGradMtPerM, profile.maxGrad);
    fillMetric(m_safetyResult.maxSlew, measuredSlewTPerMPerS, profile.maxSlew);
    fillMetric(m_safetyResult.maxB1, measuredB1uT, profile.maxB1);
    applyMetricLocation(m_safetyResult.maxGrad,
                        maxGradLocation,
                        Settings::getInstance().convertGradient(1.0, "Hz/m", "mT/m"));
    applyMetricLocation(m_safetyResult.maxSlew,
                        maxSlewLocation,
                        Settings::getInstance().convertSlew(1.0, "Hz/m/s", "T/m/s"));

    if (!m_safetyResult.hasAnyChecks)
    {
        m_safetyResult.summary = QStringLiteral("Safety checks are not configured.");
        return;
    }

    QStringList failed;
    auto appendFailure = [&](const QString& name, const SafetyMetric& metric, const QString& unit) {
        if (!metric.configured || metric.passed)
            return;
        auto formatField = [](const QString& label, const QString& value) {
            return QStringLiteral("  %1  %2")
                .arg(label.leftJustified(11, QLatin1Char(' ')))
                .arg(value);
        };

        QStringList lines;
        lines << name
              << formatField(QStringLiteral("measured"),
                             QStringLiteral("%1 %2").arg(metric.measured, 0, 'f', 3).arg(unit))
              << formatField(QStringLiteral("limit"),
                             QStringLiteral("%1 %2").arg(metric.limit, 0, 'f', 3).arg(unit));
        if (metric.hasLocation)
        {
            lines << formatField(QStringLiteral("channel"),
                                 metric.channel.isEmpty() ? QStringLiteral("?") : metric.channel)
                  << formatField(QStringLiteral("block index"),
                                 QString::number(metric.blockIndex))
                  << formatField(QStringLiteral("time"),
                                 QStringLiteral("%1 ms").arg(metric.timeUs / 1000.0, 0, 'f', 3))
                  << formatField(QStringLiteral("block range"),
                                 QStringLiteral("%1 - %2 ms")
                                     .arg(metric.blockStartUs / 1000.0, 0, 'f', 3)
                                     .arg(metric.blockEndUs / 1000.0, 0, 'f', 3));
        }
        failed.append(lines.join(QStringLiteral("\n")));
    };
    appendFailure(QStringLiteral("maxGrad"), m_safetyResult.maxGrad, QStringLiteral("mT/m"));
    appendFailure(QStringLiteral("maxSlew"), m_safetyResult.maxSlew, QStringLiteral("T/m/s"));
    appendFailure(QStringLiteral("maxB1"), m_safetyResult.maxB1, QStringLiteral("uT"));

    if (m_safetyResult.hasViolation)
    {
        m_safetyResult.summary = QStringLiteral("Safety warning");
        m_safetyResult.warningMessage =
            QStringLiteral("<html><body><p>Safety limits of system profile &quot;%1&quot; exceeded.</p><pre>%2</pre></body></html>")
                .arg((m_safetyResult.profileAlias.isEmpty() ? QStringLiteral("(unnamed)") : m_safetyResult.profileAlias).toHtmlEscaped(),
                     failed.join(QStringLiteral("\n\n")).toHtmlEscaped());
        if (showWarningDialog && !m_silentMode && m_mainWindow)
        {
            QMessageBox::warning(m_mainWindow, QStringLiteral("Safety warning"), m_safetyResult.warningMessage);
        }
    }
    else
    {
        m_safetyResult.summary = QStringLiteral("Safety OK");
    }
}

void PulseqLoader::computePnsSynchronously()
{
    m_pnsResult = PnsCalculator::Result{};
    m_pnsStatusMessage.clear();
    m_pnsAscPath = Settings::getInstance().getPnsAscPath().trimmed();

    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2 || !m_spPulseqSeq)
    {
        m_pnsStatusMessage = QStringLiteral("Load a sequence to compute PNS.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    if (m_pnsAscPath.isEmpty())
    {
        m_pnsStatusMessage = QStringLiteral("PNS is not configured. Select a system profile with a valid ASC path in Settings > Safety.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    if (!QFileInfo::exists(m_pnsAscPath))
    {
        m_pnsStatusMessage = QStringLiteral("PNS ASC file not found: %1").arg(m_pnsAscPath);
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    PnsCalculator::Hardware hw;
    QString parseError;
    if (!PnsCalculator::parseAscFile(m_pnsAscPath, hw, &parseError))
    {
        m_pnsStatusMessage = parseError;
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
    if (def.empty() || !std::isfinite(def[0]) || def[0] <= 0.0)
    {
        m_pnsStatusMessage = QStringLiteral("GradientRasterTime definition is missing.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    const double gradientRasterUs = def[0] * 1e6;
    const double gammaHzPerT = Settings::getInstance().getGamma();
    m_pnsResult = PnsCalculator::calculate(
        m_vecDecodeSeqBlocks,
        vecBlockEdges,
        tFactor,
        gradientRasterUs,
        gammaHzPerT,
        hw);

    if (!m_pnsResult.valid)
    {
        m_pnsStatusMessage = m_pnsResult.error;
        setPnsState(PnsState::Failed);
    }
    else
    {
        m_pnsStatusMessage = m_pnsResult.ok
            ? QStringLiteral("PNS prediction OK (max < 100%).")
            : QStringLiteral("PNS warning: predicted level reaches/exceeds 100%.");
        setPnsState(PnsState::Ready);
    }
    emit pnsDataUpdated();
}

void PulseqLoader::startPnsComputationAsync()
{
    m_pnsResult = PnsCalculator::Result{};
    m_pnsStatusMessage.clear();
    m_pnsAscPath = Settings::getInstance().getPnsAscPath().trimmed();

    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2 || !m_spPulseqSeq)
    {
        m_pnsStatusMessage = QStringLiteral("Load a sequence to compute PNS.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    if (m_pnsAscPath.isEmpty())
    {
        m_pnsStatusMessage = QStringLiteral("PNS is not configured. Select a system profile with a valid ASC path in Settings > Safety.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    if (!QFileInfo::exists(m_pnsAscPath))
    {
        m_pnsStatusMessage = QStringLiteral("PNS ASC file not found: %1").arg(m_pnsAscPath);
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    PnsCalculator::Hardware hw;
    QString parseError;
    if (!PnsCalculator::parseAscFile(m_pnsAscPath, hw, &parseError))
    {
        m_pnsStatusMessage = parseError;
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
    if (def.empty() || !std::isfinite(def[0]) || def[0] <= 0.0)
    {
        m_pnsStatusMessage = QStringLiteral("GradientRasterTime definition is missing.");
        setPnsState(PnsState::Failed);
        emit pnsDataUpdated();
        return;
    }

    const double gradientRasterUs = def[0] * 1e6;
    const double gammaHzPerT = Settings::getInstance().getGamma();
    const QString ascPath = m_pnsAscPath;
    const std::uint64_t sequenceGeneration = m_trajectorySequenceGeneration;
    const std::uint64_t requestId = ++m_pnsRequestSerial;
    m_activePnsRequestId = requestId;
    // Keep the decoded blocks alive for the duration of this task, even if the
    // sequence is reopened/cleared while we are still computing.
    const auto blockBundle = m_blockBundle;
    setPnsState(PnsState::Calculating);
    emit pnsDataUpdated();

    QPointer<PulseqLoader> self(this);
    std::thread([self, blockBundle, hw, gradientRasterUs, gammaHzPerT, ascPath, sequenceGeneration, requestId,
                 blocks = m_vecDecodeSeqBlocks, blockEdges = vecBlockEdges, tf = tFactor]() mutable {
        PnsCalculator::Result result;
        bool ok = true;
        try {
            result = PnsCalculator::calculate(
                blocks,
                blockEdges,
                tf,
                gradientRasterUs,
                gammaHzPerT,
                hw);
        } catch (...) {
            ok = false;
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, result = std::move(result), ok, ascPath, sequenceGeneration, requestId, gammaHzPerT]() mutable {
            if (!self)
                return;
            if (self->m_trajectorySequenceGeneration != sequenceGeneration)
                return;
            if (self->m_activePnsRequestId != requestId)
                return;

            self->m_pnsAscPath = ascPath;
            if (!ok)
            {
                self->m_pnsResult = PnsCalculator::Result{};
                self->m_pnsStatusMessage = QStringLiteral("PNS calculation failed.");
                self->setPnsState(PnsState::Failed);
                emit self->pnsDataUpdated();
                return;
            }

            self->m_pnsResult = result;
            if (!self->m_pnsResult.valid)
            {
                self->m_pnsStatusMessage = self->m_pnsResult.error;
                self->setPnsState(PnsState::Failed);
            }
            else
            {
                self->m_pnsStatusMessage = self->m_pnsResult.ok
                    ? QStringLiteral("PNS prediction OK (max < 100%).")
                    : QStringLiteral("PNS warning: predicted level reaches/exceeds 100%.");
                self->setPnsState(PnsState::Ready);
                self->m_pnsDirty = false;
                self->m_lastPnsComputedSequenceGeneration = sequenceGeneration;
                self->m_lastPnsComputedAscPath = ascPath;
                self->m_lastPnsComputedGammaHzPerT = gammaHzPerT;
            }
            emit self->pnsDataUpdated();
        }, Qt::QueuedConnection);
    }).detach();
}

void PulseqLoader::startM1ComputationAsync()
{
    if (!m_spPulseqSeq || m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
    {
        m_m1Result = M1Calculator::Result{};
        m_m1Result.valid = false;
        m_m1Result.ok = false;
        m_m1Result.error = QStringLiteral("Empty or invalid block list.");
        setM1State(M1State::NotStarted);
        emit m1DataUpdated();
        return;
    }

    // Build the same Input shape KSpaceTrajectory uses; we only need blocks,
    // edges, tFactor, and the raster-time hints.
    double gradRasterUs = -1.0;
    double rfRasterUs = -1.0;
    if (m_spPulseqSeq) {
        std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) {
            gradRasterUs = def[0] * 1e6;
        }
        def = m_spPulseqSeq->GetDefinition("RadiofrequencyRasterTime");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) {
            rfRasterUs = def[0] * 1e6;
        }
    }

    QVector<double> adcEventTimes;
    if (!m_vecDecodeSeqBlocks.empty() && vecBlockEdges.size() >= 2) {
        qsizetype totalSamples = 0;
        for (SeqBlock* blk : m_vecDecodeSeqBlocks) {
            if (!blk || !blk->isADC())
                continue;
            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples > 0)
                totalSamples += adc.numSamples;
        }
        if (totalSamples > 0)
            adcEventTimes.reserve(totalSamples);

        for (int i = 0; i < static_cast<int>(m_vecDecodeSeqBlocks.size()); ++i) {
            SeqBlock* blk = m_vecDecodeSeqBlocks[i];
            if (!blk || !blk->isADC())
                continue;
            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples <= 0 || adc.dwellTime <= 0)
                continue;
            double dwellUs = static_cast<double>(adc.dwellTime) * 1e-3;
            double dwellInternal = dwellUs * tFactor;
            double startInternal = vecBlockEdges[i] + adc.delay * tFactor + 0.5 * dwellInternal;
            for (int sample = 0; sample < adc.numSamples; ++sample) {
                adcEventTimes.append(startInternal + sample * dwellInternal);
            }
        }
    }

    double b0Tesla = 0.0;
    if (m_spPulseqSeq) {
        std::vector<double> defB0 = m_spPulseqSeq->GetDefinition("B0");
        if (!defB0.empty())
            b0Tesla = defB0[0];
    }
    if (b0Tesla == 0.0) {
        b0Tesla = 3.0;
    }

    const M1Calculator::Input input { m_vecDecodeSeqBlocks,
                                     vecBlockEdges,
                                     tFactor,
                                     m_supportsRfUseMetadata,
                                     rfRasterUs,
                                     gradRasterUs,
                                     adcEventTimes,
                                     b0Tesla };
    const std::uint64_t requestId = ++m_m1RequestSerial;
    setM1State(M1State::Calculating);

    QPointer<PulseqLoader> self(this);
    const auto blockBundle = m_blockBundle;
    std::thread([self, input, requestId, blockBundle]() mutable {
        M1Calculator::Result result;
        bool ok = true;
        try {
            result = M1Calculator::compute(input);
        } catch (...) {
            ok = false;
        }

        if (!self)
            return;

        QMetaObject::invokeMethod(self, [self, result = std::move(result), ok, requestId]() mutable {
            if (!self)
                return;
            if (self->m_m1RequestSerial != requestId)
                return; // superseded by a newer request
            if (!ok)
            {
                self->m_m1Result = M1Calculator::Result{};
                self->m_m1Result.valid = false;
                self->m_m1Result.ok = false;
                self->m_m1Result.error = QStringLiteral("M1 computation failed.");
                LogManager::getInstance().appendStructured(
                    QtWarningMsg,
                    QStringLiteral("M1"),
                    QStringLiteral("Computation failed: worker threw an exception."));
                self->setM1State(M1State::Failed);
                emit self->m1DataUpdated();
                return;
            }
            self->applyM1Result(result);
        }, Qt::QueuedConnection);
    }).detach();
}

void PulseqLoader::applyM1Result(const M1Calculator::Result& result)
{
    m_m1Result = result;
    m_m1Result.valid = result.ok && result.valid;
    if (m_m1Result.ok)
    {
        setM1State(M1State::Ready);
    }
    else
    {
        setM1State(M1State::Failed);
    }
    emit m1DataUpdated();
}

void PulseqLoader::setM1State(M1State state)
{
    if (m_m1State == state)
        return;
    m_m1State = state;
    emit m1StateChanged();
}

void PulseqLoader::startPnsComputationIfEnabled()
{
    if (!m_autoStartPnsAfterLoad)
        return;
    if (m_trajectoryState == TrajectoryState::Calculating)
        return;
    if (m_pnsState == PnsState::Calculating)
        return;
    if (!shouldRecomputePns())
        return;
    startPnsComputationAsync();
}

void PulseqLoader::ensureTrajectoryPrepared()
{
    if (m_kTrajectoryReady)
        return;
    if (m_trajectoryState == TrajectoryState::Calculating)
        return;

    const std::uint64_t requestId = ++m_trajectoryRequestSerial;
    m_activeTrajectoryRequestId = requestId;
    m_kTrajectoryReady = false;
    setTrajectoryState(TrajectoryState::Calculating);
    computeKSpaceTrajectory();
}

bool PulseqLoader::waitForBackgroundComputations(int timeoutMs)
{
    if (timeoutMs < 0)
        timeoutMs = 0;

    QElapsedTimer timer;
    timer.start();
    while (isTrajectoryCalculating() || isPnsCalculating())
    {
        if (timer.elapsed() >= timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

QVector<double> PulseqLoader::getKxKyZeroTimes() const
{
    QVector<double> result;
    if (!m_kTrajectoryReady || m_kTrajectoryX.isEmpty() || m_kTrajectoryY.isEmpty() || m_kTimeSec.isEmpty())
        return result;

    // Calculate tolerance based on FOV (deltak = 1/FOV)
    // Use 0.2 * deltak as tolerance to account for numerical precision and interpolation
    double kTolerance = 1e-3; // Default fallback tolerance
    if (m_spPulseqSeq)
    {
        std::vector<double> def = m_spPulseqSeq->GetDefinition("FOV");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0)
        {
            double fovMeters = def[0];
            double deltak = 1.0 / fovMeters; // k-space sampling interval
            kTolerance = deltak * 0.2; // Use 20% of deltak as tolerance
        }
    }

    // Collect candidate kx=ky=0 times
    QVector<double> candidates;

    // FIRST: Check ADC samples directly - these are the most reliable
    const int nAdc = qMin(qMin(m_kTrajectoryXAdc.size(), m_kTrajectoryYAdc.size()), m_kTimeAdcSec.size());
    for (int i = 0; i < nAdc; ++i)
    {
        double kx = m_kTrajectoryXAdc[i];
        double ky = m_kTrajectoryYAdc[i];
        
        if (!std::isfinite(kx) || !std::isfinite(ky))
            continue;
        
        // Check if both kx and ky are near zero at this ADC sample
        if (qAbs(kx) <= kTolerance && qAbs(ky) <= kTolerance)
        {
            double tSec = m_kTimeAdcSec[i];
            candidates.append(tSec);
        }
    }

    // SECOND: Check trajectory zero crossings for points near ADC samples
    const int n = qMin(qMin(m_kTrajectoryX.size(), m_kTrajectoryY.size()), m_kTimeSec.size());
    if (n < 2)
    {
        // If no trajectory, just return ADC results
        std::sort(candidates.begin(), candidates.end());
        double dupToleranceSec = 1e-6;
        candidates.erase(std::unique(candidates.begin(), candidates.end(), [dupToleranceSec](double a, double b) {
            return qAbs(a - b) < dupToleranceSec;
        }), candidates.end());
        
        // Convert to axis units
        for (double tSec : candidates)
        {
            double tAxis = tSec * 1e6 * getTFactor();
            result.append(tAxis);
        }
        return result;
    }

    // Only check adjacent point pairs - this limits interpolation to immediate neighbors
    for (int i = 1; i < n; ++i)
    {
        double kx0 = m_kTrajectoryX[i - 1];
        double kx1 = m_kTrajectoryX[i];
        double ky0 = m_kTrajectoryY[i - 1];
        double ky1 = m_kTrajectoryY[i];
        double t0 = m_kTimeSec[i - 1];
        double t1 = m_kTimeSec[i];

        // Skip invalid values
        if (!std::isfinite(kx0) || !std::isfinite(kx1) || !std::isfinite(ky0) || !std::isfinite(ky1))
            continue;

        // Check if both kx and ky cross zero in this segment
        bool kxCrosses = (kx0 < 0 && kx1 > 0) || (kx0 > 0 && kx1 < 0) || (kx0 == 0.0) || (kx1 == 0.0);
        bool kyCrosses = (ky0 < 0 && ky1 > 0) || (ky0 > 0 && ky1 < 0) || (ky0 == 0.0) || (ky1 == 0.0);

        // Only proceed if both cross zero (or are near zero)
        if (!kxCrosses && !kyCrosses)
            continue;

        // Check if both are already near zero at the endpoints
        if (qAbs(kx0) <= kTolerance && qAbs(ky0) <= kTolerance)
        {
            candidates.append(t0);
            continue;
        }
        if (qAbs(kx1) <= kTolerance && qAbs(ky1) <= kTolerance)
        {
            candidates.append(t1);
            continue;
        }

        // If kx crosses zero, find the zero crossing time
        double tKxZero = -1.0;
        if (kxCrosses && (kx0 != kx1))
        {
            double alphaKx = -kx0 / (kx1 - kx0);
            if (alphaKx >= 0.0 && alphaKx <= 1.0)
            {
                tKxZero = t0 + alphaKx * (t1 - t0);
            }
        }

        // If ky crosses zero, find the zero crossing time
        double tKyZero = -1.0;
        if (kyCrosses && (ky0 != ky1))
        {
            double alphaKy = -ky0 / (ky1 - ky0);
            if (alphaKy >= 0.0 && alphaKy <= 1.0)
            {
                tKyZero = t0 + alphaKy * (t1 - t0);
            }
        }

        // Only accept if BOTH kx and ky cross zero (or are near zero)
        // This ensures we only mark true kx=ky=0 points
        if (tKxZero >= 0.0 && tKyZero >= 0.0)
        {
            double timeDiff = qAbs(tKxZero - tKyZero);
            double segmentDuration = qAbs(t1 - t0);
            
            // If crossings are very close (within 1% of segment duration), use the average
            if (timeDiff <= segmentDuration * 0.01)
            {
                double tZero = (tKxZero + tKyZero) * 0.5;
                // CRITICAL: Verify BOTH kx and ky are near zero at this time
                double alpha = (t1 > t0) ? (tZero - t0) / (t1 - t0) : 0.5;
                alpha = qBound(0.0, alpha, 1.0);
                double kxAtZero = kx0 + alpha * (kx1 - kx0);
                double kyAtZero = ky0 + alpha * (ky1 - ky0);
                
                // Both must be within tolerance - strict check
                if (qAbs(kxAtZero) <= kTolerance && qAbs(kyAtZero) <= kTolerance)
                {
                    candidates.append(tZero);
                }
            }
        }
        // If only kx crosses zero, check if ky is ALSO near zero at that exact crossing
        else if (tKxZero >= 0.0)
        {
            double alpha = (t1 > t0) ? (tKxZero - t0) / (t1 - t0) : 0.5;
            alpha = qBound(0.0, alpha, 1.0);
            double kyAtKxZero = ky0 + alpha * (ky1 - ky0);
            // CRITICAL: ky must be near zero, not just any value
            if (qAbs(kyAtKxZero) <= kTolerance)
            {
                candidates.append(tKxZero);
            }
        }
        // If only ky crosses zero, check if kx is ALSO near zero at that exact crossing
        else if (tKyZero >= 0.0)
        {
            double alpha = (t1 > t0) ? (tKyZero - t0) / (t1 - t0) : 0.5;
            alpha = qBound(0.0, alpha, 1.0);
            double kxAtKyZero = kx0 + alpha * (kx1 - kx0);
            // CRITICAL: kx must be near zero, not just any value
            if (qAbs(kxAtKyZero) <= kTolerance)
            {
                candidates.append(tKyZero);
            }
        }
    }

    // For trajectory zero crossings, filter to only keep those near ADC samples
    if (nAdc > 0)
    {
        QVector<double> adcTimesSorted = m_kTimeAdcSec;
        std::sort(adcTimesSorted.begin(), adcTimesSorted.end());

        // Tolerance for "near ADC": use half the minimum ADC interval or 50us
        double adcProximityTolerance = 50e-6; // default 50 microseconds
        if (nAdc >= 2)
        {
            double minInterval = std::numeric_limits<double>::max();
            for (int i = 1; i < nAdc; ++i)
            {
                double interval = adcTimesSorted[i] - adcTimesSorted[i - 1];
                if (interval > 0)
                    minInterval = qMin(minInterval, interval);
            }
            if (minInterval < std::numeric_limits<double>::max())
                adcProximityTolerance = qMin(minInterval * 0.3, 50e-6); // Use 30% of min interval, max 50us
        }

        // Filter trajectory zero crossings: only keep if near an ADC sample
        QVector<double> trajectoryCandidates;
        for (int i = 1; i < n; ++i)
        {
            double kx0 = m_kTrajectoryX[i - 1];
            double kx1 = m_kTrajectoryX[i];
            double ky0 = m_kTrajectoryY[i - 1];
            double ky1 = m_kTrajectoryY[i];
            double t0 = m_kTimeSec[i - 1];
            double t1 = m_kTimeSec[i];

            // Skip invalid values
            if (!std::isfinite(kx0) || !std::isfinite(kx1) || !std::isfinite(ky0) || !std::isfinite(ky1))
                continue;

            // Check if both kx and ky cross zero in this segment
            bool kxCrosses = (kx0 < 0 && kx1 > 0) || (kx0 > 0 && kx1 < 0) || (kx0 == 0.0) || (kx1 == 0.0);
            bool kyCrosses = (ky0 < 0 && ky1 > 0) || (ky0 > 0 && ky1 < 0) || (ky0 == 0.0) || (ky1 == 0.0);

            // Only proceed if both cross zero (or are near zero)
            if (!kxCrosses && !kyCrosses)
                continue;

            // Check if both are already near zero at the endpoints
            if (qAbs(kx0) <= kTolerance && qAbs(ky0) <= kTolerance)
            {
                trajectoryCandidates.append(t0);
                continue;
            }
            if (qAbs(kx1) <= kTolerance && qAbs(ky1) <= kTolerance)
            {
                trajectoryCandidates.append(t1);
                continue;
            }

            // If kx crosses zero, find the zero crossing time
            double tKxZero = -1.0;
            if (kxCrosses && (kx0 != kx1))
            {
                double alphaKx = -kx0 / (kx1 - kx0);
                if (alphaKx >= 0.0 && alphaKx <= 1.0)
                {
                    tKxZero = t0 + alphaKx * (t1 - t0);
                }
            }

            // If ky crosses zero, find the zero crossing time
            double tKyZero = -1.0;
            if (kyCrosses && (ky0 != ky1))
            {
                double alphaKy = -ky0 / (ky1 - ky0);
                if (alphaKy >= 0.0 && alphaKy <= 1.0)
                {
                    tKyZero = t0 + alphaKy * (t1 - t0);
                }
            }

            // Only accept if BOTH kx and ky cross zero (or are near zero)
            if (tKxZero >= 0.0 && tKyZero >= 0.0)
            {
                double timeDiff = qAbs(tKxZero - tKyZero);
                double segmentDuration = qAbs(t1 - t0);
                
                // If crossings are very close (within 1% of segment duration), use the average
                if (timeDiff <= segmentDuration * 0.01)
                {
                    double tZero = (tKxZero + tKyZero) * 0.5;
                    // Verify BOTH kx and ky are near zero at this time
                    double alpha = (t1 > t0) ? (tZero - t0) / (t1 - t0) : 0.5;
                    alpha = qBound(0.0, alpha, 1.0);
                    double kxAtZero = kx0 + alpha * (kx1 - kx0);
                    double kyAtZero = ky0 + alpha * (ky1 - ky0);
                    
                    if (qAbs(kxAtZero) <= kTolerance && qAbs(kyAtZero) <= kTolerance)
                    {
                        trajectoryCandidates.append(tZero);
                    }
                }
            }
            // If only kx crosses zero, check if ky is ALSO near zero at that exact crossing
            else if (tKxZero >= 0.0)
            {
                double alpha = (t1 > t0) ? (tKxZero - t0) / (t1 - t0) : 0.5;
                alpha = qBound(0.0, alpha, 1.0);
                double kyAtKxZero = ky0 + alpha * (ky1 - ky0);
                if (qAbs(kyAtKxZero) <= kTolerance)
                {
                    trajectoryCandidates.append(tKxZero);
                }
            }
            // If only ky crosses zero, check if kx is ALSO near zero at that exact crossing
            else if (tKyZero >= 0.0)
            {
                double alpha = (t1 > t0) ? (tKyZero - t0) / (t1 - t0) : 0.5;
                alpha = qBound(0.0, alpha, 1.0);
                double kxAtKyZero = kx0 + alpha * (kx1 - kx0);
                if (qAbs(kxAtKyZero) <= kTolerance)
                {
                    trajectoryCandidates.append(tKyZero);
                }
            }
        }

        // Filter trajectory candidates: only keep if near an ADC sample
        for (double tZeroSec : trajectoryCandidates)
        {
            auto it = std::lower_bound(adcTimesSorted.begin(), adcTimesSorted.end(), tZeroSec);
            
            double minDist = std::numeric_limits<double>::max();
            if (it != adcTimesSorted.end())
                minDist = qMin(minDist, qAbs(*it - tZeroSec));
            if (it != adcTimesSorted.begin())
                minDist = qMin(minDist, qAbs(*(it - 1) - tZeroSec));
            
            // Only keep if close to an ADC sample
            if (minDist <= adcProximityTolerance)
            {
                candidates.append(tZeroSec);
            }
        }
    }

    // Sort candidates and remove duplicates
    std::sort(candidates.begin(), candidates.end());
    double dupToleranceSec = 1e-6; // 1 microsecond
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [dupToleranceSec](double a, double b) {
        return qAbs(a - b) < dupToleranceSec;
    }), candidates.end());

    // Filter: only keep times that are within ADC blocks
    // Build list of ADC block time ranges (in seconds)
    QVector<QPair<double, double>> adcBlockRanges;
    if (!m_vecDecodeSeqBlocks.empty() && vecBlockEdges.size() >= 2)
    {
        for (int i = 0; i < static_cast<int>(m_vecDecodeSeqBlocks.size()); ++i)
        {
            SeqBlock* blk = m_vecDecodeSeqBlocks[i];
            if (!blk || !blk->isADC())
                continue;
            
            const ADCEvent& adc = blk->GetADCEvent();
            if (adc.numSamples <= 0 || adc.dwellTime <= 0)
                continue;
            
            // Convert block time range from internal units to seconds
            double blockStartInternal = vecBlockEdges[i];
            double blockEndInternal = vecBlockEdges[i + 1];
            double blockStartSec = blockStartInternal / (1e6 * getTFactor());
            double blockEndSec = blockEndInternal / (1e6 * getTFactor());
            
            adcBlockRanges.append(QPair<double, double>(blockStartSec, blockEndSec));
        }
    }

    // Filter candidates: only keep those within ADC blocks
    QVector<double> filteredCandidates;
    for (double tSec : candidates)
    {
        bool inAdcBlock = false;
        for (const auto& range : adcBlockRanges)
        {
            // Check if time is within this ADC block range
            if (tSec >= range.first && tSec <= range.second)
            {
                inAdcBlock = true;
                break;
            }
        }
        
        if (inAdcBlock)
        {
            filteredCandidates.append(tSec);
        }
    }

    // Convert to axis units
    for (double tSec : filteredCandidates)
    {
        double tAxis = tSec * 1e6 * getTFactor();
        result.append(tAxis);
    }

    // Final sort and dedup
    std::sort(result.begin(), result.end());
    double dupTolerance = 1e-6 * 1e6 * getTFactor();
    result.erase(std::unique(result.begin(), result.end(), [dupTolerance](double a, double b) {
        return qAbs(a - b) < dupTolerance;
    }), result.end());

    return result;
}

void PulseqLoader::updateTimeUnitFromSettings()
{
    Settings& settings = Settings::getInstance();
    switch (settings.getTimeUnit()) {
        case Settings::TimeUnit::Microseconds:
            TimeUnits = "us";
            tFactor = 1.0;
            break;
        case Settings::TimeUnit::Milliseconds:
        default:
            TimeUnits = "ms";
            tFactor = 1e-3;
            break;
    }
}

void PulseqLoader::rescaleTimeUnit()
{
    // Lightweight time-unit change: rescale cached time-dependent data in-place
    // instead of reloading the entire sequence file from disk.
    double oldFactor = tFactor;
    updateTimeUnitFromSettings();
    double newFactor = tFactor;

    if (oldFactor == newFactor) return; // no effective change
    if (vecBlockEdges.empty()) return;  // no file loaded

    double ratio = newFactor / oldFactor;

    // Rescale block edges
    for (auto& edge : vecBlockEdges)
        edge *= ratio;

    // Rescale pre-built ADC time series
    for (auto& t : m_adcTime)
        t *= ratio;

    // Rescale TE overlay data (excitation/refocusing centers are in axis units)
    m_teDurationAxis *= ratio;
    for (auto& t : m_teDurationsAxis)
        t *= ratio;
    for (auto& t : m_excitationCentersAxis)
        t *= ratio;
    for (auto& t : m_refocusingCentersAxis)
        t *= ratio;

    // Rescale waveform display
    WaveformDrawer* drawer = m_mainWindow->getWaveformDrawer();
    if (drawer)
    {
        // Rescale all time-dependent cached state (viewport ranges, debounce
        // cache, initial view bounds) in one encapsulated call.
        drawer->rescaleTimeCachedState(ratio);

        // Update the x-axis label text (e.g. "Time (ms)" -> "Time (us)")
        drawer->configureXAxisLabels();

        // Redraw all waveforms with the new time scale
        drawer->DrawRFWaveform();
        drawer->DrawADCWaveform();
        drawer->DrawGWaveform();
        if (drawer->getShowBlockEdges()) drawer->DrawBlockEdges();

        // Recompute and lock Y-axis ranges so they stay consistent
        drawer->computeAndLockYAxisRanges();
    }

    // Update trajectory if visible
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
        m_mainWindow->refreshTrajectoryPlotData();

    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
        m_mainWindow->ui->customPlot->replot();
}

void PulseqLoader::recomputePnsFromSettings()
{
    markPnsDirty();
    computeSafetyAnalysis(false);
    if (m_pnsState == PnsState::Calculating)
        return;
    startPnsComputationIfEnabled();
}

void PulseqLoader::saveLastOpenDirectory()
{
    QSettings settings;
    settings.setValue("LastOpenDirectory", m_sLastOpenDirectory);
}

void PulseqLoader::loadLastOpenDirectory()
{
    QSettings settings;
    m_sLastOpenDirectory = settings.value("LastOpenDirectory", "").toString();
    
    // Validate that the directory still exists
    if (!m_sLastOpenDirectory.isEmpty() && !QDir(m_sLastOpenDirectory).exists()) {
        m_sLastOpenDirectory.clear();
    }
}
// --- RF shape cache helpers ---
QString PulseqLoader::rfAmpKey(int magShapeId, int timeShapeId, int len) const
{
    return QString("rfA:%1:%2#%3").arg(magShapeId).arg(timeShapeId).arg(len);
}

QString PulseqLoader::rfPhKey(int phaseShapeId, int timeShapeId, int len) const
{
    return QString("rfP:%1:%2#%3").arg(phaseShapeId).arg(timeShapeId).arg(len);
}

const PulseqLoader::RFAmpEntry& PulseqLoader::ensureRfAmpCached(const float* amp, int len,
                                                               int magShapeId, int timeShapeId)
{
    QString key = rfAmpKey(magShapeId, timeShapeId, len);
    auto it = m_rfAmpCache.find(key);
    if (it != m_rfAmpCache.end()) return it.value();
    RFAmpEntry e; e.length = len; e.ampNorm.resize(len);
    double mnA = std::numeric_limits<double>::infinity();
    double mxA = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < len; ++i) {
        float a = amp ? amp[i] : std::numeric_limits<float>::quiet_NaN();
        e.ampNorm[i] = a;
        if (!std::isnan(a)) { if (a < mnA) mnA = a; if (a > mxA) mxA = a; }
    }
    if (!std::isfinite(mnA) || !std::isfinite(mxA)) { mnA = 0.0; mxA = 0.0; }
    e.ampMin = mnA; e.ampMax = mxA;
    // cache peak index by absolute value for later ultra-low-pixel path
    if (len > 0) {
        auto it = std::max_element(e.ampNorm.begin(), e.ampNorm.end(),
                                   [](float a, float b){ return std::fabs(a) < std::fabs(b); });
        e.peakIndex = (it != e.ampNorm.end() ? int(std::distance(e.ampNorm.begin(), it)) : 0);
    } else {
        e.peakIndex = -1;
    }
    auto ins = m_rfAmpCache.insert(key, e);
    return ins.value();
}

const PulseqLoader::RFPhEntry& PulseqLoader::ensureRfPhCached(const float* phase, int len,
                                                             int phaseShapeId, int timeShapeId)
{
    QString key = rfPhKey(phaseShapeId, timeShapeId, len);
    auto it = m_rfPhCache.find(key);
    if (it != m_rfPhCache.end()) return it.value();
    RFPhEntry e; e.length = len; e.phNorm.resize(len);
    double mnP = std::numeric_limits<double>::infinity();
    double mxP = -std::numeric_limits<double>::infinity();
    bool isReal = true;
    for (int i = 0; i < len; ++i) {
        float p = phase ? phase[i] : std::numeric_limits<float>::quiet_NaN();
        e.phNorm[i] = p;
        if (!std::isnan(p)) { 
            if (p < mnP) mnP = p; if (p > mxP) mxP = p; 
            // Check if logic shape is "Real" (only 0 or pi phases, ignoring small numerical noise)
            // Relaxed threshold to 1e-2 (approx 0.5 deg) to match render logic
            if (std::abs(std::sin(p)) > 1e-2) isReal = false;
        }
    }
    if (!std::isfinite(mnP) || !std::isfinite(mxP)) { mnP = 0.0; mxP = 0.0; }
    e.phMin = mnP; e.phMax = mxP;
    e.isRealLike = isReal;
    auto ins = m_rfPhCache.insert(key, e);
    return ins.value();
}

QString PulseqLoader::gradKey(int waveShapeId, int timeShapeId, int len) const
{
    return QString("grad:%1:%2#%3").arg(waveShapeId).arg(timeShapeId).arg(len);
}

const PulseqLoader::GradShapeEntry& PulseqLoader::ensureGradCached(const float* shape, int len,
                                                                  int waveShapeId, int timeShapeId)
{
    QString key = gradKey(waveShapeId, timeShapeId, len);
    auto it = m_gradShapeCache.find(key);
    if (it != m_gradShapeCache.end()) return it.value();
    GradShapeEntry e; e.length = len; e.norm.resize(len);
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < len; ++i) {
        float v = shape[i]; e.norm[i] = v;
        if (!std::isnan(v)) { if (v < mn) mn = v; if (v > mx) mx = v; }
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = 0.0; mx = 0.0; }
    e.vMin = mn; e.vMax = mx;
    auto ins = m_gradShapeCache.insert(key, e);
    return ins.value();
}

void PulseqLoader::getGradViewportDecimated(int channel, double visibleStart, double visibleEnd, int pixelWidth,
                                            QVector<double>& tOut, QVector<double>& vOut)
{
    tOut.clear(); vOut.clear();
    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.isEmpty() || pixelWidth <= 0) return;

    // Find visible block range
    int startBlock = 0;
    int endBlock = int(vecBlockEdges.size()) - 2;
    for (int i = 0; i < vecBlockEdges.size() - 1; ++i) {
        if (vecBlockEdges[i + 1] > visibleStart) { startBlock = i; break; }
    }
    for (int i = int(vecBlockEdges.size()) - 2; i >= startBlock; --i) {
        if (vecBlockEdges[i] < visibleEnd) { endBlock = i; break; }
    }
    if (startBlock > endBlock) return;

    const double window = std::max(1e-9, visibleEnd - visibleStart);

    bool haveLast = false; double lastT=0.0, lastV=0.0;
    // Global decimation gating for gradients (heavy-only)
    const int DECIMATE_TOTAL_THRESHOLD_GRAD = 150000;
    long long totalGradSamples = 0;
    for (int i = startBlock; i <= endBlock; ++i) {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i]; if (!blk) continue;
        if (blk->isArbitraryGradient(channel)) totalGradSamples += std::max(0, blk->GetArbGradNumSamples(channel));
        else if (blk->isExtTrapGradient(channel)) totalGradSamples += (int)blk->GetExtTrapGradTimes(channel).size();
        else if (blk->isTrapGradient(channel)) totalGradSamples += 4;
    }
    bool allowDecimateGrad = (totalGradSamples > DECIMATE_TOTAL_THRESHOLD_GRAD);
    if (pixelWidth > 0) {
        double pppTotal = double(std::max<long long>(1, totalGradSamples)) / double(pixelWidth);
        if (pppTotal <= 2.0) allowDecimateGrad = false;
    }

    for (int i = startBlock; i <= endBlock; ++i) {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i]; if (!blk) continue;
        bool hasGradient = blk->isTrapGradient(channel) || blk->isArbitraryGradient(channel) || blk->isExtTrapGradient(channel);
        if (!hasGradient) continue;
        const GradEvent& grad = blk->GetGradEvent(channel);
        const double tStart = vecBlockEdges[i] + grad.delay * tFactor;

        if (blk->isTrapGradient(channel)) {
            double rampUpTime = grad.rampUpTime * tFactor;
            double flatTime = grad.flatTime * tFactor;
            double rampDownTime = grad.rampDownTime * tFactor;
            double t0 = tStart;
            double t1 = tStart + rampUpTime;
            double t2 = t1 + flatTime;
            double t3 = t2 + rampDownTime;
            if (t3 <= visibleStart || t0 >= visibleEnd) continue;
            // Build block arrays
            QVector<double> tt{t0,t1,t2,t3};
            QVector<double> vv{0.0, grad.amplitude, grad.amplitude, 0.0};
            // Continuity at block start
            if (!tt.isEmpty()) {
                if (haveLast) {
                    double dtTol = 1e-9;
                    double dvTol = 1e-12;
                    bool continuous = (std::abs(tt.first() - lastT) <= dtTol) && (std::abs(vv.first() - lastV) <= dvTol);
                    if (!continuous) {
                        // Keep x monotonic: duplicate last x as a NaN break marker.
                        // This avoids generating a break time that is > lastT when the next segment starts at the same timestamp.
                        tOut.append(lastT);
                        vOut.append(std::numeric_limits<double>::quiet_NaN());
                    }
                }
                tOut += tt; vOut += vv;
                lastT = tt.last(); lastV = vv.last(); haveLast = true;
            }
            continue;
        }

        if (blk->isArbitraryGradient(channel)) {
            int numSamples = blk->GetArbGradNumSamples(channel);
            const float* shapePtr = blk->GetArbGradShapePtr(channel);
            if (numSamples <= 0 || !shapePtr) continue;
            const GradShapeEntry& entry = ensureGradCached(shapePtr, numSamples, grad.waveShape, grad.timeShape);
            // Use sequence GradientRasterTime (seconds) — required by loader
            if (!m_spPulseqSeq) return; // defensive: loader guarantees presence
            std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
            if (def.empty() || !std::isfinite(def[0]) || def[0] <= 0.0) return; // do not render without definition
            double gradRaster_us = def[0] * 1e6;
            double dt = gradRaster_us * tFactor;
            const bool oversampled = blk->isArbGradWithOversampling(channel) || (grad.timeShape == -1);
            const double duration = oversampled
                ? (static_cast<double>(numSamples) + 1.0) * 0.5 * dt
                : static_cast<double>(numSamples) * dt;
            if (tStart >= visibleEnd || (tStart + duration) <= visibleStart) continue;
            int pxForBlock = std::max(1, int(std::round(duration / window * pixelWidth)));
            // Prefer LTTB decimation for shape fidelity
            QVector<double> tBlk, vBlk;
            double ppp = (pxForBlock > 0) ? double(numSamples) / double(pxForBlock) : double(numSamples);
            if (!allowDecimateGrad || numSamples <= 64 || ppp <= 1.2) {
                tBlk.reserve(numSamples); vBlk.reserve(numSamples);
                for (int j = 0; j < numSamples; ++j) {
                    // Match Pulseq semantics:
                    // - center-raster arbitrary: sample j at (j+0.5)*dt
                    // - oversampled arbitrary:   sample j at (j+1.0)*0.5*dt
                    const double tj = oversampled
                        ? (static_cast<double>(j) + 1.0) * 0.5 * dt
                        : (static_cast<double>(j) + 0.5) * dt;
                    tBlk.append(tStart + tj);
                    vBlk.append(double(entry.norm[j]) * double(grad.amplitude));
                }
            } else {
                int target = std::min(numSamples, std::min(10000, int(std::round(pxForBlock*3.0))));
                if (target <= 4 || pxForBlock <= 2) {
                    // Extremely narrow: take a few evenly spaced samples to avoid sawtooth artifacts
                    QSet<int> idxs;
                    idxs.insert(0);
                    idxs.insert(std::max(0, std::min(numSamples-1, (int)std::floor(0.25*(numSamples-1)))));
                    idxs.insert(std::max(0, std::min(numSamples-1, (int)std::floor(0.5*(numSamples-1)))));
                    idxs.insert(std::max(0, std::min(numSamples-1, (int)std::floor(0.75*(numSamples-1)))));
                    idxs.insert(numSamples-1);
                    QList<int> sorted = QList<int>(idxs.constBegin(), idxs.constEnd());
                    std::sort(sorted.begin(), sorted.end());
                    tBlk.reserve(sorted.size()); vBlk.reserve(sorted.size());
                    for (int k : sorted) {
                        const double tk = oversampled
                            ? (static_cast<double>(k) + 1.0) * 0.5 * dt
                            : (static_cast<double>(k) + 0.5) * dt;
                        tBlk.append(tStart + tk);
                        vBlk.append(double(entry.norm[k]) * double(grad.amplitude));
                    }
                } else {
                    const double tFirst = tStart + 0.5 * dt;
                    const double dtEff = oversampled ? (0.5 * dt) : dt;
                    QVector<double> dT, dV; lttbDownsampleUniform(entry.norm, tFirst, dtEff, target, dT, dV);
                    tBlk = dT; vBlk.reserve(dV.size()); for (double val: dV){ vBlk.append(val * double(grad.amplitude)); }
                }
            }
            if (!tBlk.isEmpty()) {
                if (haveLast) {
                    double dtTol = 1e-9; double dvTol = 1e-12;
                    bool continuous = (std::abs(tBlk.first() - lastT) <= dtTol) && (std::abs(vBlk.first() - lastV) <= dvTol);
                    if (!continuous) {
                        // Duplicate last x for NaN break to keep x monotonic.
                        tOut.append(lastT);
                        vOut.append(std::numeric_limits<double>::quiet_NaN());
                    }
                }
                tOut += tBlk; vOut += vBlk; lastT = tBlk.last(); lastV = vBlk.last(); haveLast = true;
            }
            continue;
        }

        if (blk->isExtTrapGradient(channel)) {
            const std::vector<long>& times = blk->GetExtTrapGradTimes(channel);
            const std::vector<float>& shape = blk->GetExtTrapGradShape(channel);
            if (times.empty() || shape.empty() || times.size() != shape.size()) continue;
            // Build and decimate via buckets in time domain
            // We’ll do a simple min-max on the resampled sequence by index mapping similar to arbitrary
            int n = int(times.size());
            // Estimate dt average for epsilon
            double dtAvg = (n>1 ? (times.back()-times.front())/(double)(n-1) * tFactor : 1e-6);
            QVector<double> tBlk; QVector<double> vBlk; tBlk.reserve(n); vBlk.reserve(n);
            for (int j = 0; j < n; ++j) {
                double t = tStart + times[j] * tFactor;
                tBlk.append(t); vBlk.append(double(shape[j]) * double(grad.amplitude));
            }
            if (!tBlk.isEmpty()) {
                if (haveLast) {
                    double dtTol = 1e-9; double dvTol = 1e-12;
                    bool continuous = (std::abs(tBlk.first() - lastT) <= dtTol) && (std::abs(vBlk.first() - lastV) <= dvTol);
                    if (!continuous) {
                        // Duplicate last x for NaN break to keep x monotonic.
                        tOut.append(lastT);
                        vOut.append(std::numeric_limits<double>::quiet_NaN());
                    }
                }
                tOut += tBlk; vOut += vBlk; lastT = tBlk.last(); lastV = vBlk.last(); haveLast = true;
            }
            continue;
        }
    }
}

QPair<double,double> PulseqLoader::getGradGlobalRange(int channel)
{
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    // 1) Arbitrary shapes via per-shape aggregates
    const auto& agg = m_gradAgg[channel];
    for (auto it = agg.constBegin(); it != agg.constEnd(); ++it) {
        const ScaleAgg& ag = it.value();
        if (!ag.hasShape) continue;
        double candidates[4] = {
            ag.shapeMin * ag.maxPosScale,
            ag.shapeMax * ag.maxPosScale,
            ag.shapeMin * ag.minNegScale,
            ag.shapeMax * ag.minNegScale
        };
        for (double c : candidates) { if (std::isfinite(c)) { if (c < mn) mn = c; if (c > mx) mx = c; } }
    }
    // 2) Trapezoids: extremes at 0 and amplitude
    mn = std::min(mn, std::min(0.0, m_gradTrapMinNegScale[channel]));
    mx = std::max(mx, std::max(0.0, m_gradTrapMaxPosScale[channel]));
    // 3) External trapezoid aggregated min/max
    mn = std::min(mn, m_gradExtTrapGlobalMin[channel]);
    mx = std::max(mx, m_gradExtTrapGlobalMax[channel]);

    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = -1.0; mx = 1.0; }
    double pad = (mx - mn) * 0.05; if (pad == 0) pad = 0.1;
    return qMakePair(mn - pad, mx + pad);
}

bool PulseqLoader::sampleRFAtTime(double time, int blockIdx, double& ampHzOut, double& phaseRadOut) const
{
    ampHzOut = 0.0; phaseRadOut = 0.0;
    if (blockIdx < 0 || blockIdx + 1 >= vecBlockEdges.size()) return false;
    if (blockIdx >= static_cast<int>(m_vecDecodeSeqBlocks.size())) return false;
    SeqBlock* blk = m_vecDecodeSeqBlocks[blockIdx];
    if (!blk || !blk->isRF()) return false;

    const RFEvent& rf = blk->GetRFEvent();
    int RFLength = blk->GetRFLength();
    if (RFLength <= 0) return false;
    float dwell = blk->GetRFDwellTime(); // us
    const float* rfList = blk->GetRFAmplitudePtr();
    const float* phaseList = blk->GetRFPhasePtr();
    double tStart = vecBlockEdges[blockIdx] + rf.delay * tFactor;
    double dt = dwell * tFactor;

    // Outside block window
    if (time < tStart || time > tStart + (RFLength - 1) * dt) return false;

    // Use cache to get isRealLike property (requires mutable access? No, ensureRfPhCached is non-const but we are const)
    // Problem: sampleRFAtTime is const, ensureRfPhCached is not. 
    // However, cached entry should exist if rendered. If not, we can't update cache.
    // Solution: Look up in cache directly. If missing, default to safe assumption (not real-like) or re-scan.
    // For status bar (mouse hover), it's likely already rendered.
    QString key = rfPhKey(rf.phaseShape, rf.timeShape, RFLength);
    bool isRealLike = false; // Default safe
    // We need access to m_rfPhCache. It is mutable? No.
    // We can cast away constness if we really need to update cache, but cleaner to check if exists.
    auto it = m_rfPhCache.find(key);
    if (it != m_rfPhCache.end()) {
        isRealLike = it.value().isRealLike;
    } else {
        // If not cached, do quick scan? Or just assume complex?
        // Assuming complex means we show raw phase. If real pulse has pi phase, it shows 3.14.
        // User complained about "uncoordinated". 
        // We can do a quick scan here.
        bool isReal = true;
        for (int k=0; k<RFLength; ++k) {
             float p = phaseList[k];
             if (!std::isnan(p) && std::abs(std::sin(p)) > 1e-2) { isReal=false; break; }
        }
        isRealLike = isReal;
    }

    // Compute local index and interpolate linearly for maximum fidelity
    double u = (time - tStart) / dt;
    int i0 = static_cast<int>(std::floor(u));
    int i1 = std::min(RFLength - 1, i0 + 1);
    double alpha = u - i0;
    if (i0 < 0) { i0 = 0; alpha = 0.0; }

    auto amp0 = static_cast<double>(rfList[i0]) * static_cast<double>(rf.amplitude);
    auto ph0  = static_cast<double>(phaseList[i0]);
    
    auto amp1 = static_cast<double>(rfList[i1]) * static_cast<double>(rf.amplitude);
    auto ph1  = static_cast<double>(phaseList[i1]);
    
    // Interpolate Amplitude
    ampHzOut = amp0 + (amp1 - amp0) * alpha;

    // Phase Calculation matching getRfViewportDecimated
    // Base phase: 0 for real-like, otherwise interpolated raw phase
    double basePh0 = isRealLike ? 0.0 : ph0;
    double basePh1 = isRealLike ? 0.0 : ph1;
    // Linear interp of base phase (for complex, wrap-around interp is ideal but linear is standard here)
    double basePh = basePh0 + (basePh1 - basePh0) * alpha;

    // Full Offsets
    double gamma = Settings::getInstance().getGamma();
    double fullFreqOff = rf.freqOffset + rf.freqPPM * 1e-6 * gamma * m_b0Tesla;
    double fullPhaseOff = rf.phaseOffset + rf.phasePPM * 1e-6 * gamma * m_b0Tesla;
    
    // Time in seconds from pulse start
    double t_local_sec = ((time - tStart) / tFactor) * 1e-6;
    
    double totalPhase = basePh + fullPhaseOff + 2.0 * M_PI * t_local_sec * fullFreqOff;
    
    // Use atan2 to wrap to [-pi, pi]
    phaseRadOut = std::atan2(std::sin(totalPhase), std::cos(totalPhase));
    
    return true;
}

bool PulseqLoader::sampleGradAtTime(int channel, double time, int blockIdx, double& gradOutHzPerM) const
{
    gradOutHzPerM = 0.0;
    if (blockIdx < 0 || blockIdx + 1 >= vecBlockEdges.size()) return false;
    if (blockIdx >= static_cast<int>(m_vecDecodeSeqBlocks.size())) return false;
    SeqBlock* blk = m_vecDecodeSeqBlocks[blockIdx];
    if (!blk) return false;
    bool hasGradient = blk->isTrapGradient(channel) || blk->isArbitraryGradient(channel) || blk->isExtTrapGradient(channel);
    if (!hasGradient) return false;

    const GradEvent& grad = blk->GetGradEvent(channel);
    double tStart = vecBlockEdges[blockIdx] + grad.delay * tFactor;

    // Trapezoid
    if (blk->isTrapGradient(channel)) {
        double ru = grad.rampUpTime * tFactor;
        double fl = grad.flatTime * tFactor;
        double rd = grad.rampDownTime * tFactor;
        double t0 = tStart;
        double t1 = t0 + ru;
        double t2 = t1 + fl;
        double t3 = t2 + rd;
        if (time < t0 || time > t3) return false;
        if (time <= t1) {
            double a = (ru > 0.0 ? (time - t0) / ru : 0.0);
            gradOutHzPerM = grad.amplitude * a;
            return true;
        } else if (time <= t2) {
            gradOutHzPerM = grad.amplitude;
            return true;
        } else {
            double a = (rd > 0.0 ? (t3 - time) / rd : 0.0);
            gradOutHzPerM = grad.amplitude * a;
            return true;
        }
    }

    // Arbitrary
    if (blk->isArbitraryGradient(channel)) {
        int n = blk->GetArbGradNumSamples(channel);
        const float* shape = blk->GetArbGradShapePtr(channel);
        if (n <= 0 || !shape) return false;
        // Use sequence GradientRasterTime (seconds) — required by loader
        if (!m_spPulseqSeq) return false; // defensive
        std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        if (def.empty() || !std::isfinite(def[0]) || def[0] <= 0.0) return false;
        double gradRaster_us = def[0] * 1e6; // seconds -> microseconds
        double dt = gradRaster_us * tFactor;
        const bool oversampled = blk->isArbGradWithOversampling(channel) || (grad.timeShape == -1);
        const double tFirst = tStart + 0.5 * dt;
        const double dtEff = oversampled ? (0.5 * dt) : dt;
        const double tEnd = tFirst + (n - 1) * dtEff;
        if (time < tFirst || time > tEnd) return false;
        double u = (time - tFirst) / dtEff;
        int i0 = static_cast<int>(std::floor(u));
        int i1 = std::min(n - 1, i0 + 1);
        double alpha = u - i0;
        if (i0 < 0) { i0 = 0; alpha = 0.0; }
        double v0 = static_cast<double>(shape[i0]) * static_cast<double>(grad.amplitude);
        if (i1 == i0) { gradOutHzPerM = v0; return true; }
        double v1 = static_cast<double>(shape[i1]) * static_cast<double>(grad.amplitude);
        gradOutHzPerM = v0 + (v1 - v0) * alpha;
        return true;
    }

    // External trapezoid (piecewise linear defined by times/shape)
    if (blk->isExtTrapGradient(channel)) {
        const std::vector<long>& times = blk->GetExtTrapGradTimes(channel);
        const std::vector<float>& shape = blk->GetExtTrapGradShape(channel);
        int n = static_cast<int>(times.size());
        if (n <= 0 || shape.size() != times.size()) return false;
        double tFirst = tStart + times.front() * tFactor;
        double tLast  = tStart + times.back()  * tFactor;
        if (time < tFirst || time > tLast) return false;
        // Find segment
        int i0 = 0;
        for (int i = 0; i < n - 1; ++i) {
            double a = tStart + times[i] * tFactor;
            double b = tStart + times[i+1] * tFactor;
            if (time >= a && time <= b) { i0 = i; break; }
        }
        double ta = tStart + times[i0] * tFactor;
        double tb = tStart + times[i0+1] * tFactor;
        double va = static_cast<double>(shape[i0]) * static_cast<double>(grad.amplitude);
        double vb = static_cast<double>(shape[i0+1]) * static_cast<double>(grad.amplitude);
        if (tb <= ta) { gradOutHzPerM = va; return true; }
        double alpha = (time - ta) / (tb - ta);
        gradOutHzPerM = va + (vb - va) * alpha;
        return true;
    }

    return false;
}

void PulseqLoader::downsampleMinMax(const QVector<float>& src, int buckets, QVector<int>& outIdxMin, QVector<int>& outIdxMax) const
{
    outIdxMin.clear(); outIdxMax.clear();
    int n = src.size();
    if (n == 0 || buckets <= 0) return;
    if (n <= 2 * buckets) {
        outIdxMin.reserve(n); outIdxMax.reserve(n);
        for (int i = 0; i < n; ++i) { outIdxMin.append(i); outIdxMax.append(i); }
        return;
    }
    double bw = double(n) / double(buckets);
    int idx = 0;
    for (int b = 0; b < buckets; ++b) {
        double start = b * bw;
        double end = (b + 1 == buckets) ? n : (b + 1) * bw;
        int i0 = int(std::floor(start));
        int i1 = int(std::floor(end));
        if (i0 >= n) i0 = n - 1;
        if (i1 <= i0) i1 = std::min(n, i0 + 1);
        float mn = std::numeric_limits<float>::infinity(); int iMin = i0;
        float mx = -std::numeric_limits<float>::infinity(); int iMax = i0;
        for (int i = i0; i < i1; ++i) {
            float v = src[i];
            if (std::isnan(v)) continue;
            if (v < mn) { mn = v; iMin = i; }
            if (v > mx) { mx = v; iMax = i; }
        }
        outIdxMin.append(iMin);
        outIdxMax.append(iMax);
        idx = i1;
    }
}

void PulseqLoader::lttbDownsampleUniform(const QVector<float>& src, double tStart, double dt, int targetPoints,
                               QVector<double>& tOut, QVector<double>& vOut) const
{
    tOut.clear(); vOut.clear();
    int n = src.size();
    if (n <= 0 || targetPoints <= 0) return;
    if (n <= targetPoints) {
        tOut.reserve(n); vOut.reserve(n);
        for (int i=0;i<n;++i){ tOut.append(tStart + i*dt); vOut.append(double(src[i])); }
        return;
    }
    tOut.reserve(targetPoints); vOut.reserve(targetPoints);
    // Always include first point
    tOut.append(tStart); vOut.append(double(src[0]));
    if (targetPoints == 1) return;
    if (targetPoints == 2) {
        tOut.append(tStart + (n-1)*dt); vOut.append(double(src[n-1]));
        return;
    }
    int buckets = targetPoints - 2;
    int bucketSize = (n - 2) / (buckets == 0 ? 1 : buckets);
    if (bucketSize <= 0) bucketSize = 1;
    int a = 0; // prev chosen index
    for (int b = 0; b < buckets; ++b) {
        int start = 1 + b*bucketSize;
        int end = (b==buckets-1 ? n-1 : std::min(n-1, start + bucketSize));
        // Next bucket avg index
        int nextStart = end;
        int nextEnd = (b==buckets-1 ? n-1 : std::min(n-1, end + bucketSize));
        double avgX = 0.0, avgY = 0.0; int count = 0;
        for (int i = nextStart; i < nextEnd; ++i) { avgX += (tStart + i*dt); avgY += double(src[i]); ++count; }
        if (count == 0) { avgX = tStart + (nextStart)*dt; avgY = double(src[std::min(nextStart, n-1)]); }
        double maxArea = -1.0; int maxIndex = start;
        double ax = tStart + a*dt; double ay = double(src[a]);
        for (int i = start; i < end; ++i) {
            double bx = tStart + i*dt; double by = double(src[i]);
            double cx = avgX; double cy = avgY;
            double area = std::abs((ax - cx)*(by - ay) - (ax - bx)*(cy - ay));
            if (area > maxArea) { maxArea = area; maxIndex = i; }
        }
        tOut.append(tStart + maxIndex*dt);
        vOut.append(double(src[maxIndex]));
        a = maxIndex;
    }
    // include last
    tOut.append(tStart + (n-1)*dt);
    vOut.append(double(src[n-1]));
}

void PulseqLoader::getRfViewportDecimated(double visibleStart, double visibleEnd, int pixelWidth,
                                          QVector<double>& tAmp, QVector<double>& vAmp,
                                          QVector<double>& tPh, QVector<double>& vPh)
{
    UnifiedRfViewport viewport;
    getUnifiedRfViewport(visibleStart, visibleEnd, pixelWidth, viewport);
    tAmp.clear(); vAmp.clear(); tPh.clear(); vPh.clear();
    if (!viewport.ampTimeByChannel.isEmpty()) {
        tAmp = viewport.ampTimeByChannel.first();
        vAmp = viewport.ampValueByChannel.first();
    }
    if (!viewport.phaseTimeByChannel.isEmpty()) {
        tPh = viewport.phaseTimeByChannel.first();
        vPh = viewport.phaseValueByChannel.first();
    }
}

QString PulseqLoader::rfSourceTypeToString(RfSourceType type) const
{
    switch (type) {
    case RfSourceType::SingleChannel: return QStringLiteral("SingleChannel");
    case RfSourceType::RfShim: return QStringLiteral("RfShim");
    case RfSourceType::RoosPtxHack: return QStringLiteral("RoosPtxHack");
    }
    return QStringLiteral("SingleChannel");
}

PulseqLoader::RoosPtxDetectionResult PulseqLoader::detectRoosPtxHackPattern() const
{
    RoosPtxDetectionResult result;
    if (m_vecDecodeSeqBlocks.empty()) {
        return result;
    }

    QHash<int, int> resetCountHits;
    QHash<int, int> resetCountSegmentLen;
    int splitCapableRfBlocks = 0;
    for (SeqBlock* blk : m_vecDecodeSeqBlocks) {
        if (!blk || !blk->isRF() || blk->hasRfShim()) {
            continue;
        }

        const RFEvent& rf = blk->GetRFEvent();
        std::vector<float> rawAmp;
        std::vector<float> rawPhase;
        if (!getRoosRawRfShapes(m_spPulseqSeq.get(), rf, rawAmp, rawPhase)) {
            continue;
        }
        const int rfLength = int(rawAmp.size());
        const QVector<int> boundaries = detectRoosTimeShapeBoundaries(m_spPulseqSeq.get(), rf.timeShape, rfLength);
        int segmentLen = 0;
        if (!areUniformRoosSegments(boundaries, &segmentLen)) {
            continue;
        }

        const int channelCount = boundaries.size() - 1;
        if (channelCount <= 1) {
            continue;
        }
        resetCountHits[channelCount] += 1;
        resetCountSegmentLen[channelCount] = segmentLen;
        ++splitCapableRfBlocks;
    }

    int bestCount = 1;
    int bestHits = 0;
    for (auto it = resetCountHits.cbegin(); it != resetCountHits.cend(); ++it) {
        const int channelCount = it.key();
        const int hitCount = it.value();
        if (hitCount > bestHits || (hitCount == bestHits && channelCount > bestCount)) {
            bestHits = hitCount;
            bestCount = channelCount;
        }
    }

    if (bestCount > 1) {
        result.inferredChannelCount = bestCount;
        result.inferredSamplesPerChannel = resetCountSegmentLen.value(bestCount, 0);
    }

    result.matchedRfGroupCount = splitCapableRfBlocks;
    result.matchedAdcGroupCount = 0;
    result.uniqueMatchedPhaseCount = 0;
    result.matchedBlockPairs = 0;
    result.detected = splitCapableRfBlocks > 0
        && result.inferredChannelCount > 1
        && result.inferredSamplesPerChannel > 0;

    if (result.detected) {
        LogManager::getInstance().appendStructured(
            QtInfoMsg,
            QStringLiteral("PulseqLoader"),
            QStringLiteral("RoosPtxHack detected from time-shape resets: splitCapableRfBlocks=%1 inferredChannels=%2 samplesPerChannel=%3")
                .arg(result.matchedRfGroupCount)
                .arg(result.inferredChannelCount)
                .arg(result.inferredSamplesPerChannel));
    } else {
        LogManager::getInstance().appendStructured(
            QtInfoMsg,
            QStringLiteral("PulseqLoader"),
            QStringLiteral("RoosPtxHack not detected from time-shape resets: splitCapableRfBlocks=%1 inferredChannels=%2 samplesPerChannel=%3")
                .arg(result.matchedRfGroupCount)
                .arg(result.inferredChannelCount)
                .arg(result.inferredSamplesPerChannel));
    }
    return result;
}

bool PulseqLoader::buildUnifiedRfBlocks(QString* errorMessage)
{
    m_unifiedRfBlocks.clear();
    m_unifiedRfChannelCount = 1;
    m_detectedRoosPtxHack = false;
    m_unifiedRfStatusMessage.clear();
    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.isEmpty()) {
        return true;
    }

    const bool enableRoosAutoDetection = Settings::getInstance().getEnableRoosPtxHackAutoDetection();
    const RoosPtxDetectionResult roosDetection = detectRoosPtxHackPattern();
    bool hasExplicitRfShim = false;
    for (int i = 0; i < static_cast<int>(m_vecDecodeSeqBlocks.size()); ++i) {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (!blk || !blk->isRF()) {
            continue;
        }

        RFEvent& rf = blk->GetRFEvent();
        const int rfLength = blk->GetRFLength();
        if (rfLength <= 0) {
            continue;
        }

        UnifiedRfBlock unifiedBlock;
        unifiedBlock.blockIndex = i;
        unifiedBlock.rfLength = rfLength;
        unifiedBlock.startTimeAxis = vecBlockEdges[i] + rf.delay * tFactor;
        unifiedBlock.dwellAxis = blk->GetRFDwellTime() * tFactor;

        float* rfList = blk->GetRFAmplitudePtr();
        float* phaseList = blk->GetRFPhasePtr();
        const RFAmpEntry& ampEntry = ensureRfAmpCached(rfList, rfLength, rf.magShape, rf.timeShape);
        const RFPhEntry& phEntry = ensureRfPhCached(phaseList, rfLength, rf.phaseShape, rf.timeShape);

        auto makeChannel = [&](int channelIndex,
                               RfSourceType source,
                               double amplitudeScale,
                               double phaseOffsetRad) {
            UnifiedRfChannel channel;
            channel.channelIndex = channelIndex;
            channel.source = source;
            channel.amplitudeScale = amplitudeScale;
            channel.phaseOffsetRad = phaseOffsetRad;
            channel.freqOffsetHz = rf.freqOffset + rf.freqPPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla;
            channel.phaseIsRealLike = phEntry.isRealLike;
            channel.ampNorm = ampEntry.ampNorm;
            channel.phaseNorm = phEntry.phNorm;
            unifiedBlock.channels.append(channel);
        };

        if (blk->hasRfShim()) {
            hasExplicitRfShim = true;
            auto& rfShim = blk->GetRfShim();
            for (int channelIndex = 0; channelIndex < rfShim.nchan; ++channelIndex) {
                const double scale = double(rf.amplitude) * double(rfShim.amplitudes[channelIndex]);
                const double phaseOffset = rf.phaseOffset
                                         + rf.phasePPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla
                                         + double(rfShim.phases[channelIndex]);
                makeChannel(channelIndex, RfSourceType::RfShim, scale, phaseOffset);
            }
        } else if (enableRoosAutoDetection && roosDetection.detected) {
            std::vector<float> rawAmp;
            std::vector<float> rawPhase;
            const bool hasRawShapes = getRoosRawRfShapes(m_spPulseqSeq.get(), rf, rawAmp, rawPhase);
            const QVector<int> boundaries = hasRawShapes
                ? detectRoosTimeShapeBoundaries(m_spPulseqSeq.get(), rf.timeShape, int(rawAmp.size()))
                : QVector<int>();
            int samplesPerChannel = 0;
            if (hasRawShapes
                && areUniformRoosSegments(boundaries, &samplesPerChannel)
                && (boundaries.size() - 1) == roosDetection.inferredChannelCount) {
                const RFAmpEntry& rawAmpEntry = ensureRfAmpCached(rawAmp.data(), int(rawAmp.size()), rf.magShape, rf.timeShape);
                const RFPhEntry& rawPhEntry = ensureRfPhCached(rawPhase.data(), int(rawPhase.size()), rf.phaseShape, rf.timeShape);
                unifiedBlock.rfLength = samplesPerChannel;

                for (int channelIndex = 0; channelIndex + 1 < boundaries.size(); ++channelIndex) {
                    const int start = boundaries[channelIndex];
                    const int stop = boundaries[channelIndex + 1];
                    if (start < 0 || stop <= start
                        || stop > rawAmpEntry.ampNorm.size()
                        || stop > rawPhEntry.phNorm.size()) {
                        unifiedBlock.channels.clear();
                        break;
                    }

                    UnifiedRfChannel channel;
                    channel.channelIndex = channelIndex;
                    channel.source = RfSourceType::RoosPtxHack;
                    channel.amplitudeScale = double(rf.amplitude);
                    channel.phaseOffsetRad = rf.phaseOffset
                        + rf.phasePPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla;
                    channel.freqOffsetHz = rf.freqOffset + rf.freqPPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla;
                    channel.phaseIsRealLike = rawPhEntry.isRealLike;
                    channel.ampNorm = rawAmpEntry.ampNorm.mid(start, stop - start);
                    channel.phaseNorm = rawPhEntry.phNorm.mid(start, stop - start);
                    unifiedBlock.channels.append(channel);
                }

                LogManager::getInstance().appendStructured(
                    QtInfoMsg,
                    QStringLiteral("PulseqLoader"),
                    QStringLiteral("RoosPtxHack split RF block %1: rawLen=%2 boundaries=%3 samplesPerChannel=%4 channels=%5")
                        .arg(i)
                        .arg(int(rawAmp.size()))
                        .arg(boundaries.size() - 1)
                        .arg(samplesPerChannel)
                        .arg(unifiedBlock.channels.size()));
            }

            if (unifiedBlock.channels.isEmpty()) {
                LogManager::getInstance().appendStructured(
                    QtWarningMsg,
                    QStringLiteral("PulseqLoader"),
                    QStringLiteral("RoosPtxHack fallback to single-channel for RF block %1: decodedLen=%2 rawShapeOk=%3 timeShapeId=%4")
                        .arg(i)
                        .arg(rfLength)
                        .arg(hasRawShapes ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(rf.timeShape));
                makeChannel(0,
                            RfSourceType::RoosPtxHack,
                            double(rf.amplitude),
                            rf.phaseOffset + rf.phasePPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla);
            }
        } else {
            makeChannel(0,
                        roosDetection.detected ? RfSourceType::RoosPtxHack : RfSourceType::SingleChannel,
                        double(rf.amplitude),
                        rf.phaseOffset + rf.phasePPM * 1e-6 * Settings::getInstance().getGamma() * m_b0Tesla);
        }

        if (!unifiedBlock.channels.isEmpty()) {
            m_unifiedRfChannelCount = std::max(m_unifiedRfChannelCount, int(unifiedBlock.channels.size()));
            m_unifiedRfBlocks.append(unifiedBlock);
        }
    }

    if (hasExplicitRfShim && roosDetection.detected) {
        const QString message = QStringLiteral(
            "Illegal RF multi-channel combination detected: the sequence contains explicit RF_SHIMS and "
            "also matches the RoosPtxHack RF/ADC phase-table pattern (%1 RF groups, %2 ADC groups, %3 matched phases). "
            "Mixing both encodings in one file is not supported.")
            .arg(roosDetection.matchedRfGroupCount)
            .arg(roosDetection.matchedAdcGroupCount)
            .arg(roosDetection.uniqueMatchedPhaseCount);
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    }

    m_detectedRoosPtxHack = roosDetection.detected;
    if (roosDetection.detected) {
        if (enableRoosAutoDetection) {
            m_unifiedRfStatusMessage = QStringLiteral(
                "Detected RoosPtxHack-style RF/ADC phase tables (%1 RF groups, %2 ADC groups, %3 matched phases, inferred %4 channels x %5 samples).")
                .arg(roosDetection.matchedRfGroupCount)
                .arg(roosDetection.matchedAdcGroupCount)
                .arg(roosDetection.uniqueMatchedPhaseCount)
                .arg(roosDetection.inferredChannelCount)
                .arg(roosDetection.inferredSamplesPerChannel);
        } else {
            m_unifiedRfStatusMessage = QStringLiteral(
                "Detected RoosPtxHack-style RF/ADC phase tables, but auto-detection is disabled in settings.");
        }
    } else if (!enableRoosAutoDetection) {
        m_unifiedRfStatusMessage = QStringLiteral("RoosPtxHack auto-detection disabled in settings.");
    }
    return true;
}

bool PulseqLoader::appendUnifiedRfChannelSeries(const UnifiedRfBlock& block,
                                                const UnifiedRfChannel& channel,
                                                int pixelWidth,
                                                double window,
                                                bool allowDecimate,
                                                QVector<double>& tAmp,
                                                QVector<double>& vAmp,
                                                QVector<double>& tPh,
                                                QVector<double>& vPh) const
{
    tAmp.clear();
    vAmp.clear();
    tPh.clear();
    vPh.clear();
    if (block.rfLength <= 0 || channel.ampNorm.isEmpty() || channel.phaseNorm.isEmpty()) {
        return false;
    }

    const double tStart = block.startTimeAxis;
    const double dt = block.dwellAxis;
    const double duration = block.rfLength * dt;
    const int pxForBlock = std::max(1, int(std::round(duration / std::max(1e-9, window) * pixelWidth)));
    const double ampTol = 1e-6;

    double ampMin = std::numeric_limits<double>::infinity();
    double ampMax = -std::numeric_limits<double>::infinity();
    int peakIndex = 0;
    for (int i = 0; i < channel.ampNorm.size(); ++i) {
        const double value = double(channel.ampNorm[i]);
        if (value < ampMin) ampMin = value;
        if (value > ampMax) {
            ampMax = value;
            peakIndex = i;
        }
    }
    const bool isFlatAmp = std::abs(ampMax - ampMin) <= ampTol;
    const double ppp = (pxForBlock > 0) ? double(block.rfLength) / double(pxForBlock) : double(block.rfLength);
    if (isFlatAmp) {
        const double flatAmp = channel.amplitudeScale * (channel.ampNorm.isEmpty() ? 0.0 : double(channel.ampNorm[0]));
        const double tEnd = tStart + duration;
        tAmp = {tStart, tStart, tEnd, tEnd};
        vAmp = {0.0, flatAmp, flatAmp, 0.0};
    } else if (!allowDecimate || block.rfLength <= 64 || ppp <= 1.2) {
        tAmp.reserve(block.rfLength);
        vAmp.reserve(block.rfLength);
        for (int i = 0; i < block.rfLength; ++i) {
            tAmp.append(tStart + i * dt);
            vAmp.append(double(channel.ampNorm[i]) * channel.amplitudeScale);
        }
    } else {
        int target = std::min(block.rfLength, std::min(10000, int(std::round(pxForBlock * 2.0))));
        if (target <= 3 || pxForBlock <= 2) {
            auto clampIndex = [&](int idx) { return std::max(0, std::min(block.rfLength - 1, idx)); };
            QSet<int> idxs;
            idxs.insert(0);
            idxs.insert(clampIndex(int(std::floor(0.25 * (block.rfLength - 1)))));
            idxs.insert(clampIndex(peakIndex - 1));
            idxs.insert(clampIndex(peakIndex));
            idxs.insert(clampIndex(peakIndex + 1));
            idxs.insert(clampIndex(int(std::floor(0.75 * (block.rfLength - 1)))));
            idxs.insert(block.rfLength - 1);
            QList<int> sorted = QList<int>(idxs.constBegin(), idxs.constEnd());
            std::sort(sorted.begin(), sorted.end());
            for (int idx : sorted) {
                tAmp.append(tStart + idx * dt);
                vAmp.append(double(channel.ampNorm[idx]) * channel.amplitudeScale);
            }
        } else {
            lttbDownsampleUniform(channel.ampNorm, tStart, dt, target, tAmp, vAmp);
            for (double& value : vAmp) {
                value *= channel.amplitudeScale;
            }
        }
    }

    const double pppPh = (pxForBlock > 0) ? double(block.rfLength) / double(pxForBlock) : double(block.rfLength);
    if (!allowDecimate || block.rfLength <= 64 || pppPh <= 1.2) {
        tPh.reserve(block.rfLength);
        vPh.reserve(block.rfLength);
        for (int i = 0; i < block.rfLength; ++i) {
            tPh.append(tStart + i * dt);
            vPh.append(double(channel.phaseNorm[i]));
        }
    } else {
        int target = std::min(block.rfLength, std::min(10000, int(std::round(pxForBlock * 2.0))));
        lttbDownsampleUniform(channel.phaseNorm, tStart, dt, target, tPh, vPh);
    }

    applyRfPhaseOffsets(block, channel, tPh, vPh);
    return !tAmp.isEmpty() || !tPh.isEmpty();
}

void PulseqLoader::applyRfPhaseOffsets(const UnifiedRfBlock& block,
                                       const UnifiedRfChannel& channel,
                                       QVector<double>& tPh,
                                       QVector<double>& vPh) const
{
    for (int i = 0; i < vPh.size() && i < tPh.size(); ++i) {
        const double tLocalSec = ((tPh[i] - block.startTimeAxis) / tFactor) * 1e-6;
        const double phaseVal = channel.phaseIsRealLike ? 0.0 : vPh[i];
        const double totalPhase = phaseVal + channel.phaseOffsetRad + 2.0 * M_PI * tLocalSec * channel.freqOffsetHz;
        vPh[i] = std::atan2(std::sin(totalPhase), std::cos(totalPhase));
    }
}

void PulseqLoader::appendUnifiedRfBlockSeries(const UnifiedRfBlock& block,
                                              int pixelWidth,
                                              double window,
                                              bool allowDecimate,
                                              UnifiedRfViewport& viewport) const
{
    for (const UnifiedRfChannel& channel : block.channels) {
        const int index = channel.channelIndex;
        if (index < 0
            || index >= viewport.ampTimeByChannel.size()
            || index >= viewport.ampValueByChannel.size()
            || index >= viewport.phaseTimeByChannel.size()
            || index >= viewport.phaseValueByChannel.size()) {
            continue;
        }

        QVector<double> tAmpBlk, vAmpBlk, tPhBlk, vPhBlk;
        if (!appendUnifiedRfChannelSeries(block, channel, pixelWidth, window, allowDecimate, tAmpBlk, vAmpBlk, tPhBlk, vPhBlk)) {
            continue;
        }

        auto appendWithBreak = [](QVector<double>& dstT, QVector<double>& dstV,
                                  const QVector<double>& srcT, const QVector<double>& srcV) {
            if (srcT.isEmpty() || srcV.isEmpty()) {
                return;
            }
            if (!dstT.isEmpty()) {
                dstT.append(srcT.first());
                dstV.append(std::numeric_limits<double>::quiet_NaN());
            }
            dstT += srcT;
            dstV += srcV;
        };

        appendWithBreak(viewport.ampTimeByChannel[index], viewport.ampValueByChannel[index], tAmpBlk, vAmpBlk);
        appendWithBreak(viewport.phaseTimeByChannel[index], viewport.phaseValueByChannel[index], tPhBlk, vPhBlk);
    }
}

void PulseqLoader::getUnifiedRfViewport(double visibleStart, double visibleEnd, int pixelWidth,
                                        UnifiedRfViewport& viewport)
{
    viewport.ampTimeByChannel = QVector<QVector<double>>(m_unifiedRfChannelCount);
    viewport.ampValueByChannel = QVector<QVector<double>>(m_unifiedRfChannelCount);
    viewport.phaseTimeByChannel = QVector<QVector<double>>(m_unifiedRfChannelCount);
    viewport.phaseValueByChannel = QVector<QVector<double>>(m_unifiedRfChannelCount);
    if (m_unifiedRfBlocks.isEmpty() || pixelWidth <= 0) {
        return;
    }

    const double window = std::max(1e-9, visibleEnd - visibleStart);
    long long totalRfSamples = 0;
    for (const UnifiedRfBlock& block : m_unifiedRfBlocks) {
        const double duration = block.rfLength * block.dwellAxis;
        if (block.startTimeAxis >= visibleEnd || (block.startTimeAxis + duration) <= visibleStart) {
            continue;
        }
        totalRfSamples += std::max(1, block.rfLength * std::max(1, int(block.channels.size())));
    }

    bool allowDecimate = totalRfSamples > 120000;
    if (pixelWidth > 0) {
        const double pppTotal = double(std::max<long long>(1, totalRfSamples)) / double(pixelWidth);
        if (pppTotal <= 2.0) {
            allowDecimate = false;
        }
    }

    for (const UnifiedRfBlock& block : m_unifiedRfBlocks) {
        const double duration = block.rfLength * block.dwellAxis;
        if (block.startTimeAxis >= visibleEnd || (block.startTimeAxis + duration) <= visibleStart) {
            continue;
        }
        appendUnifiedRfBlockSeries(block, pixelWidth, window, allowDecimate, viewport);
    }
}

QPair<double,double> PulseqLoader::getRfGlobalRangeAmp()
{
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (const UnifiedRfBlock& block : m_unifiedRfBlocks) {
        for (const UnifiedRfChannel& channel : block.channels) {
            for (float sample : channel.ampNorm) {
                const double value = double(sample) * channel.amplitudeScale;
                if (!std::isfinite(value)) continue;
                mn = std::min(mn, value);
                mx = std::max(mx, value);
            }
        }
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = -1.0; mx = 1.0; }
    return qMakePair(mn, mx);
}

QPair<double,double> PulseqLoader::getRfGlobalRangePh()
{
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (const UnifiedRfBlock& block : m_unifiedRfBlocks) {
        for (const UnifiedRfChannel& channel : block.channels) {
            QVector<double> tPh;
            QVector<double> vPh;
            tPh.reserve(block.rfLength);
            vPh.reserve(block.rfLength);
            for (int i = 0; i < block.rfLength; ++i) {
                tPh.append(block.startTimeAxis + i * block.dwellAxis);
                vPh.append(double(channel.phaseNorm.value(i)));
            }
            applyRfPhaseOffsets(block, channel, tPh, vPh);
            for (double value : vPh) {
                if (!std::isfinite(value)) continue;
                mn = std::min(mn, value);
                mx = std::max(mx, value);
            }
        }
    }
    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = -1.0; mx = 1.0; }
    return qMakePair(mn, mx);
}

// (removed getRfViewportRangeAmp; y-axis ranges are computed once at load time)

// ADC Phase viewport rendering (MATLAB-matching formula: angle(exp(i*phase)*exp(i*2*pi*t*freq)))
// Three optimization strategies to keep rendering fast:
//   1. Pixel-aware decimation: stride through samples when points-per-pixel > 2
//   2. Viewport caching: if visibleStart/visibleEnd/pixelWidth unchanged, return cached result
//   3. NaN breaks between ADC blocks: enables lsLine rendering (10x faster than scatter dots)
//      while preventing lines from connecting unrelated ADC events
void PulseqLoader::getAdcPhaseViewport(double visibleStart, double visibleEnd, int pixelWidth,
                                       QVector<double>& tOut, QVector<double>& vOut)
{
    // Viewport cache: return cached data if viewport/pixelWidth unchanged
    if (m_adcPhaseCache.valid &&
        m_adcPhaseCache.visibleStart == visibleStart &&
        m_adcPhaseCache.visibleEnd == visibleEnd &&
        m_adcPhaseCache.pixelWidth == pixelWidth)
    {
        tOut = m_adcPhaseCache.tData;
        vOut = m_adcPhaseCache.vData;
        return;
    }

    tOut.clear(); vOut.clear();
    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.isEmpty() || pixelWidth <= 0) return;

    // Find visible block range via binary search
    auto itStart = std::lower_bound(vecBlockEdges.begin(), vecBlockEdges.end(), visibleStart);
    int startBlock = std::max(0, int(std::distance(vecBlockEdges.begin(), itStart)) - 1);
    
    double gamma = Settings::getInstance().getGamma();

    // Count total visible ADC samples for global decimation gating (like RF approach)
    long long totalAdcSamples = 0;
    for (int i = startBlock; i < vecBlockEdges.size() - 1; ++i) {
        if (vecBlockEdges[i] > visibleEnd) break;
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (!blk || !blk->isADC()) continue;
        totalAdcSamples += blk->GetADCEvent().numSamples;
    }
    if (totalAdcSamples == 0) return;

    // Determine stride based on points-per-pixel (ppp), mirroring RF decimation logic
    // For line plots, ~2 samples/pixel is sufficient
    double pppTotal = double(totalAdcSamples) / double(pixelWidth);
    int stride = 1;
    if (pppTotal > 2.0) {
        int targetPoints = pixelWidth * 2;
        stride = std::max(1, static_cast<int>(std::ceil(double(totalAdcSamples) / double(targetPoints))));
    }

    // Emit points with computed stride, NaN-break between ADC blocks for line plot
    bool emittedAny = false;
    for (int i = startBlock; i < vecBlockEdges.size() - 1; ++i) {
        double blockStart = vecBlockEdges[i];
        if (blockStart > visibleEnd) break;
        
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (!blk || !blk->isADC()) continue;

        ADCEvent& adc = blk->GetADCEvent();
        int nSamples = adc.numSamples;
        double dwell = adc.dwellTime * 1e-9; // ns to seconds
        double delay = adc.delay * 1e-6;     // us to seconds
        
        double fullFreqOff = adc.freqOffset + adc.freqPPM * 1e-6 * gamma * m_b0Tesla;
        double fullPhaseOff = adc.phaseOffset + adc.phasePPM * 1e-6 * gamma * m_b0Tesla;

        // Insert NaN break before this block to separate from previous block's line
        if (emittedAny) {
            tOut.append(tOut.last());
            vOut.append(std::numeric_limits<double>::quiet_NaN());
        }

        bool emittedInBlock = false;
        for (int k = 0; k < nSamples; k += stride) {
            double t_local = delay + (k + 0.5) * dwell; // Center of dwell
            double t_offset_us = adc.delay + (k + 0.5) * (adc.dwellTime * 1e-3);
            double t_plot = vecBlockEdges[i] + t_offset_us * tFactor;
            
            if (t_plot < visibleStart) continue;
            if (t_plot > visibleEnd) break;

            double totalPhase = fullPhaseOff + 2.0 * M_PI * t_local * fullFreqOff;
            double wrapped = std::atan2(std::sin(totalPhase), std::cos(totalPhase));
            
            tOut.append(t_plot);
            vOut.append(wrapped);
            emittedInBlock = true;
        }
        if (emittedInBlock) emittedAny = true;
    }

    // Store in cache for next call
    m_adcPhaseCache.visibleStart = visibleStart;
    m_adcPhaseCache.visibleEnd = visibleEnd;
    m_adcPhaseCache.pixelWidth = pixelWidth;
    m_adcPhaseCache.tData = tOut;
    m_adcPhaseCache.vData = vOut;
    m_adcPhaseCache.valid = true;
}

void PulseqLoader::buildShapeScaleAggregates()
{
    // Reset
    m_rfAgg.clear();
    for (int c = 0; c < 3; ++c) {
        m_gradAgg[c].clear();
        m_gradTrapMaxPosScale[c] = 0.0;
        m_gradTrapMinNegScale[c] = 0.0;
        m_gradExtTrapGlobalMin[c] = std::numeric_limits<double>::infinity();
        m_gradExtTrapGlobalMax[c] = -std::numeric_limits<double>::infinity();
    }
    // Single pass over blocks
    for (SeqBlock* blk : m_vecDecodeSeqBlocks) {
        if (!blk) continue;
        // RF
        if (blk->isRF()) {
            RFEvent& rf = blk->GetRFEvent();
            int RFLength = blk->GetRFLength();
            if (RFLength > 0) {
                float* rfList = blk->GetRFAmplitudePtr();
                const RFAmpEntry& eA = ensureRfAmpCached(rfList, RFLength, rf.magShape, rf.timeShape);
                QString key = rfAmpKey(rf.magShape, rf.timeShape, RFLength);
                ScaleAgg& ag = m_rfAgg[key];
                if (!ag.hasShape) ag.updateShape(eA.ampMin, eA.ampMax);
                ag.updateScale(double(rf.amplitude));
            }
        }
        // Gradients per channel
        for (int ch = 0; ch < 3; ++ch) {
            bool hasG = blk->isTrapGradient(ch) || blk->isArbitraryGradient(ch) || blk->isExtTrapGradient(ch);
            if (!hasG) continue;
            const GradEvent& grad = blk->GetGradEvent(ch);
            if (blk->isTrapGradient(ch)) {
                double s = double(grad.amplitude);
                if (s >= 0) m_gradTrapMaxPosScale[ch] = std::max(m_gradTrapMaxPosScale[ch], s);
                else        m_gradTrapMinNegScale[ch] = std::min(m_gradTrapMinNegScale[ch], s);
                continue;
            }
            if (blk->isArbitraryGradient(ch)) {
                int numSamples = blk->GetArbGradNumSamples(ch);
                const float* shapePtr = blk->GetArbGradShapePtr(ch);
                if (numSamples > 0 && shapePtr) {
                    const GradShapeEntry& e = ensureGradCached(shapePtr, numSamples, grad.waveShape, grad.timeShape);
                    QString key = gradKey(grad.waveShape, grad.timeShape, numSamples);
                    ScaleAgg& ag = m_gradAgg[ch][key];
                    if (!ag.hasShape) ag.updateShape(e.vMin, e.vMax);
                    ag.updateScale(double(grad.amplitude));
                }
                continue;
            }
            if (blk->isExtTrapGradient(ch)) {
                const std::vector<float>& shape = blk->GetExtTrapGradShape(ch);
                if (!shape.empty()) {
                    double smin = std::numeric_limits<double>::infinity();
                    double smax = -std::numeric_limits<double>::infinity();
                    for (float v : shape) {
                        if (!std::isnan(v)) { if (v < smin) smin = v; if (v > smax) smax = v; }
                    }
                    if (!std::isfinite(smin) || !std::isfinite(smax)) { smin = 0.0; smax = 0.0; }
                    double scale = double(grad.amplitude);
                    double cands[2] = { smin * scale, smax * scale };
                    for (double v : cands) {
                        if (!std::isfinite(v)) continue;
                        if (v < m_gradExtTrapGlobalMin[ch]) m_gradExtTrapGlobalMin[ch] = v;
                        if (v > m_gradExtTrapGlobalMax[ch]) m_gradExtTrapGlobalMax[ch] = v;
                    }
                }
                continue;
            }
        }
    }
}

QList<QPair<QString, int>> PulseqLoader::getActiveLabels(int blockIdx) const
{
    QList<QPair<QString, int>> result;
    if (!labelSnapshotAfterBlock(blockIdx))
        return result;

    const QStringList labels = getAvailableExtensionLabels();
    for (const QString& label : labels)
    {
        if (!Settings::getInstance().isExtensionLabelEnabled(label))
            continue;
        if (!m_usedExtensions.contains(label.toUpper()))
            continue;

        int value = 0;
        bool isFlag = false;
        if (!getExtensionValueAfterBlock(blockIdx, label, value, isFlag))
            continue;

        if (isFlag)
        {
            if (value != 0)
                result.append({label.toUpper(), 1});
        }
        else
        {
            result.append({label.toUpper(), value});
        }
    }
    // Sort alphabetically by name
    std::sort(result.begin(), result.end(), [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
        return a.first < b.first;
    });
    return result;
}
