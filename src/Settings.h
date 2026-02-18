#ifndef SETTINGS_H
#define SETTINGS_H

#include <QObject>
#include <QString>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMap>

class Settings : public QObject
{
    Q_OBJECT

public:
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
    
    // Known issues dialog (startup)
    void setShowKnownIssuesDialog(bool show);
    bool getShowKnownIssuesDialog() const;

    // Approximate overlay warning dialogs (legacy sequence behavior)
    void setShowTeApproximateDialog(bool show);
    bool getShowTeApproximateDialog() const;
    void setShowTrajectoryApproximateDialog(bool show);
    bool getShowTrajectoryApproximateDialog() const;
    
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
    QString m_panLeftKey;
    QString m_panRightKey;
    GradientUnit m_gradientUnit;
    SlewUnit m_slewUnit;
    TimeUnit m_timeUnit;
    TrajectoryUnit m_trajectoryUnit;
    TrajectoryColormap m_trajectoryColormap;
    double m_gamma; // Hz/T
    LogLevel m_logLevel; // Log level setting
    bool m_showKnownIssuesDialog { true }; // Show known-issues dialog on startup
    bool m_showTeApproximateDialog { true }; // Show TE approximate warning for legacy sequences
    bool m_showTrajectoryApproximateDialog { true }; // Show trajectory warning for legacy sequences
    bool m_showExtensionTooltip { false }; // Show extension tooltip on hover
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
    QMap<QString, bool> m_extensionLabelStates;
};

#endif // SETTINGS_H
