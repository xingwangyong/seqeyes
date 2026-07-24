#ifndef SETTINGS_H
#define SETTINGS_H

#include <QObject>
#include <QString>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>
#include <QStringList>
#include <QVector>

class Settings : public QObject
{
    Q_OBJECT

public:
    struct SystemProfile
    {
        QString alias;
        QString ascPath;
        double b0Tesla {3.0};
        double maxGrad {0.0}; // Stored in mT/m; NaN means unset
        double maxSlew {0.0}; // Stored in T/m/s; NaN means unset
        double maxB1 {0.0};   // Stored in uT; NaN means unset

        bool operator==(const SystemProfile& other) const
        {
            auto sameDouble = [](double a, double b) {
                const bool aNan = !(a == a);
                const bool bNan = !(b == b);
                return (aNan && bNan) || a == b;
            };
            return alias == other.alias &&
                   ascPath == other.ascPath &&
                   sameDouble(b0Tesla, other.b0Tesla) &&
                   sameDouble(maxGrad, other.maxGrad) &&
                   sameDouble(maxSlew, other.maxSlew) &&
                   sameDouble(maxB1, other.maxB1);
        }

        bool operator!=(const SystemProfile& other) const
        {
            return !(*this == other);
        }
    };

    // Mouse zoom input mode
    enum class ZoomInputMode {
        CtrlWheel,  // Ctrl + Mouse wheel for zoom
        Wheel       // Mouse wheel for zoom
    };

    // Gradient units
    enum class GradientUnit {
        HzPerM,      // Hz/m
        mTPerM,      // mT/m
        radPerMsPerMm, // rad/ms/mm
        GPerCm       // G/cm
    };

    // Slew units
    enum class SlewUnit {
        HzPerMPerS,     // Hz/m/s
        mTPerMPerMs,    // mT/m/ms
        TPerMPerS,      // T/m/s
        radPerMsPerMmPerMs, // rad/ms/mm/ms
        GPerCmPerMs,    // G/cm/ms
        GPerCmPerS      // G/cm/s
    };

    // Time units for UI/plot display
    enum class TimeUnit {
        Milliseconds,
        Microseconds
    };

    // Trajectory display units for k-space
    enum class TrajectoryUnit {
        PerM = 0,   // 1/m
        RadPerM,    // rad/m
        InvFov      // 1/FOV (dimensionless)
    };

    // Trajectory colormap for ADC samples in trajectory view
    enum class TrajectoryColormap {
        Jet = 0,
        Cividis,
        Plasma
    };

    // Log levels (aligned with Qt: qCritical, qWarning, qInfo, qDebug)
    enum class LogLevel {
        Fatal    = 0,   // Abort after logging
        Critical = 1,   // Non-fatal errors
        Warning  = 2,   // Warnings
        Info     = 3,   // Informational
        Debug    = 4    // Verbose
    };

    static Settings& getInstance();
    
    // Input behavior settings
    void setZoomInputMode(ZoomInputMode mode);
    ZoomInputMode getZoomInputMode() const;
    QString getZoomInputModeString() const;

    void setPanWheelEnabled(bool enabled);
    bool getPanWheelEnabled() const;

    void setAutoReloadOnFileChange(bool enabled);
    bool getAutoReloadOnFileChange() const;

    // Keyboard shortcuts (pan / TR stepping)
    void setPanLeftKey(const QString& key);
    void setPanRightKey(const QString& key);
    QString getPanLeftKey() const;
    QString getPanRightKey() const;

    // Gradient unit settings
    void setGradientUnit(GradientUnit unit);
    GradientUnit getGradientUnit() const;
    QString getGradientUnitString() const;
    
    // Slew unit settings
    void setSlewUnit(SlewUnit unit);
   SlewUnit getSlewUnit() const;
   QString getSlewUnitString() const;

    void setTimeUnit(TimeUnit unit);
    TimeUnit getTimeUnit() const;
    QString getTimeUnitString() const;

    // Trajectory unit settings
    void setTrajectoryUnit(TrajectoryUnit unit);
    TrajectoryUnit getTrajectoryUnit() const;
    QString getTrajectoryUnitString() const;

    // Trajectory colormap settings
    void setTrajectoryColormap(TrajectoryColormap map);
    TrajectoryColormap getTrajectoryColormap() const;
    QString getTrajectoryColormapString() const;
    
    // Gamma parameter (magnetic gyromagnetic ratio)
    void setGamma(double gamma);
    double getGamma() const;
    
    // Log level settings
    void setLogLevel(LogLevel level);
    LogLevel getLogLevel() const;
    QString getLogLevelString() const;
    
    // Approximate overlay warning dialogs (legacy sequence behavior)
    void setShowTeApproximateDialog(bool show);
    bool getShowTeApproximateDialog() const;
    void setShowTrajectoryApproximateDialog(bool show);
    bool getShowTrajectoryApproximateDialog() const;
    void setEnableRoosPtxHackAutoDetection(bool enable);
    bool getEnableRoosPtxHackAutoDetection() const;

    // Performance diagnostics
    void setPerformanceDebugEnabled(bool enabled);
    bool getPerformanceDebugEnabled() const;
    
    // Configuration file locations
    QString getConfigDirPath() const;
    QString getSettingsFilePath() const;
    
    // Unit conversion functions
    double convertGradient(double value, const QString& fromUnit, const QString& toUnit) const;
    double convertSlew(double value, const QString& fromUnit, const QString& toUnit) const;
    
    // Old time-based LOD settings removed - replaced with complexity-based LOD system

    // Settings persistence
    void saveSettings();
    void loadSettings();
    void resetToDefaults();
    
    // Get all available units
    static QStringList getAvailableGradientUnits();
    static QStringList getAvailableSlewUnits();
    static QStringList getAvailableTimeUnits();
    static QStringList getAvailableTrajectoryUnits();

    // Extension label support
    static QStringList getSupportedExtensionLabels();
    void setExtensionLabelEnabled(const QString& label, bool enabled);
    bool isExtensionLabelEnabled(const QString& label) const;

    // Extension tooltip setting
    void setShowExtensionTooltip(bool show);
    bool getShowExtensionTooltip() const;

    // System safety profiles
    QVector<SystemProfile> getSystemProfiles() const;
    QString globalSystemProfileAlias() const;
    SystemProfile globalSystemProfile() const;
    void setSystemProfiles(const QVector<SystemProfile>& profiles);
    void setGlobalSystemProfileAlias(const QString& alias);
    QString generateNextSystemProfileAlias() const;

    // Legacy ASC/PNS compatibility accessors

    QStringList getPnsAscHistory() const;
    QString getPnsAscNickname(const QString& path) const;
    QMap<QString, QString> getPnsAscNicknames() const;
    void setPnsAscPath(const QString& path);
    void setPnsAscHistory(const QStringList& history);
    void setPnsAscNickname(const QString& path, const QString& nickname);
    bool removePnsAscHistoryPath(const QString& path);
    int removeInvalidPnsAscHistoryPaths();
    void setPnsChannelVisibleX(bool visible);
    void setPnsChannelVisibleY(bool visible);
    void setPnsChannelVisibleZ(bool visible);
    void setPnsChannelVisibleNorm(bool visible);
    bool getPnsChannelVisibleX() const;
    bool getPnsChannelVisibleY() const;
    bool getPnsChannelVisibleZ() const;
    bool getPnsChannelVisibleNorm() const;

signals:
    void settingsChanged();
    void timeUnitChanged();

private:
    explicit Settings(QObject* parent = nullptr);
    ~Settings() = default;
    
    // Disable copy constructor and assignment operator
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    
    QSettings* m_qSettings;
    QString m_jsonFilePath;
    
    // Current settings
    ZoomInputMode m_zoomInputMode;
    bool m_panWheelEnabled;
    bool m_autoReloadOnFileChange {false};
    QString m_panLeftKey;
    QString m_panRightKey;
    GradientUnit m_gradientUnit;
    SlewUnit m_slewUnit;
    TimeUnit m_timeUnit;
    TrajectoryUnit m_trajectoryUnit;
    TrajectoryColormap m_trajectoryColormap;
    double m_gamma; // Hz/T
    LogLevel m_logLevel; // Log level setting
    bool m_showTeApproximateDialog { true }; // Show TE approximate warning for legacy sequences
    bool m_showTrajectoryApproximateDialog { true }; // Show trajectory warning for legacy sequences
    bool m_showExtensionTooltip { false }; // Show extension tooltip on hover
    bool m_enableRoosPtxHackAutoDetection { true };
    bool m_performanceDebugEnabled { false };
    bool m_pnsShowX {false};
    bool m_pnsShowY {false};
    bool m_pnsShowZ {true};
    bool m_pnsShowNorm {true};
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    
    // Conversion helper functions
    double convertToStandardGradient(double value, const QString& fromUnit) const;
    double convertFromStandardGradient(double value, const QString& toUnit) const;
    double convertToStandardSlew(double value, const QString& fromUnit) const;
    double convertFromStandardSlew(double value, const QString& toUnit) const;
    
    // String to enum conversion helpers
    GradientUnit stringToGradientUnit(const QString& unitString) const;
    SlewUnit stringToSlewUnit(const QString& unitString) const;
    ZoomInputMode stringToZoomInputMode(const QString& s) const;
    TimeUnit stringToTimeUnit(const QString& unitString) const;
    TrajectoryUnit stringToTrajectoryUnit(const QString& unitString) const;
    TrajectoryColormap stringToTrajectoryColormap(const QString& name) const;

    // Extension label helpers
    void initDefaultExtensionLabels();
    QString normalizeSystemAlias(const QString& alias) const;
    QString makeUniqueSystemAlias(const QString& preferredAlias, const QString& currentAlias = QString()) const;
    void sanitizeSystemProfiles();
    void migrateLegacyPnsSettings(const QJsonObject& obj);
    static bool isLimitConfigured(double value);
    QMap<QString, bool> m_extensionLabelStates;
    QVector<SystemProfile> m_systemProfiles;
    QString m_activeSystemProfileAlias;
};

#endif // SETTINGS_H
