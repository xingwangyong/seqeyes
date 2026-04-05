#ifndef ZOOMMANAGER_H
#define ZOOMMANAGER_H

#include <QObject>
#include <QString>

class ZoomManager : public QObject
{
    Q_OBJECT

public:
    enum ZoomLevel { Level0, Level1, Level2 };

public:
    explicit ZoomManager(QObject* parent = nullptr);

    // Load config from JSON file; return true if loaded
    bool loadConfig(const QString& configPath);

    // Determine zoom level by viewport duration in milliseconds (legacy support)
    ZoomLevel getZoomLevel(double viewportDurationMs) const;

    // Accessors for complexity-based LOD thresholds
    int getComplexCurveThresholdLow() const { return m_complexCurveThresholdLow; }
    int getComplexCurveThresholdHigh() const { return m_complexCurveThresholdHigh; }
    double getLargeViewportThresholdMs() const { return m_largeViewportThresholdMs; }


    // Accessors for downsample factors
    int getDownsampleFactorFullDetail() const { return m_downsampleFactorFullDetail; }
    int getDownsampleFactorMedium() const { return m_downsampleFactorMedium; }
    int getDownsampleFactorHigh() const { return m_downsampleFactorHigh; }

    // Cache configuration
    int getMaxCacheEntries() const { return m_maxCacheEntries; }
    int getCacheCleanupThreshold() const { return m_cacheCleanupThreshold; }

private:
    // Complexity-based LOD thresholds
    int m_complexCurveThresholdLow;    // < 100 complex curves: FULL_DETAIL
    int m_complexCurveThresholdHigh;   // < 1000 complex curves: DOWNSAMPLED_2X
    double m_largeViewportThresholdMs; // Very large viewport threshold (ms)

    // Downsample factors for different LOD levels
    int m_downsampleFactorFullDetail;  // 1 (no downsampling)
    int m_downsampleFactorMedium;      // 2 (2x downsampling)
    int m_downsampleFactorHigh;       // 100 (100x downsampling)

    // Cache configuration
    int m_maxCacheEntries;        // Maximum number of cached entries
    int m_cacheCleanupThreshold;   // Cache cleanup threshold
};

#endif // ZOOMMANAGER_H


