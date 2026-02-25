#ifndef PULSEQLOADER_H
#define PULSEQLOADER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>
#include <memory>
#include <tuple>
#include <QHash>
#include <limits>
#include <QSet>

#include "ExternalSequence.h" // For ExternalSequence factory and SeqBlock

// Forward declarations
class MainWindow;
class EventBlockInfoDialog;

class PulseqLoader : public QObject
{
    Q_OBJECT

public:
    explicit PulseqLoader(MainWindow* mainWindow);
    ~PulseqLoader();

    // Public API for other classes
    bool LoadPulseqFile(const QString& sPulseqFilePath);
    void setBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock);
    void setRawBlockInfoContent(EventBlockInfoDialog* dialog, int currentBlock);

    // Extension label snapshots (current values after a block).
    bool getCounterValueAfterBlock(int blockIdx, int counterId, int& outVal) const;
    bool getFlagValueAfterBlock(int blockIdx, int flagId, bool& outVal) const;
    QSet<QString> getUsedExtensions() const { return m_usedExtensions; }
    // Get all active labels (counters/flags) with their current values for a block
    QList<QPair<QString, int>> getActiveLabels(int blockIdx) const;

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
    std::shared_ptr<ExternalSequence> getSequence(){ return m_spPulseqSeq; }

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
    bool supportsExcitationMetadata() const { return m_supportsRfUseMetadata; }
    const QVector<double>& getExcitationCenters() const { return m_excitationCentersAxis; }
    const QVector<double>& getRefocusingCenters() const { return m_refocusingCentersAxis; }
    QVector<double> getKxKyZeroTimes() const; // Returns times when kx=ky=0 (in axis units)

    void ensureTrajectoryPrepared();
    const QVector<double>& getTrajectoryKx() const { return m_kTrajectoryX; }
    const QVector<double>& getTrajectoryKy() const { return m_kTrajectoryY; }
    const QVector<double>& getTrajectoryKz() const { return m_kTrajectoryZ; }
    const QVector<double>& getTrajectoryTimeSec() const { return m_kTimeSec; }
    const QVector<double>& getTrajectoryKxAdc() const { return m_kTrajectoryXAdc; }
    const QVector<double>& getTrajectoryKyAdc() const { return m_kTrajectoryYAdc; }
    const QVector<double>& getTrajectoryKzAdc() const { return m_kTrajectoryZAdc; }
    const QVector<double>& getTrajectoryTimeAdcSec() const { return m_kTimeAdcSec; }
    bool hasTrajectoryData() const { return m_kTrajectoryReady; }
    bool needsRfUseGuessWarning() const { return m_rfUseGuessed && !m_warnedRfUseGuess; }
    void markRfUseGuessWarningShown() { m_warnedRfUseGuess = true; }
    QString getRfUseGuessWarning() const { return m_rfGuessWarning; }
    // RF use per block (filled after trajectory compute)
    char getRfUseForBlock(int blockIdx) const {
        if (blockIdx < 0 || blockIdx >= m_rfUsePerBlock.size()) return 'u';
        char c = m_rfUsePerBlock[blockIdx];
        return c ? c : 'u';
    }

public slots:
    // Slots for UI connections
    void OpenPulseqFile();
    void ReOpenPulseqFile();
    bool ClosePulseqFile();
    // Lightweight time-unit rescaling (avoids full file reload)
    void rescaleTimeUnit();

private:
    struct LabelSnapshot
    {
        QVector<int>  counters; // size NUM_LABELS (known counters only)
        QVector<bool> flags;    // size NUM_FLAGS (known flags only)
    };

    void buildLabelSnapshotCache();
    const LabelSnapshot* labelSnapshotAfterBlock(int blockIdx) const;

    void buildShapeScaleAggregates();
    void ClearPulseqCache();
    bool IsBlockRf(const float* fAmp, const float* fPhase, const int& iSamples);
    void updateEchoAndExcitationMetadata(int versionMajor, int versionMinor);
    void computeKSpaceTrajectory();
    void updateTimeUnitFromSettings();

    // Settings management
    void saveLastOpenDirectory();
    void loadLastOpenDirectory();

private:
    MainWindow* m_mainWindow;

    // Member variables moved from MainWindow
    QString m_sPulseqFilePath;
    QString m_sPulseqFilePathCache;
    QString m_sLastOpenDirectory;  // Remember last opened directory
    QStringList m_listRecentPulseqFilePaths;
    std::shared_ptr<ExternalSequence> m_spPulseqSeq;
    std::vector<SeqBlock*> m_vecDecodeSeqBlocks;
    std::vector<int> m_vecTrBlockIndices;
    double m_dTotalDuration_us;

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
    QVector<double> m_excitationCentersAxis;
    QVector<double> m_refocusingCentersAxis;
    bool m_rfUseGuessed {false};
    bool m_warnedRfUseGuess {false};
    QString m_rfGuessWarning;

    bool m_kTrajectoryReady {false};
    QVector<double> m_kTrajectoryX;
    QVector<double> m_kTrajectoryY;
    QVector<double> m_kTrajectoryZ;
    QVector<double> m_kTimeSec;
    QVector<double> m_kTrajectoryXAdc;
    QVector<double> m_kTrajectoryYAdc;
    QVector<double> m_kTrajectoryZAdc;
    QVector<double> m_kTimeAdcSec;
    QVector<char>   m_rfUsePerBlock;

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
};

#endif // PULSEQLOADER_H
