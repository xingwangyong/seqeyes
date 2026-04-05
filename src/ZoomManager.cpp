#include "ZoomManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

/*
ZoomManager: provides configuration for complexity-based LOD (Level of Detail) system.
The system determines LOD levels based on the number of complex curves in the viewport
rather than viewport duration, providing more intelligent performance optimization.
*/

ZoomManager::ZoomManager(QObject* parent)
    : QObject(parent),
      // Complexity-based LOD thresholds
      m_complexCurveThresholdLow(100),        // < 100 complex curves: FULL_DETAIL
      m_complexCurveThresholdHigh(200),      // < 1000 complex curves: DOWNSAMPLED_2X
      m_largeViewportThresholdMs(1000000.0),   // Very large viewport threshold (1000s)
      
      // Downsample factors for different LOD levels
      m_downsampleFactorFullDetail(1),        // No downsampling
      m_downsampleFactorMedium(5),            // 2x downsampling
      m_downsampleFactorHigh(100),            // 100x downsampling
      
      // Cache configuration
      m_maxCacheEntries(1000),
      m_cacheCleanupThreshold(1000)
{
}

bool ZoomManager::loadConfig(const QString& configPath)
{
    QFile f(configPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray raw = f.readAll();
    QString text = QString::fromUtf8(raw);
    // Strip block comments /* ... */
    text.replace(QRegularExpression("/\\*[^*]*\\*+(?:[^/*][^*]*\\*+)*/"), "");
    // Strip line comments // ... (to end of line)
    text.replace(QRegularExpression("//[^\n\r]*"), "");
    auto doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return false;
    auto obj = doc.object();

    // Load complexity-based LOD thresholds
    if (obj.contains("complex_curve_threshold_low")) 
        m_complexCurveThresholdLow = obj.value("complex_curve_threshold_low").toInt(m_complexCurveThresholdLow);
    if (obj.contains("complex_curve_threshold_high")) 
        m_complexCurveThresholdHigh = obj.value("complex_curve_threshold_high").toInt(m_complexCurveThresholdHigh);
    if (obj.contains("large_viewport_threshold_ms")) 
        m_largeViewportThresholdMs = obj.value("large_viewport_threshold_ms").toDouble(m_largeViewportThresholdMs);

    // Load downsample factors
    if (obj.contains("downsample_factor_full_detail")) 
        m_downsampleFactorFullDetail = obj.value("downsample_factor_full_detail").toInt(m_downsampleFactorFullDetail);
    if (obj.contains("downsample_factor_medium")) 
        m_downsampleFactorMedium = obj.value("downsample_factor_medium").toInt(m_downsampleFactorMedium);
    if (obj.contains("downsample_factor_high")) 
        m_downsampleFactorHigh = obj.value("downsample_factor_high").toInt(m_downsampleFactorHigh);

    // Load cache configuration
    if (obj.contains("max_cache_entries")) 
        m_maxCacheEntries = obj.value("max_cache_entries").toInt(m_maxCacheEntries);
    if (obj.contains("cache_cleanup_threshold")) 
        m_cacheCleanupThreshold = obj.value("cache_cleanup_threshold").toInt(m_cacheCleanupThreshold);

    return true;
}

ZoomManager::ZoomLevel ZoomManager::getZoomLevel(double viewportDurationMs) const
{
    // Legacy method for backward compatibility
    // Note: This method is deprecated in favor of complexity-based LOD system
    // The new system uses determineOptimalLODLevel() based on complex curve count
    if (viewportDurationMs > 500.0) return Level0;  // Overview
    if (viewportDurationMs > 50.0) return Level1;   // Medium detail
    return Level2;  // Full detail
}


