#ifndef PULSEQLOADER_H
#define PULSEQLOADER_H

#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>
#include <memory>
#include <tuple>
#include <QHash>
#include <limits>
#include <QSet>
#include <atomic>
#include <cstdint>

#include "ExternalSequence.h" // For ExternalSequence factory and SeqBlock
#include "KSpaceTrajectory.h"
#include "PnsCalculator.h"
#include "M1Calculator.h"
#include "Settings.h"

// Forward declarations
class MainWindow;
class EventBlockInfoDialog;
class PulseqLoadTransaction;
class PulseqLoadUiAdapter;
class PulseqOpenController;

class PulseqLoader : public QObject
{
    Q_OBJECT

public:
    enum class RfSourceType {
        SingleChannel,
        RfShim,
        RoosPtxHack
    };
    enum class SequenceLoadState {
        Blank,
        Loading,
        Loaded
    };
    enum class TrajectoryState {
        NotStarted,
        Calculating,
        Ready,
        Failed
    };
    enum class PnsState {
        NotStarted,
        Calculating,
        Ready,
        Failed
    };
    enum class M1State {
        NotStarted,
        Calculating,
        Ready,
        Failed
    };
    struct SafetyMetric
    {
        bool configured {false};
        double measured {0.0};
        double limit {0.0};
        bool passed {true};
        bool hasLocation {false};
        int blockIndex {-1};
        double timeUs {std::numeric_limits<double>::quiet_NaN()};
        double blockStartUs {std::numeric_limits<double>::quiet_NaN()};
        double blockEndUs {std::numeric_limits<double>::quiet_NaN()};
        QString channel;
    };
    struct SafetyResult
    {
        QString profileAlias;
        SafetyMetric maxGrad;
        SafetyMetric maxSlew;
        SafetyMetric maxB1;
        bool hasAnyChecks {false};
        bool hasViolation {false};
        QString summary;
        QString warningMessage;
    };
    struct UnifiedRfChannel
    {
        int channelIndex {0};
        RfSourceType source {RfSourceType::SingleChannel};
        double amplitudeScale {1.0};
        double phaseOffsetRad {0.0};
        double freqOffsetHz {0.0};
        bool phaseIsRealLike {false};
        QVector<float> ampNorm;
        QVector<float> phaseNorm;
    };
    struct UnifiedRfBlock
    {
        int blockIndex {-1};
        int rfLength {0};
        double startTimeAxis {0.0};
        double dwellAxis {0.0};
        QVector<UnifiedRfChannel> channels;
    };
    struct UnifiedRfViewport
    {
        QVector<QVector<double>> ampTimeByChannel;
        QVector<QVector<double>> ampValueByChannel;
        QVector<QVector<double>> phaseTimeByChannel;
        QVector<QVector<double>> phaseValueByChannel;
    };
    struct LoadError
    {
        QString title;
        QString message;
    };

    explicit PulseqLoader(MainWindow* mainWindow);
    ~PulseqLoader();

    // Public API for other classes
    bool OpenPulseqFilePath(QString candidatePath);
    bool LoadPulseqFile(QString sPulseqFilePath);
    void setBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock);
    void setRawBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock);

    // Extension label snapshots (current values after a block).
    bool getCounterValueAfterBlock(int blockIdx, int counterId, int& outVal) const;
    bool getFlagValueAfterBlock(int blockIdx, int flagId, bool& outVal) const;
    bool getExtensionValueAfterBlock(int blockIdx, const QString& name, int& outVal, bool& isFlag) const;
    QStringList getAvailableExtensionLabels() const;
    QSet<QString> getUsedExtensions() const { return m_usedExtensions; }
    // Get all active labels (counters/flags) with their current values for a block
    QList<QPair<QString, int>> getActiveLabels(int blockIdx) const;
    QStringList getActiveLabelLines(int blockIdx) const;

    // Getters for data needed by other handlers
    const QVector<double>& getBlockEdges() const { return vecBlockEdges; }
    const QString& getTimeUnits() const { return TimeUnits; }
    double getTotalDuration_us() const { return m_dTotalDuration_us; }
    const std::vector<SeqBlock*>& getDecodedSeqBlocks() const { return m_vecDecodeSeqBlocks; }
    int getBlockRangeStart() const { return nBlockRangeStart; }
    int getBlockRangeEnd() const { return nBlockRangeEnd; }
    void setBlockRange(int start, int end) { nBlockRangeStart = start; nBlockRangeEnd = end; }
    const std::vector<int>& getTrBlockIndices() const { return m_vecTrBlockIndices; }
    bool hasRepetitionTime() const { return m_bHasRepetitionTime; }
    double getRepetitionTime_us() const { return m_dRepetitionTime_us; }
    int getTrCount() const { return m_nTrCount; }
    double getTFactor() const { return tFactor; }
    void setPulseqFilePathCache(const QString& path) { m_sPulseqFilePathCache = path; }
    const QString& getLoadedPulseqFilePath() const { return m_sPulseqFilePath; }
    const QString& getReopenPulseqFilePath() const { return m_sPulseqFilePathCache; }
    std::shared_ptr<ExternalSequence> getSequence(){ return m_spPulseqSeq; }
    SequenceLoadState getSequenceLoadState() const { return m_sequenceLoadState; }
    bool isSequenceLoading() const { return m_sequenceLoadState == SequenceLoadState::Loading; }
    bool hasLoadedSequence() const { return m_sequenceLoadState == SequenceLoadState::Loaded; }
    bool canRenderSequence() const { return m_sequenceLoadState == SequenceLoadState::Loaded; }

    // Merged series getters (load-time built)
    const QVector<double>& getRfTimeAmp() const { return m_rfTimeAmp; }
    const QVector<double>& getRfAmp() const { return m_rfAmp; }
    const QVector<double>& getRfTimePh() const { return m_rfTimePh; }
    const QVector<double>& getRfPh() const { return m_rfPh; }
    
    // Gradient merged series getters
    const QVector<double>& getGxTime() const { return m_gxTime; }
    const QVector<double>& getGxValues() const { return m_gxValues; }
    const QVector<double>& getGyTime() const { return m_gyTime; }
    const QVector<double>& getGyValues() const { return m_gyValues; }
    const QVector<double>& getGzTime() const { return m_gzTime; }
    const QVector<double>& getGzValues() const { return m_gzValues; }
    
    // ADC merged series getters
    const QVector<double>& getAdcTime() const { return m_adcTime; }
    const QVector<double>& getAdcValues() const { return m_adcValues; }

    void setManualRepetitionTime(double trValue);

    // Version reading functionality
    static std::pair<int, int> ReadFileVersion(const std::string& filename);

    // Test/CLI: suppress GUI dialogs during load failures
    void setSilentMode(bool silent) { m_silentMode = silent; }
    bool isSilentMode() const { return m_silentMode; }

    // RF on-demand rendering API (Phase 1)
    // Build viewport RF amplitude/phase series using per-shape cache and per-block scaling.
    void getRfViewportDecimated(double visibleStart, double visibleEnd, int pixelWidth,
                                QVector<double>& tAmp, QVector<double>& vAmp,
                                QVector<double>& tPh, QVector<double>& vPh);
    void getUnifiedRfViewport(double visibleStart, double visibleEnd, int pixelWidth,
                              UnifiedRfViewport& viewport);
    int getUnifiedRfChannelCount() const { return m_unifiedRfChannelCount; }
    bool hasDetectedRoosPtxHack() const { return m_detectedRoosPtxHack; }
    QString getUnifiedRfStatusMessage() const { return m_unifiedRfStatusMessage; }

    // Global RF ranges without materializing merged arrays
    QPair<double,double> getRfGlobalRangeAmp();
    QPair<double,double> getRfGlobalRangePh();

    // ADC phase on-demand rendering (MATLAB-matching formula)
    void getAdcPhaseViewport(double visibleStart, double visibleEnd, int pixelWidth,
                             QVector<double>& tOut, QVector<double>& vOut);

    // ADC phase viewport cache (invalidated on sequence reload)
    struct AdcPhaseCache {
        double visibleStart {0.0};
        double visibleEnd {0.0};
        int pixelWidth {0};
        QVector<double> tData;
        QVector<double> vData;
        bool valid {false};
    };
    mutable AdcPhaseCache m_adcPhaseCache;

    // B0 accessor (from sequence [DEFINITIONS])
    double getB0Tesla() const { return m_b0Tesla; }

    // Phase 2: Gradient on-demand rendering API
    void getGradViewportDecimated(int channel, double visibleStart, double visibleEnd, int pixelWidth,
                                  QVector<double>& tOut, QVector<double>& vOut);
    QPair<double,double> getGradGlobalRange(int channel);

    // Precise single-point sampling APIs (for status bar, no merged arrays)
    // time: internal units (already multiplied by tFactor). blockIdx: index of block containing time
    // Returns true if a value is defined at the given time within the specified block.
    bool sampleRFAtTime(double time, int blockIdx, double& ampHzOut, double& phaseRadOut) const;
    bool sampleGradAtTime(int channel, double time, int blockIdx, double& gradOutHzPerM) const;

    // Sequence version string (e.g., "v1.4.1") for status display
    const QString& getPulseqVersionString() const { return m_pulseqVersionString; }

    // Echo-time / excitation overlay helpers
    bool hasEchoTimeDefinition() const { return m_hasEchoTimeDefinition; }
    double getTeDurationAxis() const { return m_hasEchoTimeDefinition ? m_teDurationAxis : 0.0; }
    const QVector<double>& getTeDurationsAxis() const { return m_teDurationsAxis; }
    bool supportsExcitationMetadata() const { return m_supportsRfUseMetadata; }
    const QVector<double>& getExcitationCenters() const { return m_excitationCentersAxis; }
    const QVector<double>& getRefocusingCenters() const { return m_refocusingCentersAxis; }
    QVector<double> getKxKyZeroTimes() const; // Returns times when kx=ky=0 (in axis units)

    void ensureTrajectoryPrepared();
    bool waitForBackgroundComputations(int timeoutMs = 60000);
    const QVector<double>& getTrajectoryKx() const { return m_kTrajectoryX; }
    const QVector<double>& getTrajectoryKy() const { return m_kTrajectoryY; }
    const QVector<double>& getTrajectoryKz() const { return m_kTrajectoryZ; }
    const QVector<double>& getTrajectoryTimeSec() const { return m_kTimeSec; }
    const QVector<double>& getTrajectoryKxAdc() const { return m_kTrajectoryXAdc; }
    const QVector<double>& getTrajectoryKyAdc() const { return m_kTrajectoryYAdc; }
    const QVector<double>& getTrajectoryKzAdc() const { return m_kTrajectoryZAdc; }
    const QVector<double>& getTrajectoryTimeAdcSec() const { return m_kTimeAdcSec; }
    bool hasTrajectoryData() const { return m_kTrajectoryReady; }
    TrajectoryState getTrajectoryState() const { return m_trajectoryState; }
    bool isTrajectoryCalculating() const { return m_trajectoryState == TrajectoryState::Calculating; }
    bool shouldAutoStartTrajectoryAfterLoad() const { return m_autoStartTrajectoryAfterLoad; }
    void setAutoStartTrajectoryAfterLoad(bool enabled) { m_autoStartTrajectoryAfterLoad = enabled; }
    bool needsRfUseGuessWarning() const { return m_rfUseGuessed && !m_warnedRfUseGuess; }
    void markRfUseGuessWarningShown() { m_warnedRfUseGuess = true; }
    QString getRfUseGuessWarning() const { return m_rfGuessWarning; }
    // RF use per block (filled after trajectory compute)
    char getRfUseForBlock(int blockIdx) const {
        if (blockIdx < 0 || blockIdx >= m_rfUsePerBlock.size()) return 'u';
        char c = m_rfUsePerBlock[blockIdx];
        return c ? c : 'u';
    }
    // PNS
    bool hasPnsData() const { return m_pnsResult.valid; }
    PnsState getPnsState() const { return m_pnsState; }
    bool isPnsCalculating() const { return m_pnsState == PnsState::Calculating; }
    bool shouldAutoStartPnsAfterLoad() const { return m_autoStartPnsAfterLoad; }
    void setAutoStartPnsAfterLoad(bool enabled) { m_autoStartPnsAfterLoad = enabled; }
    bool isPnsOk() const { return m_pnsResult.ok; }
    QString getPnsAscPath() const { return m_pnsAscPath; }
    QString getPnsStatusMessage() const { return m_pnsStatusMessage; }
    const SafetyResult& getSafetyResult() const { return m_safetyResult; }

    // M1 (first gradient moment) accessors
    bool hasM1Data() const { return m_m1Result.valid; }
    M1State getM1State() const { return m_m1State; }
    bool isM1Calculating() const { return m_m1State == M1State::Calculating; }
    const M1Calculator::Result& getM1Result() const { return m_m1Result; }
    QStringList getM1Warnings() const { return m_m1Result.warnings; }
    // Convenience accessors so WaveformDrawer::DrawGWaveform can mirror the PNS
    // data path (load -> downsample on viewport change -> setData) without
    // reaching into the M1Calculator::Result struct directly.
    const QVector<double>& getM1TimeSec() const { return m_m1Result.tSec; }
    const QVector<double>& getM1X() const { return m_m1Result.m1x; }
    const QVector<double>& getM1Y() const { return m_m1Result.m1y; }
    const QVector<double>& getM1Z() const { return m_m1Result.m1z; }
    const QVector<double>& getPnsTimeSec() const { return m_pnsResult.timeSec; }
    const QVector<double>& getPnsX() const { return m_pnsResult.pnsX; }
    const QVector<double>& getPnsY() const { return m_pnsResult.pnsY; }
    const QVector<double>& getPnsZ() const { return m_pnsResult.pnsZ; }
    const QVector<double>& getPnsNorm() const { return m_pnsResult.pnsNorm; }
    void recomputePnsFromSettings();

public slots:
    // Slots for UI connections
    void OpenPulseqFile();
    void ReOpenPulseqFile();
    // Lightweight time-unit rescaling (avoids full file reload)
    void rescaleTimeUnit();

signals:
    void pnsDataUpdated();
    void pnsStateChanged();
    void m1DataUpdated();
    void m1StateChanged();
    void trajectoryStateChanged();
    void trajectoryDataUpdated();

private:
    friend class PulseqLoadTransaction;
    friend class PulseqOpenController;

    struct LabelSnapshot
    {
        QVector<int>  counters; // size NUM_LABELS (known counters only)
        QVector<bool> flags;    // size NUM_FLAGS (known flags only)
        QHash<QString, int> customCounters; // unknown/custom counters keyed by upper-case name
    };

    void buildLabelSnapshotCache();
    void parseTridIdNamesDefinition();
    const LabelSnapshot* labelSnapshotAfterBlock(int blockIdx) const;
    QString formatExtensionLabelLine(const QString& label, int value, bool isFlag) const;

    void buildShapeScaleAggregates();
    struct RoosPtxDetectionResult {
        bool detected {false};
        int matchedRfGroupCount {0};
        int matchedAdcGroupCount {0};
        int uniqueMatchedPhaseCount {0};
        int matchedBlockPairs {0};
        int inferredChannelCount {1};
        int inferredSamplesPerChannel {0};
    };
    bool buildUnifiedRfBlocks(QString* errorMessage = nullptr);
    RoosPtxDetectionResult detectRoosPtxHackPattern() const;
    void appendUnifiedRfBlockSeries(const UnifiedRfBlock& block,
                                    int pixelWidth,
                                    double window,
                                    bool allowDecimate,
                                    UnifiedRfViewport& viewport) const;
    bool appendUnifiedRfChannelSeries(const UnifiedRfBlock& block,
                                      const UnifiedRfChannel& channel,
                                      int pixelWidth,
                                      double window,
                                      bool allowDecimate,
                                      QVector<double>& tAmp,
                                      QVector<double>& vAmp,
                                      QVector<double>& tPh,
                                      QVector<double>& vPh) const;
    void applyRfPhaseOffsets(const UnifiedRfBlock& block,
                             const UnifiedRfChannel& channel,
                             QVector<double>& tPh,
                             QVector<double>& vPh) const;
    QString rfSourceTypeToString(RfSourceType type) const;
    void beginLoad();
    bool readAndCreateVersionedLoader(const QString& path, LoadError* error);
    bool loadParserFile(const QString& path, LoadError* error);
    bool validateRequiredDefinitions(LoadError* error) const;
    bool decodeBlocks(LoadError* error);
    bool buildLoadedWaveformCaches(LoadError* error);
    QPair<double, double> configureInitialViewport();
    void updateRepetitionTimeMetadata();
    void finishSuccessfulLoad(const QString& path, const QPair<double, double>& initialRange);
    bool failLoad(const LoadError& error);
    void ClearPulseqCache(bool withUi = true);
    bool IsBlockRf(const float* fAmp, const float* fPhase, const int& iSamples);
    void updateEchoAndExcitationMetadata(int versionMajor, int versionMinor);
    void computeKSpaceTrajectory();
    KSpaceTrajectory::Input buildKSpaceTrajectoryInput() const;
    void applyTrajectoryResult(const KSpaceTrajectory::Result& result);
    void setTrajectoryState(TrajectoryState state);
    void startTrajectoryComputationAsync();
    void startTrajectoryComputationIfEnabled();
    // M1 (first gradient moment) async computation. Mirrors the trajectory
    // pattern: launched off the main thread on sequence load, result cached
    // and pushed to the WaveformDrawer via applyM1Result.
    void startM1ComputationAsync();
    void applyM1Result(const M1Calculator::Result& result);
    void setM1State(M1State state);
    void setPnsState(PnsState state);
    void computeSafetyAnalysis(bool showWarningDialog);
    void computePnsSynchronously();
    void startPnsComputationAsync();
    void startPnsComputationIfEnabled();
    void markPnsDirty();
    bool shouldRecomputePns() const;
    void updateTimeUnitFromSettings();

    // Settings management
    void saveLastOpenDirectory();
    void loadLastOpenDirectory();
    void loadRecentFiles();
    void saveRecentFiles();
    void addRecentFile(const QString& filePath);
    void updateRecentFilesMenu();
    void clearRecentFiles();

private:
    MainWindow* m_mainWindow;
    std::unique_ptr<PulseqLoadUiAdapter> m_loadUi;
    std::unique_ptr<PulseqOpenController> m_openController;

    // Member variables moved from MainWindow
    QString m_sPulseqFilePath;
    QString m_sPulseqFilePathCache;
    QString m_sLastOpenDirectory;  // Remember last opened directory
    QStringList m_listRecentPulseqFilePaths;
    std::shared_ptr<ExternalSequence> m_spPulseqSeq;
    std::vector<SeqBlock*> m_vecDecodeSeqBlocks;
    std::vector<int> m_vecTrBlockIndices;
    double m_dTotalDuration_us;
    SequenceLoadState m_sequenceLoadState {SequenceLoadState::Blank};

    // TR detection and navigation
    bool m_bHasRepetitionTime;
    double m_dRepetitionTime_us;
    int m_nTrCount;

    // Block range for display
    int nBlockRangeStart;
    int nBlockRangeEnd;

    // Time unit handling
    QString TimeUnits;
    double tFactor;

    // Block Edges
    QVector<double> vecBlockEdges;

    // Merged series storage
    QVector<double> m_rfTimeAmp, m_rfAmp;
    QVector<double> m_rfTimePh, m_rfPh;
    QVector<UnifiedRfBlock> m_unifiedRfBlocks;
    int m_unifiedRfChannelCount {1};
    bool m_detectedRoosPtxHack {false};
    QString m_unifiedRfStatusMessage;
    
    // Gradient merged series storage
    QVector<double> m_gxTime, m_gxValues;
    QVector<double> m_gyTime, m_gyValues;
    QVector<double> m_gzTime, m_gzValues;
    
    // ADC merged series storage
    QVector<double> m_adcTime, m_adcValues;

    // Cached pulseq version like "v1.4.1"
    QString m_pulseqVersionString;

    // Cached extension label values after each block (for Information window)
    QVector<LabelSnapshot> m_labelSnapshots;
    QSet<QString> m_usedExtensions;
    QStringList m_tridIdNames;
    // Precomputed maximum accumulated counter value across all blocks.
    // Computed once in buildLabelSnapshotCache; avoids per-frame scanning loops.
    int m_maxAccumulatedCounter {0};
public:
    int getMaxAccumulatedCounter() const { return m_maxAccumulatedCounter; }
private:

    // Test/CLI behavior
    bool m_silentMode {false};

    // B0 field strength from [DEFINITIONS] (Tesla); needed for PPM phase terms
    double m_b0Tesla {0.0};

    // Echo-time / excitation overlay cache
    bool m_supportsRfUseMetadata {false};
    bool m_hasEchoTimeDefinition {false};
    double m_teTime_us {0.0};
    double m_teDurationAxis {0.0};
    QVector<double> m_teDurationsAxis;
    QVector<double> m_excitationCentersAxis;
    QVector<double> m_refocusingCentersAxis;
    bool m_rfUseGuessed {false};
    bool m_warnedRfUseGuess {false};
    QString m_rfGuessWarning;

    bool m_kTrajectoryReady {false};
    TrajectoryState m_trajectoryState {TrajectoryState::NotStarted};
    bool m_autoStartTrajectoryAfterLoad {true};
    std::uint64_t m_trajectorySequenceGeneration {0};
    std::uint64_t m_trajectoryRequestSerial {0};
    std::uint64_t m_activeTrajectoryRequestId {0};
    QVector<double> m_kTrajectoryX;
    QVector<double> m_kTrajectoryY;
    QVector<double> m_kTrajectoryZ;
    QVector<double> m_kTimeSec;
    QVector<double> m_kTrajectoryXAdc;
    QVector<double> m_kTrajectoryYAdc;
    QVector<double> m_kTrajectoryZAdc;
    QVector<double> m_kTimeAdcSec;
    QVector<char>   m_rfUsePerBlock;
    PnsCalculator::Result m_pnsResult;
    PnsState m_pnsState {PnsState::NotStarted};
    // M1 (first gradient moment) result + state. Mirrors the trajectory/PNS
    // caching pattern: populated asynchronously on sequence load, then
    // pushed to WaveformDrawer via applyM1Result.
    M1Calculator::Result m_m1Result;
    M1State m_m1State {M1State::NotStarted};
    std::uint64_t m_m1RequestSerial {0};
    bool m_autoStartPnsAfterLoad {true};
    bool m_pnsDirty {true};
    std::uint64_t m_pnsRequestSerial {0};
    std::uint64_t m_activePnsRequestId {0};
    std::uint64_t m_lastPnsComputedSequenceGeneration {0};
    QString m_lastPnsComputedAscPath;
    double m_lastPnsComputedGammaHzPerT {0.0};
    QString m_pnsAscPath;
    QString m_pnsStatusMessage;
    SafetyResult m_safetyResult;

    // ===== RF Shape Cache (split Amp/Phase) =====
    struct RFAmpEntry {
        QVector<float> ampNorm; // normalized amplitude shape
        int length {0};
        double ampMin {0.0};
        double ampMax {0.0};
        int peakIndex {-1};
    };
    struct RFPhEntry {
        QVector<float> phNorm;  // phase samples
        int length {0};
        double phMin {0.0};
        double phMax {0.0};
        bool isRealLike {false};
    };
    QHash<QString, RFAmpEntry> m_rfAmpCache; // rfA:<magShapeId>:<timeShapeId>#<len>
    QHash<QString, RFPhEntry>  m_rfPhCache;  // rfP:<phaseShapeId>:<timeShapeId>#<len>
    QString rfAmpKey(int magShapeId, int timeShapeId, int len) const;
    QString rfPhKey(int phaseShapeId, int timeShapeId, int len) const;
    const RFAmpEntry& ensureRfAmpCached(const float* amp, int len, int magShapeId, int timeShapeId);
    const RFPhEntry&  ensureRfPhCached(const float* phase, int len, int phaseShapeId, int timeShapeId);
    void downsampleMinMax(const QVector<float>& src, int buckets, QVector<int>& outIdxMin, QVector<int>& outIdxMax) const;
    void lttbDownsampleUniform(const QVector<float>& src, double tStart, double dt, int targetPoints,
                               QVector<double>& tOut, QVector<double>& vOut) const;

    // Gradient shape cache for arbitrary gradients
    struct GradShapeEntry {
        QVector<float> norm; // normalized gradient shape
        int length {0};
        double vMin {0.0};
        double vMax {0.0};
    };
    QHash<QString, GradShapeEntry> m_gradShapeCache; // key: grad:<waveShapeId>:<timeShapeId>#<len>
    QString gradKey(int waveShapeId, int timeShapeId, int len) const;
    const GradShapeEntry& ensureGradCached(const float* shape, int len,
                                          int waveShapeId, int timeShapeId);

    // ===== Aggregated per-shape scale tracking (for global Y-range, computed once at load) =====
    struct ScaleAgg {
        double shapeMin {0.0};
        double shapeMax {0.0};
        double maxPosScale {0.0}; // maximum non-negative scale encountered
        double minNegScale {0.0}; // minimum (most negative) scale encountered
        bool   hasShape {false};
        inline void updateShape(double smin, double smax) {
            shapeMin = smin; shapeMax = smax; hasShape = true;
        }
        inline void updateScale(double s) {
            if (s >= 0) maxPosScale = std::max(maxPosScale, s);
            else        minNegScale = std::min(minNegScale, s);
        }
    };
    // RF amplitude aggregations (keyed by rfAmpKey)
    QHash<QString, ScaleAgg> m_rfAgg;
    // Gradient aggregations per channel (keyed by gradKey for arbitrary shapes)
    QHash<QString, ScaleAgg> m_gradAgg[3];
    // Trapezoid gradient per-channel scale extremes (no shape key)
    double m_gradTrapMaxPosScale[3] {0.0, 0.0, 0.0};
    double m_gradTrapMinNegScale[3] {0.0, 0.0, 0.0};
    // External trapezoid global min/max per channel (aggregated during load)
    double m_gradExtTrapGlobalMin[3] { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity() };
    double m_gradExtTrapGlobalMax[3] { -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };

    // Shared owner of the decoded SeqBlock* for the current sequence. Both the
    // loader and any in-flight async trajectory/PNS task hold a shared_ptr to
    // the same bundle; the blocks are deleted only when the LAST holder releases
    // it. This makes "reopen while a computation is still running" safe: the old
    // blocks survive until every reader has finished, instead of being freed out
    // from under an async task (the previous single-slot handoff could only
    // protect one of two concurrent tasks, causing use-after-free / nondeterministic
    // PNS results on reopen).
    struct BlockBundle {
        std::vector<SeqBlock*> blocks;
        ~BlockBundle()
        {
            for (SeqBlock* block : blocks)
            {
                delete block;
            }
        }
    };
    std::shared_ptr<BlockBundle> m_blockBundle;
};

#endif // PULSEQLOADER_H
