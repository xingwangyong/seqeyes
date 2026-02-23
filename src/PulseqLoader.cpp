#include "PulseqLoader.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "WaveformDrawer.h"
#include "TRManager.h"
#include "SeriesBuilder.h"
#include "KSpaceTrajectory.h"
#include "InteractionHandler.h"
#include "Settings.h"
#include <QCryptographicHash>

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <iostream>
#include <sstream>
#include <complex>
#include <cmath>
#include <array>
#include <algorithm>
#include <utility>
#include <QSet>

#define SAFE_DELETE(p) { if(p) { delete p; p = nullptr; } }

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
}

PulseqLoader::~PulseqLoader()
{
    ClearPulseqCache();
}

void PulseqLoader::OpenPulseqFile()
{
    // Use last open directory if available, otherwise use current path
    QString startDir = m_sLastOpenDirectory.isEmpty() ? QDir::currentPath() : m_sLastOpenDirectory;
    
    m_sPulseqFilePath = QFileDialog::getOpenFileName(
        m_mainWindow,
        "Select a Pulseq File",
        startDir,
        "Text Files (*.seq);;All Files (*)"
    );

    if (!m_sPulseqFilePath.isEmpty())
    {
        // Save the directory of the selected file
        QFileInfo fileInfo(m_sPulseqFilePath);
        m_sLastOpenDirectory = fileInfo.absolutePath();
        saveLastOpenDirectory();
        
        if (!LoadPulseqFile(m_sPulseqFilePath))
        {
            m_sPulseqFilePath.clear();
            std::cout << "LoadPulseqFile failed!\n";
        }
        m_sPulseqFilePathCache = m_sPulseqFilePath;
    }
}

void PulseqLoader::ReOpenPulseqFile()
{
    if (m_sPulseqFilePathCache.size() > 0)
    {
        ClearPulseqCache();
        LoadPulseqFile(m_sPulseqFilePathCache);
    }
}

bool PulseqLoader::ClosePulseqFile()
{
    ClearPulseqCache();
    return true;
}

void PulseqLoader::ClearPulseqCache()
{
    if (m_mainWindow)
    {
        m_mainWindow->clearLoadedFileTitle();
        if (auto lbl = m_mainWindow->getVersionLabel()) { lbl->setText(""); lbl->setVisible(false); }
        if (auto pb = m_mainWindow->getProgressBar()) { pb->hide(); }
        if (m_mainWindow->ui && m_mainWindow->ui->customPlot)
        {
            // Do not clear graphs here, as graphs are persistent and owned by WaveformDrawer.
            // Just trigger a light replot; WaveformDrawer will set empty data on next draw.
            m_mainWindow->ui->customPlot->replot();
        }
    }

    m_dTotalDuration_us = 0.;
    m_bHasRepetitionTime = false;
    m_dRepetitionTime_us = 0.0;
    m_nTrCount = 0;
    m_vecTrBlockIndices.clear();

    // Clear RF/Gradient shape caches
    m_rfAmpCache.clear();
    m_rfPhCache.clear();
    m_gradShapeCache.clear();
    m_supportsRfUseMetadata = false;
    m_hasEchoTimeDefinition = false;
    m_teTime_us = 0.0;
    m_teDurationAxis = 0.0;
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
    m_usedExtensions.clear();
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
        // m_spPulseqSeq will be recreated based on file version
        for (uint16_t ushBlockIndex = 0; ushBlockIndex < m_vecDecodeSeqBlocks.size(); ushBlockIndex++)
        {
            SAFE_DELETE(m_vecDecodeSeqBlocks[ushBlockIndex]);
        }
        m_vecDecodeSeqBlocks.clear();
        std::cout << m_sPulseqFilePath.toStdString() << " Closed\n";
    }
    if (m_mainWindow) { m_mainWindow->setWindowFilePath(""); }
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

    // First, read version information without loading the full file
    std::pair<int, int> version = ReadFileVersion(sPulseqFilePath.toStdString());
    if (version.first == -1 || version.second == -1)
    {
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
            m_mainWindow->setEnabled(true);
            const char* msg = "Missing required definition: GradientRasterTime (seconds)\n\n"
                              "The sequence lacks GradientRasterTime in [DEFINITIONS].\n"
                              "Please add e.g. 'GradientRasterTime = 1e-5' and reload.";
            if (m_silentMode) { qWarning() << msg; }
            else { QMessageBox::critical(m_mainWindow, "Missing Definition", msg); }
            ClearPulseqCache();
            return false;
        }
    }

    // Debug: Check if gradient library was loaded
    qDebug() << "Pulseq file loaded successfully";
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
            std::stringstream sLog;
            sLog << "Decode SeqBlock failed, block index: " << ushBlockIndex;
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
    updateEchoAndExcitationMetadata(shVersionMajor, shVersionMinor);

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

    nBlockRangeStart = 0;
    nBlockRangeEnd = std::min(int(lSeqBlockNum - 1), 10);

    // Precompute per-shape scale aggregates for RF/Gradients (single pass over blocks)
    buildShapeScaleAggregates();

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

    if (lSeqBlockNum > 0)
    {
        // Determine initial view range based on render mode
        TRManager* trManager = m_mainWindow->getTRManager();
        double initialStartTime, initialEndTime;

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

        // Single-point sync instead of per-rect setRange to avoid N cascaded updates
        if (auto* ih = m_mainWindow->getInteractionHandler()) {
            ih->synchronizeXAxes(QCPRange(initialStartTime, initialEndTime));
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

    // Keep UI/status updates only; drawing was already triggered via synchronizeXAxes
    trManager->updateTrStatusDisplay();
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
    {
        m_mainWindow->refreshTrajectoryPlotData();
    }
    if (m_mainWindow)
    {
        // Show "SeqEyes - file.seq" only after a successful load.
        m_mainWindow->setLoadedFileTitle(sPulseqFilePath);
    }
    m_mainWindow->setEnabled(true);
    return true;
}

void PulseqLoader::buildLabelSnapshotCache()
{
    m_labelSnapshots.clear();
    m_usedExtensions.clear();
    const int nBlocks = static_cast<int>(m_vecDecodeSeqBlocks.size());
    if (nBlocks <= 0)
        return;

    // Do NOT call pulseq's LabelStateAndBookkeeping::updateLabelValues here because
    // it can crash on unknown label IDs (>=1000) for LABELINC events. We apply events ourselves with bounds checks.
    QVector<int>  counterVal(NUM_LABELS, 0);
    QVector<bool> flagVal(NUM_FLAGS, false);

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
                if (!s.empty()) { m_usedExtensions.insert(QString::fromStdString(s).toUpper()); return; }
                const std::string u = seq->GetUnknownLabelName(id);
                if (!u.empty()) { m_usedExtensions.insert(QString::fromStdString(u).toUpper()); return; }
                m_usedExtensions.insert(QString("LABEL[%1]").arg(id).toUpper());
            };
            auto markFlagUsed = [&](int id) {
                if (!seq) return;
                const std::string s = seq->getFlagIdAsString(id);
                if (!s.empty()) { m_usedExtensions.insert(QString::fromStdString(s).toUpper()); return; }
                m_usedExtensions.insert(QString("FLAG[%1]").arg(id).toUpper());
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
            }
        }

        LabelSnapshot snap;
        snap.counters = counterVal;
        snap.flags = flagVal;
        m_labelSnapshots[i] = snap;
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
    for (int channel = 0; channel < 3; ++channel)
    {
        if (pSeqBlock->isTrapGradient(channel) || pSeqBlock->isArbitraryGradient(channel) || pSeqBlock->isExtTrapGradient(channel))
        {
            blockInfo += QString("|-----------------------------------------------------------------------------------------------|\n");
            const GradEvent& grad = pSeqBlock->GetGradEvent(channel);
            blockInfo += QString("Gradient Event (Channel %1):\nAmplitude: %2 Hz/m\nDelay: %3 us")
                .arg(gradChannels[channel])
                .arg(grad.amplitude)
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

        auto activeLabels = getActiveLabels(currentBlock);
        if (!activeLabels.isEmpty())
        {
            for (const auto& pair : activeLabels)
            {
                blockInfo += QString("  %1=%2\n").arg(pair.first).arg(pair.second);
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
    m_supportsRfUseMetadata = (versionMajor > 1) || (versionMajor == 1 && versionMinor >= 5);
    m_rfUseGuessed = false;
    m_rfGuessWarning.clear();

    if (!m_spPulseqSeq)
        return;

    std::vector<double> teDef = m_spPulseqSeq->GetDefinition("TE");
    if (teDef.empty())
        teDef = m_spPulseqSeq->GetDefinition("EchoTime");
    if (!teDef.empty())
    {
        m_hasEchoTimeDefinition = true;
        m_teTime_us = teDef[0] * 1e6;
        m_teDurationAxis = m_teTime_us * tFactor;
    }

    if (m_vecDecodeSeqBlocks.empty() || vecBlockEdges.size() < 2)
        return;

    computeKSpaceTrajectory();
}


void PulseqLoader::computeKSpaceTrajectory()
{
    double gradRasterUs = -1.0;
    double rfRasterUs = -1.0;
    if (m_spPulseqSeq) {
        std::vector<double> def = m_spPulseqSeq->GetDefinition("GradientRasterTime");
        if (!def.empty() && std::isfinite(def[0]) && def[0] > 0.0) {
            gradRasterUs = def[0] * 1e6; // seconds -> microseconds
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
            double dwellUs = static_cast<double>(adc.dwellTime) * 1e-3; // ns -> us
            double dwellInternal = dwellUs * tFactor;
            double startInternal = vecBlockEdges[i] + adc.delay * tFactor + 0.5 * dwellInternal;
            for (int sample = 0; sample < adc.numSamples; ++sample) {
                adcEventTimes.append(startInternal + sample * dwellInternal);
            }
        }
    }

    // Read B0 from [DEFINITIONS] if available (needed to detect fat-sat RF use in v1.4.x files)
    double b0Tesla = 0.0;
    if (m_spPulseqSeq) {
        std::vector<double> defB0 = m_spPulseqSeq->GetDefinition("B0");
        if (!defB0.empty())
            b0Tesla = defB0[0];
    }
    // If B0 is undefined, assume 3.0T (standard high field) for PPM calculations
    // This maintains compatibility with sequences that use PPM but don't define B0,
    // while remaining safe for legacy files (where freqPPM will be 0 anyway).
    if (b0Tesla == 0.0) {
        b0Tesla = 3.0; // Default to 3.0T to match KSpaceTrajectory
        // qWarning() << "B0 not defined in sequence [DEFINITIONS]. Assuming 3.0T for PPM calculations.";
    }
    m_b0Tesla = b0Tesla; // Store for phase computation

    KSpaceTrajectory::Input input { m_vecDecodeSeqBlocks,
                                    vecBlockEdges,
                                    tFactor,
                                    m_supportsRfUseMetadata,
                                    rfRasterUs,
                                    gradRasterUs,
                                    std::move(adcEventTimes),
                                    b0Tesla };
    KSpaceTrajectory::Result result = KSpaceTrajectory::compute(input);

    m_excitationCentersAxis = result.excitationTimesInternal;
    m_refocusingCentersAxis = result.refocusingTimesInternal;
    m_kTrajectoryX = result.kx;
    m_kTrajectoryY = result.ky;
    m_kTrajectoryZ = result.kz;
    m_kTimeSec      = result.t;
    m_kTrajectoryXAdc = result.kx_adc;
    m_kTrajectoryYAdc = result.ky_adc;
    m_kTrajectoryZAdc = result.kz_adc;
    m_kTimeAdcSec     = result.t_adc;
    m_rfUseGuessed = result.rfUseGuessed;
    m_rfGuessWarning = result.warning;
    m_rfUsePerBlock = result.rfUsePerBlock;
    m_kTrajectoryReady = true;
}

void PulseqLoader::ensureTrajectoryPrepared()
{
    if (!m_kTrajectoryReady)
    {
        computeKSpaceTrajectory();
    }
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

    // Update TR status display text
    TRManager* trm = m_mainWindow->getTRManager();
    if (trm) trm->updateTrStatusDisplay();

    // Update trajectory if visible
    if (m_mainWindow && m_mainWindow->isTrajectoryVisible())
        m_mainWindow->refreshTrajectoryPlotData();

    if (m_mainWindow && m_mainWindow->ui && m_mainWindow->ui->customPlot)
        m_mainWindow->ui->customPlot->replot();
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
            double duration = numSamples * dt;
            if (tStart >= visibleEnd || (tStart + duration) <= visibleStart) continue;
            int pxForBlock = std::max(1, int(std::round(duration / window * pixelWidth)));
            // Prefer LTTB decimation for shape fidelity
            QVector<double> tBlk, vBlk;
            double ppp = (pxForBlock > 0) ? double(numSamples) / double(pxForBlock) : double(numSamples);
            if (!allowDecimateGrad || numSamples <= 64 || ppp <= 1.2) {
                tBlk.reserve(numSamples); vBlk.reserve(numSamples);
                for (int j=0;j<numSamples;++j){ tBlk.append(tStart + j*dt); vBlk.append(double(entry.norm[j]) * double(grad.amplitude)); }
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
                    for (int k : sorted){ tBlk.append(tStart + k*dt); vBlk.append(double(entry.norm[k]) * double(grad.amplitude)); }
                } else {
                    QVector<double> dT, dV; lttbDownsampleUniform(entry.norm, tStart, dt, target, dT, dV);
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
        double tEnd = tStart + (n - 1) * dt;
        if (time < tStart || time > tEnd) return false;
        double u = (time - tStart) / dt;
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
    tAmp.clear(); vAmp.clear(); tPh.clear(); vPh.clear();
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

    bool haveLastAmp = false, haveLastPh = false;
    double lastTAmp = 0.0, lastVAmp = 0.0;
    double lastTPh  = 0.0, lastVPh  = 0.0;

    // Global decimation gating (heavy-only):
    const int DECIMATE_TOTAL_THRESHOLD_RF = 120000; // conservative; for very large windows
    long long totalRfSamples = 0;
    for (int i = startBlock; i <= endBlock; ++i) {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (!blk || !blk->isRF()) continue;
        totalRfSamples += std::max(0, blk->GetRFLength());
    }
    bool allowDecimateRF = (totalRfSamples > DECIMATE_TOTAL_THRESHOLD_RF);
    // Zoom-in gating: if overall points-per-pixel is low, render full detail regardless of total
    if (pixelWidth > 0) {
        double pppTotal = double(std::max<long long>(1, totalRfSamples)) / double(pixelWidth);
        if (pppTotal <= 2.0) allowDecimateRF = false;
    }

    for (int i = startBlock; i <= endBlock; ++i) {
        SeqBlock* blk = m_vecDecodeSeqBlocks[i];
        if (!blk || !blk->isRF()) continue;
        RFEvent& rf = blk->GetRFEvent();
        int RFLength = blk->GetRFLength();
        if (RFLength <= 0) continue;
        float dwell = blk->GetRFDwellTime();
        float* rfList = blk->GetRFAmplitudePtr();
        float* phaseList = blk->GetRFPhasePtr();
        const double tStart = vecBlockEdges[i] + rf.delay * tFactor;
        const double dt = dwell * tFactor;
        const double duration = RFLength * dt;
        // Skip blocks entirely outside range
        if (tStart >= visibleEnd || (tStart + duration) <= visibleStart) continue;
        // Allocate pixels proportional to duration
        int pxForBlock = std::max(1, int(std::round(duration / window * pixelWidth)));

        const RFAmpEntry& entryA = ensureRfAmpCached(rfList, RFLength, rf.magShape, rf.timeShape);
        // Build amplitude block data (prefer LTTB over min-max)
        QVector<double> tAmpBlk, vAmpBlk;
        double ppp = (pxForBlock > 0) ? double(RFLength) / double(pxForBlock) : double(RFLength);
        if (!allowDecimateRF || RFLength <= 64 || ppp <= 1.2) {
            tAmpBlk.reserve(RFLength); vAmpBlk.reserve(RFLength);
            for (int ii=0;ii<RFLength;++ii){ tAmpBlk.append(tStart + ii*dt); vAmpBlk.append(double(entryA.ampNorm[ii]) * double(rf.amplitude)); }
        } else {
            int target = std::min(RFLength, std::min(10000, int(std::round(pxForBlock*2.0))));
            if (target <= 3 || pxForBlock <= 2) {
                // Ultra-narrow pulse in pixels: sample around peak to preserve Gaussian shape
                int iPeak = (entryA.peakIndex >= 0 && entryA.peakIndex < RFLength) ? entryA.peakIndex : RFLength/2;
                auto clampIndex = [&](int idx){ return std::max(0, std::min(RFLength-1, idx)); };
                QSet<int> idxs;
                idxs.insert(0);
                idxs.insert(clampIndex((int)std::floor(0.25 * (RFLength-1))));
                idxs.insert(clampIndex(iPeak-1));
                idxs.insert(clampIndex(iPeak));
                idxs.insert(clampIndex(iPeak+1));
                idxs.insert(clampIndex((int)std::floor(0.75 * (RFLength-1))));
                idxs.insert(RFLength-1);
                QList<int> sorted = QList<int>(idxs.constBegin(), idxs.constEnd());
                std::sort(sorted.begin(), sorted.end());
                tAmpBlk.reserve(sorted.size()); vAmpBlk.reserve(sorted.size());
                for (int ii : sorted){ tAmpBlk.append(tStart + ii*dt); vAmpBlk.append(double(entryA.ampNorm[ii]) * double(rf.amplitude)); }
            } else {
                QVector<double> dT, dV; lttbDownsampleUniform(entryA.ampNorm, tStart, dt, target, dT, dV);
                tAmpBlk = dT; vAmpBlk.reserve(dV.size()); for (double val : dV){ vAmpBlk.append(val * double(rf.amplitude)); }
            }
        }
        // Continuity handling for amplitude
        auto appendWithBreakAmp = [&](const QVector<double>& tB, const QVector<double>& vB){
            if (tB.isEmpty()) return;
            // Decide if break is needed between last and first (time-gap based)
            if (haveLastAmp) {
                double tFirst = tB.first();
                double dtTol = std::max(1e-9, dt*1.1);
                bool gap = (tFirst - lastTAmp) > dtTol;
                if (gap) { tAmp.append(tFirst); vAmp.append(std::numeric_limits<double>::quiet_NaN()); }
            }
            tAmp += tB; vAmp += vB;
            // Update last valid
            for (int idx = vB.size()-1; idx >= 0; --idx){ if (!std::isnan(vB[idx])) { lastTAmp = tB[idx]; lastVAmp = vB[idx]; haveLastAmp = true; break; } }
            if (!haveLastAmp) { // all NaN? set to end
                lastTAmp = tB.last(); lastVAmp = std::numeric_limits<double>::quiet_NaN(); haveLastAmp = true;
            }
        };
        appendWithBreakAmp(tAmpBlk, vAmpBlk);
        // Keep block separation with NaN break; duplicate last x to preserve sorted order
        if (!tAmp.isEmpty()) {
            double tEnd = tStart + std::max(0, RFLength-1) * dt;
            double tBreak = std::nextafter(tEnd, std::numeric_limits<double>::infinity());
            tAmp.append(tBreak);
            vAmp.append(std::numeric_limits<double>::quiet_NaN());
        }

        // Produce phase series similarly
        // Phase block data + continuity
        QVector<double> tPhBlk, vPhBlk;
        const RFPhEntry& entryP = ensureRfPhCached(phaseList, RFLength, rf.phaseShape, rf.timeShape);
        double pppPh = (pxForBlock > 0) ? double(RFLength) / double(pxForBlock) : double(RFLength);
        if (!allowDecimateRF || RFLength <= 64 || pppPh <= 1.2) {
            tPhBlk.reserve(RFLength); vPhBlk.reserve(RFLength);
            for (int ii=0;ii<RFLength;++ii){ tPhBlk.append(tStart + ii*dt); vPhBlk.append(double(entryP.phNorm[ii])); }
        } else {
            int target = std::min(RFLength, std::min(10000, int(std::round(pxForBlock*2.0))));
            QVector<double> dT, dV; lttbDownsampleUniform(entryP.phNorm, tStart, dt, target, dT, dV);
            tPhBlk = dT; vPhBlk = dV;
        }

        // Apply full phase offsets (MATLAB-matching)
        {
            double gamma = Settings::getInstance().getGamma();
            double fullFreqOff = rf.freqOffset + rf.freqPPM * 1e-6 * gamma * m_b0Tesla;
            double fullPhaseOff = rf.phaseOffset + rf.phasePPM * 1e-6 * gamma * m_b0Tesla;
            
            // Check if logic shape is "Real" (only 0 or pi phases, ignoring small numerical noise)
            // MATLAB uses angle(s * sign(real(s))) which maps pi -> 0 for real pulses (negative lobes).
            bool isRealLike = entryP.isRealLike;

            // tStart is in display units. We need time in seconds from the start of the pulse for freq offset.
            // ii * dt -> gives time in display units from start of pulse.
            // Divide by tFactor to get internal units (us), then * 1e-6 to get seconds.
            
            double minPh = 1e9, maxPh = -1e9;
            for (int k = 0; k < vPhBlk.size(); ++k) {
                double t_display = tPhBlk[k];
                // Convert display duration to seconds: (t_display - tStart) / tFactor -> us -> * 1e-6 -> seconds
                double t_local_sec = ((t_display - tStart) / tFactor) * 1e-6;
                
                // If real-like, ignore the shape phase (treat pi as 0, i.e. negative amplitude)
                // This matches MATLAB's sign(real(s)) correction.
                double phaseVal = isRealLike ? 0.0 : vPhBlk[k];
                
                // Add linear phase evolution: 2*pi * t * freq
                double totalPhase = phaseVal + fullPhaseOff + 2.0 * M_PI * t_local_sec * fullFreqOff;
                
                // Wrap to [-pi, pi]
                double wrapped = std::atan2(std::sin(totalPhase), std::cos(totalPhase));
                vPhBlk[k] = wrapped;
                
                if (wrapped < minPh) minPh = wrapped;
                if (wrapped > maxPh) maxPh = wrapped;
            }
            // Debug print once per block (throttle maybe?)
            // Debug print once per block (throttle maybe?)
            // static int dbgCount = 0; if (dbgCount++ < 20) 
            // qDebug() << "RF Block" << i << "isRealLike:" << isRealLike << "Offset:" << fullPhaseOff 
            //          << "Freq:" << fullFreqOff << "MinPh:" << minPh << "MaxPh:" << maxPh << "B0:" << m_b0Tesla;

        }
        auto appendWithBreakPh = [&](const QVector<double>& tB, const QVector<double>& vB){
            if (tB.isEmpty()) return;
            if (haveLastPh) {
                double tFirst = tB.first();
                double dtTol = std::max(1e-9, dt*1.1);
                bool gap = (tFirst - lastTPh) > dtTol;
                if (gap) { tPh.append(tFirst); vPh.append(std::numeric_limits<double>::quiet_NaN()); }
            }
            tPh += tB; vPh += vB;
            for (int idx = vB.size()-1; idx >= 0; --idx){ if (!std::isnan(vB[idx])) { lastTPh = tB[idx]; lastVPh = vB[idx]; haveLastPh = true; break; } }
            if (!haveLastPh) { lastTPh = tB.last(); lastVPh = std::numeric_limits<double>::quiet_NaN(); haveLastPh = true; }
        };
        appendWithBreakPh(tPhBlk, vPhBlk);
    }
}

QPair<double,double> PulseqLoader::getRfGlobalRangeAmp()
{
    // Use precomputed per-shape aggregates to avoid re-scanning blocks
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    for (auto it = m_rfAgg.constBegin(); it != m_rfAgg.constEnd(); ++it) {
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
    if (!std::isfinite(mn) || !std::isfinite(mx)) { mn = -1.0; mx = 1.0; }
    return qMakePair(mn, mx);
}

QPair<double,double> PulseqLoader::getRfGlobalRangePh()
{
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    QSet<QString> seen;
    for (SeqBlock* blk : m_vecDecodeSeqBlocks) {
        if (!blk || !blk->isRF()) continue;
        RFEvent& rf = blk->GetRFEvent();
        int RFLength = blk->GetRFLength(); if (RFLength <= 0) continue;
        float* rfList = blk->GetRFAmplitudePtr();
        float* phaseList = blk->GetRFPhasePtr();
        QString key = rfPhKey(rf.phaseShape, rf.timeShape, RFLength);
        if (seen.contains(key)) continue; seen.insert(key);
        const RFPhEntry& eP = ensureRfPhCached(phaseList, RFLength, rf.phaseShape, rf.timeShape);
        if (eP.phMin < mn) mn = eP.phMin; if (eP.phMax > mx) mx = eP.phMax;
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
    const LabelSnapshot* snap = labelSnapshotAfterBlock(blockIdx);
    if (!snap) return result;

    struct Spec { QString name; bool isFlag; int id; };
    static const QVector<Spec> specs = {
        // Counters
        {"SLC", false, SLC}, {"SEG", false, SEG}, {"REP", false, REP}, {"AVG", false, AVG},
        {"SET", false, SET}, {"ECO", false, ECO}, {"PHS", false, PHS}, {"LIN", false, LIN},
        {"PAR", false, PAR}, {"ACQ", false, ACQ}, {"ONCE", false, ONCE},
        // Flags
        {"NAV", true, NAV}, {"REV", true, REV}, {"SMS", true, SMS}, {"REF", true, REF},
        {"IMA", true, IMA}, {"OFF", true, OFF}, {"NOISE", true, NOISE},
        {"PMC", true, PMC}, {"NOROT", true, NOROT}, {"NOPOS", true, NOPOS}, {"NOSCL", true, NOSCL},
    };

    for (const auto& s : specs)
    {
        if (!Settings::getInstance().isExtensionLabelEnabled(s.name))
            continue;

        // SKIP if this label was never used in the sequence (avoid ghost labels like PHS=0)
        if (!m_usedExtensions.contains(s.name))
            continue;

        if (s.isFlag)
        {
            if (s.id < 0 || s.id >= snap->flags.size()) continue;
            if (snap->flags[s.id]) {
                result.append({s.name, 1});
            }
        }
        else
        {
            if (s.id < 0 || s.id >= snap->counters.size()) continue;
            int v = snap->counters[s.id];
            // Show all counters, even if 0, to match user expectation of "current state"
            result.append({s.name, v});
        }
    }
    // Sort alphabetically by name
    std::sort(result.begin(), result.end(), [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
        return a.first < b.first;
    });
    return result;
}
