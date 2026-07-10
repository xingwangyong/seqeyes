#include "Settings.h"
#include <QApplication>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <limits>

namespace {
constexpr int kMaxAscHistoryItems = 16;
constexpr double kUnsetLimit = std::numeric_limits<double>::quiet_NaN();
}

Settings& Settings::getInstance()
{
    static Settings instance;
    return instance;
}

Settings::Settings(QObject* parent)
    : QObject(parent)
    , m_qSettings(nullptr)
    , m_zoomInputMode(ZoomInputMode::Wheel)
    , m_panWheelEnabled(false)
    , m_gradientUnit(GradientUnit::mTPerM)
    , m_slewUnit(SlewUnit::TPerMPerS)
    , m_timeUnit(TimeUnit::Milliseconds)
    , m_trajectoryUnit(TrajectoryUnit::PerM)
    , m_trajectoryColormap(TrajectoryColormap::Jet)
    , m_gamma(42.576e6) // Hz/T for hydrogen
    , m_logLevel(LogLevel::Warning) // Default to Warning level
    , m_showExtensionTooltip(false)
    , m_enableRoosPtxHackAutoDetection(true)
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
{
    // Place settings in per-user home directory: ~/.seqeyes/settings.json
    m_jsonFilePath = getSettingsFilePath();
    // Ensure directory exists
    QDir dir; dir.mkpath(getConfigDirPath());
    
    // Initialize extension labels with defaults, then load settings to allow overrides
    initDefaultExtensionLabels();
    // Load settings
    loadSettings();
}

QString Settings::getConfigDirPath() const
{
    // Per-user config directory under home
    QString home = QDir::homePath();
    return home + "/.seqeyes";
}

QString Settings::getSettingsFilePath() const
{
    return getConfigDirPath() + "/settings.json";
}

void Settings::setZoomInputMode(ZoomInputMode mode)
{
    if (m_zoomInputMode != mode) {
        m_zoomInputMode = mode;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::ZoomInputMode Settings::getZoomInputMode() const
{
    return m_zoomInputMode;
}

QString Settings::getZoomInputModeString() const
{
    switch (m_zoomInputMode) {
        case ZoomInputMode::CtrlWheel: return "CtrlWheel";
        case ZoomInputMode::Wheel: return "Wheel";
        default: return "CtrlWheel";
    }
}

void Settings::setPanWheelEnabled(bool enabled)
{
    if (m_panWheelEnabled != enabled) {
        m_panWheelEnabled = enabled;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getPanWheelEnabled() const
{
    return m_panWheelEnabled;
}

void Settings::setPanLeftKey(const QString& key)
{
    if (m_panLeftKey != key.toUpper())
    {
        m_panLeftKey = key.toUpper();
        saveSettings();
        emit settingsChanged();
    }
}

void Settings::setPanRightKey(const QString& key)
{
    if (m_panRightKey != key.toUpper())
    {
        m_panRightKey = key.toUpper();
        saveSettings();
        emit settingsChanged();
    }
}

QString Settings::getPanLeftKey() const
{
    return m_panLeftKey.isEmpty() ? QStringLiteral("A") : m_panLeftKey;
}

QString Settings::getPanRightKey() const
{
    return m_panRightKey.isEmpty() ? QStringLiteral("D") : m_panRightKey;
}

void Settings::setGradientUnit(GradientUnit unit)
{
    if (m_gradientUnit != unit) {
        m_gradientUnit = unit;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::GradientUnit Settings::getGradientUnit() const
{
    return m_gradientUnit;
}

QString Settings::getGradientUnitString() const
{
    switch (m_gradientUnit) {
        case GradientUnit::HzPerM: return "Hz/m";
        case GradientUnit::mTPerM: return "mT/m";
        case GradientUnit::radPerMsPerMm: return "rad/ms/mm";
        case GradientUnit::GPerCm: return "G/cm";
        default: return "Hz/m";
    }
}

void Settings::setSlewUnit(SlewUnit unit)
{
    if (m_slewUnit != unit) {
        m_slewUnit = unit;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::SlewUnit Settings::getSlewUnit() const
{
    return m_slewUnit;
}

QString Settings::getSlewUnitString() const
{
    switch (m_slewUnit) {
        case SlewUnit::HzPerMPerS: return "Hz/m/s";
        case SlewUnit::mTPerMPerMs: return "mT/m/ms";
        case SlewUnit::TPerMPerS: return "T/m/s";
        case SlewUnit::radPerMsPerMmPerMs: return "rad/ms/mm/ms";
        case SlewUnit::GPerCmPerMs: return "G/cm/ms";
        case SlewUnit::GPerCmPerS: return "G/cm/s";
        default: return "Hz/m/s";
    }
}

void Settings::setTimeUnit(TimeUnit unit)
{
    if (m_timeUnit != unit) {
        m_timeUnit = unit;
        saveSettings();
        emit settingsChanged();
        emit timeUnitChanged();
    }
}

Settings::TimeUnit Settings::getTimeUnit() const
{
    return m_timeUnit;
}

QString Settings::getTimeUnitString() const
{
    switch (m_timeUnit) {
        case TimeUnit::Microseconds: return "us";
        case TimeUnit::Milliseconds:
        default: return "ms";
    }
}

void Settings::setTrajectoryUnit(TrajectoryUnit unit)
{
    if (m_trajectoryUnit != unit) {
        m_trajectoryUnit = unit;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::TrajectoryUnit Settings::getTrajectoryUnit() const
{
    return m_trajectoryUnit;
}

QString Settings::getTrajectoryUnitString() const
{
    switch (m_trajectoryUnit) {
        case TrajectoryUnit::PerM:    return "1/m";
        case TrajectoryUnit::RadPerM: return "rad/m";
        case TrajectoryUnit::InvFov:  return "1/FOV";
        default:                      return "1/m";
    }
}

void Settings::setTrajectoryColormap(TrajectoryColormap map)
{
    if (m_trajectoryColormap != map) {
        m_trajectoryColormap = map;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::TrajectoryColormap Settings::getTrajectoryColormap() const
{
    return m_trajectoryColormap;
}

QString Settings::getTrajectoryColormapString() const
{
    switch (m_trajectoryColormap) {
        case TrajectoryColormap::Jet:     return "jet";
        case TrajectoryColormap::Cividis: return "cividis";
        case TrajectoryColormap::Plasma:  return "plasma";
        default:                          return "jet";
    }
}

void Settings::setGamma(double gamma)
{
    if (qAbs(m_gamma - gamma) > 1e-6) {
        m_gamma = gamma;
        saveSettings();
        emit settingsChanged();
    }
}

double Settings::getGamma() const
{
    return m_gamma;
}

void Settings::setLogLevel(LogLevel level)
{
    if (m_logLevel != level) {
        m_logLevel = level;
        saveSettings();
        emit settingsChanged();
    }
}

Settings::LogLevel Settings::getLogLevel() const
{
    return m_logLevel;
}

QString Settings::getLogLevelString() const
{
    switch (m_logLevel) {
        case LogLevel::Fatal: return "Fatal";
        case LogLevel::Critical: return "Critical";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Info: return "Info";
        case LogLevel::Debug: return "Debug";
        default: return "Info";
    }
}

// Old time-based LOD settings removed - replaced with complexity-based LOD system

void Settings::setShowTeApproximateDialog(bool show)
{
    if (m_showTeApproximateDialog != show) {
        m_showTeApproximateDialog = show;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getShowTeApproximateDialog() const
{
    return m_showTeApproximateDialog;
}

void Settings::setEnableRoosPtxHackAutoDetection(bool enable)
{
    if (m_enableRoosPtxHackAutoDetection != enable) {
        m_enableRoosPtxHackAutoDetection = enable;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getEnableRoosPtxHackAutoDetection() const
{
    return m_enableRoosPtxHackAutoDetection;
}

void Settings::setShowTrajectoryApproximateDialog(bool show)
{
    if (m_showTrajectoryApproximateDialog != show) {
        m_showTrajectoryApproximateDialog = show;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getShowTrajectoryApproximateDialog() const
{
    return m_showTrajectoryApproximateDialog;
}

double Settings::convertGradient(double value, const QString& fromUnit, const QString& toUnit) const
{
    // Convert to standard units (Hz/m)
    double standard = convertToStandardGradient(value, fromUnit);
    
    // Convert from standard units to target units
    return convertFromStandardGradient(standard, toUnit);
}

double Settings::convertSlew(double value, const QString& fromUnit, const QString& toUnit) const
{
    // Convert to standard units (Hz/m/s)
    double standard = convertToStandardSlew(value, fromUnit);
    
    // Convert from standard units to target units
    return convertFromStandardSlew(standard, toUnit);
}

double Settings::convertToStandardGradient(double value, const QString& fromUnit) const
{
    if (fromUnit == "Hz/m") {
        return value;
    } else if (fromUnit == "mT/m") {
        return value * 1e-3 * m_gamma;
    } else if (fromUnit == "rad/ms/mm") {
        return value * 1e6 / (2 * M_PI);
    } else if (fromUnit == "G/cm") {
        return 10 * value * 1e-3 * m_gamma;
    }
    return value; // Default to no conversion
}

double Settings::convertFromStandardGradient(double value, const QString& toUnit) const
{
    if (toUnit == "Hz/m") {
        return value;
    } else if (toUnit == "mT/m") {
        return 1e3 * value / m_gamma;
    } else if (toUnit == "rad/ms/mm") {
        return value * 2 * M_PI * 1e-6;
    } else if (toUnit == "G/cm") {
        return 0.1 * 1e3 * value / m_gamma;
    }
    return value; // Default to no conversion
}

double Settings::convertToStandardSlew(double value, const QString& fromUnit) const
{
    if (fromUnit == "Hz/m/s") {
        return value;
    } else if (fromUnit == "mT/m/ms" || fromUnit == "T/m/s") {
        return value * m_gamma;
    } else if (fromUnit == "rad/ms/mm/ms") {
        return value * 1e9 / (2 * M_PI);
    } else if (fromUnit == "G/cm/ms") {
        return 10 * value * m_gamma;
    } else if (fromUnit == "G/cm/s") {
        return 1e-3 * 10 * value * m_gamma;
    }
    return value; // Default to no conversion
}

double Settings::convertFromStandardSlew(double value, const QString& toUnit) const
{
    if (toUnit == "Hz/m/s") {
        return value;
    } else if (toUnit == "mT/m/ms" || toUnit == "T/m/s") {
        return value / m_gamma;
    } else if (toUnit == "rad/ms/mm/ms") {
        return value * 2 * M_PI * 1e-9;
    } else if (toUnit == "G/cm/ms") {
        return 0.1 * value / m_gamma;
    } else if (toUnit == "G/cm/s") {
        return 0.1 * 1e3 * value / m_gamma;
    }
    return value; // Default to no conversion
}

void Settings::saveSettings()
{
    QJsonObject obj;
    
    // Save units in human-readable format
    obj["gradientUnit"] = getGradientUnitString();
    obj["slewUnit"] = getSlewUnitString();
    obj["timeUnit"] = getTimeUnitString();
    obj["trajectoryUnit"] = getTrajectoryUnitString();
    obj["trajectoryColormap"] = getTrajectoryColormapString();
    obj["gamma"] = m_gamma;
    obj["logLevel"] = getLogLevelString();
    obj["showTeApproximateDialog"] = m_showTeApproximateDialog;
    obj["showTrajectoryApproximateDialog"] = m_showTrajectoryApproximateDialog;
    obj["showExtensionTooltip"] = m_showExtensionTooltip;
    obj["enableRoosPtxHackAutoDetection"] = m_enableRoosPtxHackAutoDetection;
    {
        QJsonArray arr;
        for (const SystemProfile& profile : m_systemProfiles) {
            QJsonObject profileObj;
            profileObj["alias"] = profile.alias.trimmed();
            profileObj["ascPath"] = profile.ascPath.trimmed();
            if (isLimitConfigured(profile.maxGrad))
                profileObj["maxGrad"] = profile.maxGrad;
            if (isLimitConfigured(profile.maxSlew))
                profileObj["maxSlew"] = profile.maxSlew;
            if (isLimitConfigured(profile.maxB1))
                profileObj["maxB1"] = profile.maxB1;
            arr.append(profileObj);
        }
        obj["systemProfiles"] = arr;
    }
    obj["activeSystemProfileAlias"] = m_activeSystemProfileAlias.trimmed();
    obj["pnsShowX"] = m_pnsShowX;
    obj["pnsShowY"] = m_pnsShowY;
    obj["pnsShowZ"] = m_pnsShowZ;
    obj["pnsShowNorm"] = m_pnsShowNorm;
    // Input behavior
    obj["zoomInputMode"] = getZoomInputModeString();
    obj["panWheelEnabled"] = m_panWheelEnabled;
    obj["panLeftKey"] = getPanLeftKey();
    obj["panRightKey"] = getPanRightKey();
    // Zoom & performance
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    // Extension labels (stored as a sub-object: { "SLC": true, ... })
    {
        QJsonObject extObj;
        for (auto it = m_extensionLabelStates.constBegin(); it != m_extensionLabelStates.constEnd(); ++it)
        {
            extObj[it.key()] = it.value();
        }
        obj["extensionLabels"] = extObj;
    }
    
    // Add metadata for better readability
    obj["_comment"] = "Settings file for SeqEyes";
    obj["_version"] = "1.0";
    obj["_lastModified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    
    QJsonDocument doc(obj);
    
    QFile file(m_jsonFilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open settings file for writing:" << m_jsonFilePath;
        return;
    }
    
    // Write with indentation for better readability
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    
    qDebug() << "Settings saved to JSON:";
    qDebug() << "  Gradient Unit:" << getGradientUnitString();
    qDebug() << "  Slew Unit:" << getSlewUnitString();
    qDebug() << "  Time Unit:" << getTimeUnitString();
    qDebug() << "  Gamma:" << m_gamma << "Hz/T";
    qDebug() << "  Settings file:" << m_jsonFilePath;
}

void Settings::loadSettings()
{
    QFile file(m_jsonFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Settings file not found, using defaults:" << m_jsonFilePath;
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse settings JSON:" << error.errorString();
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Load gradient unit - support both old numeric format and new string format
    if (obj.value("gradientUnit").isString()) {
        QString gradientUnitStr = obj.value("gradientUnit").toString();
        m_gradientUnit = stringToGradientUnit(gradientUnitStr);
    } else {
        // Legacy numeric format
        int gradientUnitInt = obj.value("gradientUnit").toInt(static_cast<int>(GradientUnit::mTPerM));
        m_gradientUnit = static_cast<GradientUnit>(gradientUnitInt);
    }
    
    // Load slew unit - support both old numeric format and new string format
    if (obj.value("slewUnit").isString()) {
        QString slewUnitStr = obj.value("slewUnit").toString();
        m_slewUnit = stringToSlewUnit(slewUnitStr);
    } else {
        // Legacy numeric format
        int slewUnitInt = obj.value("slewUnit").toInt(static_cast<int>(SlewUnit::HzPerMPerS));
        m_slewUnit = static_cast<SlewUnit>(slewUnitInt);
    }
    
    // Load time unit
    QString timeUnitStr = obj.value("timeUnit").toString("ms");
    m_timeUnit = stringToTimeUnit(timeUnitStr);

    // Load trajectory unit (string format only; default 1/m)
    QString trajUnitStr = obj.value("trajectoryUnit").toString("1/m");
    m_trajectoryUnit = stringToTrajectoryUnit(trajUnitStr);

    // Load trajectory colormap (string format; default jet)
    QString trajMapStr = obj.value("trajectoryColormap").toString("jet");
    m_trajectoryColormap = stringToTrajectoryColormap(trajMapStr);
    
    // Load gamma
    m_gamma = obj.value("gamma").toDouble(42.576e6);
    
    // Load log level
    QString logLevelStr = obj.value("logLevel").toString("Warning");
    if (logLevelStr == "Fatal") {
        m_logLevel = LogLevel::Fatal;
    } else if (logLevelStr == "Critical") {
        m_logLevel = LogLevel::Critical;
    } else if (logLevelStr == "Warning") {
        m_logLevel = LogLevel::Warning;
    } else if (logLevelStr == "Debug") {
        m_logLevel = LogLevel::Debug;
    } else {
        m_logLevel = LogLevel::Info; // Default (unknown strings, including legacy "Error")
    }

    // Load input behavior
    m_zoomInputMode = stringToZoomInputMode(obj.value("zoomInputMode").toString("Wheel"));
    m_panWheelEnabled = obj.value("panWheelEnabled").toBool(false);
    m_panLeftKey = obj.value("panLeftKey").toString("A").toUpper();
    m_panRightKey = obj.value("panRightKey").toString("D").toUpper();
    // Approximate overlay dialogs (default true)
    m_showTeApproximateDialog = obj.value("showTeApproximateDialog").toBool(true);
    m_showTeApproximateDialog = obj.value("showTeApproximateDialog").toBool(true);
    m_showTrajectoryApproximateDialog = obj.value("showTrajectoryApproximateDialog").toBool(true);
    m_showExtensionTooltip = obj.value("showExtensionTooltip").toBool(false);
    m_enableRoosPtxHackAutoDetection = obj.value("enableRoosPtxHackAutoDetection").toBool(true);
    m_systemProfiles.clear();
    m_activeSystemProfileAlias = obj.value("activeSystemProfileAlias").toString().trimmed();
    if (obj.value("systemProfiles").isArray()) {
        const QJsonArray profiles = obj.value("systemProfiles").toArray();
        for (const QJsonValue& v : profiles) {
            if (!v.isObject())
                continue;
            const QJsonObject profileObj = v.toObject();
            SystemProfile profile;
            profile.alias = profileObj.value("alias").toString().trimmed();
            profile.ascPath = profileObj.value("ascPath").toString().trimmed();
            profile.maxGrad = profileObj.contains("maxGrad") ? profileObj.value("maxGrad").toDouble(kUnsetLimit) : kUnsetLimit;
            profile.maxSlew = profileObj.contains("maxSlew") ? profileObj.value("maxSlew").toDouble(kUnsetLimit) : kUnsetLimit;
            profile.maxB1 = profileObj.contains("maxB1") ? profileObj.value("maxB1").toDouble(kUnsetLimit) : kUnsetLimit;
            m_systemProfiles.append(profile);
        }
    } else {
        migrateLegacyPnsSettings(obj);
    }
    sanitizeSystemProfiles();
    m_pnsShowX = obj.value("pnsShowX").toBool(false);
    m_pnsShowY = obj.value("pnsShowY").toBool(false);
    m_pnsShowZ = obj.value("pnsShowZ").toBool(true);
    m_pnsShowNorm = obj.value("pnsShowNorm").toBool(true);

    // Load extension labels (merge onto defaults)
    if (obj.contains("extensionLabels") && obj.value("extensionLabels").isObject())
    {
        QJsonObject extObj = obj.value("extensionLabels").toObject();
        for (auto it = extObj.constBegin(); it != extObj.constEnd(); ++it)
        {
            if (it.value().isBool())
                m_extensionLabelStates[it.key()] = it.value().toBool();
        }
    }

    // Load zoom & performance
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    
    qDebug() << "Settings loaded from JSON:";
    qDebug() << "  Gradient Unit:" << getGradientUnitString();
    qDebug() << "  Slew Unit:" << getSlewUnitString();
    qDebug() << "  Time Unit:" << getTimeUnitString();
    qDebug() << "  Gamma:" << m_gamma << "Hz/T";
    qDebug() << "  Log Level:" << getLogLevelString();
    qDebug() << "  Zoom Input Mode:" << getZoomInputModeString();
    qDebug() << "  Pan Wheel Enabled:" << m_panWheelEnabled;
}

void Settings::resetToDefaults()
{
    m_zoomInputMode = ZoomInputMode::Wheel;
    m_panWheelEnabled = false;
    m_gradientUnit = GradientUnit::mTPerM;
    m_slewUnit = SlewUnit::TPerMPerS;
    m_timeUnit = TimeUnit::Milliseconds;
    m_trajectoryUnit = TrajectoryUnit::PerM;
    m_trajectoryColormap = TrajectoryColormap::Jet;
    m_gamma = 42.576e6; // Hz/T for hydrogen
    m_logLevel = LogLevel::Warning; // Default to Warning level
    m_showTeApproximateDialog = true;
    m_showTrajectoryApproximateDialog = true;
    m_showExtensionTooltip = false;
    m_enableRoosPtxHackAutoDetection = true;
    m_systemProfiles.clear();
    m_activeSystemProfileAlias.clear();
    m_pnsShowX = false;
    m_pnsShowY = false;
    m_pnsShowZ = true;
    m_pnsShowNorm = true;
    m_panLeftKey = QStringLiteral("A");
    m_panRightKey = QStringLiteral("D");
    // Old time-based LOD settings removed - replaced with complexity-based LOD system
    
    // Save the reset values
    saveSettings();
    
    // Emit signal to notify UI
    emit settingsChanged();
    
    qDebug() << "Settings reset to defaults:";
    qDebug() << "  Gradient Unit:" << getGradientUnitString();
    qDebug() << "  Slew Unit:" << getSlewUnitString();
    qDebug() << "  Gamma:" << m_gamma << "Hz/T";
}

QStringList Settings::getAvailableGradientUnits()
{
    return {"Hz/m", "mT/m", "rad/ms/mm", "G/cm"};
}

QStringList Settings::getAvailableSlewUnits()
{
    return {"Hz/m/s", "mT/m/ms", "T/m/s", "rad/ms/mm/ms", "G/cm/ms", "G/cm/s"};
}

QStringList Settings::getAvailableTimeUnits()
{
    return {"ms", "us"};
}

QStringList Settings::getAvailableTrajectoryUnits()
{
    return {"1/m", "rad/m", "1/FOV"};
}

QStringList Settings::getSupportedExtensionLabels()
{
    // Supported Pulseq extension labels (data counters, data flags, control flags)
    return {
        // Data counters
        "SLC","SEG","REP","AVG","SET","ECO","PHS","LIN","PAR","ACQ",
        // Data flags
        "NAV","REV","SMS","REF","IMA","OFF","NOISE",
        // Control flags / switches
        "PMC","NOROT","NOPOS","NOSCL","ONCE","TRID"
    };
}

void Settings::setExtensionLabelEnabled(const QString& label, bool enabled)
{
    if (!m_extensionLabelStates.contains(label) || m_extensionLabelStates.value(label) != enabled)
    {
        m_extensionLabelStates[label] = enabled;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::isExtensionLabelEnabled(const QString& label) const
{
    auto it = m_extensionLabelStates.constFind(label);
    if (it != m_extensionLabelStates.constEnd())
        return it.value();
    // Unknown labels default to enabled
    return true;
}

void Settings::initDefaultExtensionLabels()
{
    m_extensionLabelStates.clear();
    const QStringList labels = getSupportedExtensionLabels();
    for (const QString& lab : labels)
        m_extensionLabelStates.insert(lab, true);
}

Settings::GradientUnit Settings::stringToGradientUnit(const QString& unitString) const
{
    if (unitString == "Hz/m") return GradientUnit::HzPerM;
    if (unitString == "mT/m") return GradientUnit::mTPerM;
    if (unitString == "rad/ms/mm") return GradientUnit::radPerMsPerMm;
    if (unitString == "G/cm") return GradientUnit::GPerCm;
    return GradientUnit::HzPerM; // Default fallback
}

Settings::SlewUnit Settings::stringToSlewUnit(const QString& unitString) const
{
    if (unitString == "Hz/m/s") return SlewUnit::HzPerMPerS;
    if (unitString == "mT/m/ms") return SlewUnit::mTPerMPerMs;
    if (unitString == "T/m/s") return SlewUnit::TPerMPerS;
    if (unitString == "rad/ms/mm/ms") return SlewUnit::radPerMsPerMmPerMs;
    if (unitString == "G/cm/ms") return SlewUnit::GPerCmPerMs;
    if (unitString == "G/cm/s") return SlewUnit::GPerCmPerS;
    return SlewUnit::HzPerMPerS; // Default fallback
}

Settings::ZoomInputMode Settings::stringToZoomInputMode(const QString& s) const
{
    if (s == "Wheel") return ZoomInputMode::Wheel;
    return ZoomInputMode::CtrlWheel;
}

Settings::TimeUnit Settings::stringToTimeUnit(const QString& unitString) const
{
    if (unitString == "us") return TimeUnit::Microseconds;
    return TimeUnit::Milliseconds;
}

Settings::TrajectoryUnit Settings::stringToTrajectoryUnit(const QString& unitString) const
{
    if (unitString == "1/m")    return TrajectoryUnit::PerM;
    if (unitString == "rad/m")  return TrajectoryUnit::RadPerM;
    if (unitString == "1/FOV")  return TrajectoryUnit::InvFov;
    return TrajectoryUnit::PerM;
}

Settings::TrajectoryColormap Settings::stringToTrajectoryColormap(const QString& name) const
{
    QString n = name.toLower();
    if (n == "jet")     return TrajectoryColormap::Jet;
    if (n == "cividis") return TrajectoryColormap::Cividis;
    if (n == "plasma")  return TrajectoryColormap::Plasma;
    return TrajectoryColormap::Jet;
}

void Settings::setShowExtensionTooltip(bool show)
{
    if (m_showExtensionTooltip != show) {
        m_showExtensionTooltip = show;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getShowExtensionTooltip() const
{
    return m_showExtensionTooltip;
}

QVector<Settings::SystemProfile> Settings::getSystemProfiles() const
{
    return m_systemProfiles;
}

QString Settings::getActiveSystemProfileAlias() const
{
    return m_activeSystemProfileAlias;
}

Settings::SystemProfile Settings::getActiveSystemProfile() const
{
    for (const SystemProfile& profile : m_systemProfiles) {
        if (profile.alias == m_activeSystemProfileAlias)
            return profile;
    }
    return SystemProfile{};
}

void Settings::setSystemProfiles(const QVector<SystemProfile>& profiles)
{
    const QVector<SystemProfile> prevProfiles = m_systemProfiles;
    const QString prevActive = m_activeSystemProfileAlias;
    m_systemProfiles = profiles;
    sanitizeSystemProfiles();
    if (m_systemProfiles != prevProfiles || m_activeSystemProfileAlias != prevActive) {
        saveSettings();
        emit settingsChanged();
    }
}

void Settings::setActiveSystemProfileAlias(const QString& alias)
{
    if (m_activeSystemProfileAlias == alias.trimmed())
        return;
    m_activeSystemProfileAlias = alias.trimmed();
    sanitizeSystemProfiles();
    saveSettings();
    emit settingsChanged();
}

QString Settings::generateNextSystemProfileAlias() const
{
    QSet<QString> used;
    for (const SystemProfile& profile : m_systemProfiles)
        used.insert(profile.alias.toCaseFolded());

    for (int i = 1; ; ++i) {
        const QString candidate = QStringLiteral("system%1").arg(i);
        if (!used.contains(candidate.toCaseFolded()))
            return candidate;
    }
}

QString Settings::getPnsAscPath() const
{
    return getActiveSystemProfile().ascPath.trimmed();
}

QStringList Settings::getPnsAscHistory() const
{
    QStringList paths;
    for (const SystemProfile& profile : m_systemProfiles) {
        const QString path = profile.ascPath.trimmed();
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

QString Settings::getPnsAscNickname(const QString& path) const
{
    const QString key = path.trimmed();
    if (key.isEmpty()) {
        return QString();
    }
    for (const SystemProfile& profile : m_systemProfiles) {
        if (profile.ascPath.trimmed() == key)
            return profile.alias.trimmed();
    }
    return QString();
}

QMap<QString, QString> Settings::getPnsAscNicknames() const
{
    QMap<QString, QString> nicknames;
    for (const SystemProfile& profile : m_systemProfiles) {
        const QString path = profile.ascPath.trimmed();
        if (!path.isEmpty())
            nicknames.insert(path, profile.alias.trimmed());
    }
    return nicknames;
}

void Settings::setPnsAscPath(const QString& path)
{
    const QString normalized = path.trimmed();
    QVector<SystemProfile> next = m_systemProfiles;

    if (next.isEmpty()) {
        SystemProfile profile;
        profile.alias = generateNextSystemProfileAlias();
        profile.ascPath = normalized;
        profile.maxGrad = kUnsetLimit;
        profile.maxSlew = kUnsetLimit;
        profile.maxB1 = kUnsetLimit;
        next.append(profile);
        m_systemProfiles = next;
        m_activeSystemProfileAlias = profile.alias;
        saveSettings();
        emit settingsChanged();
        return;
    }

    bool updated = false;
    for (SystemProfile& profile : next) {
        if (profile.alias == m_activeSystemProfileAlias) {
            profile.ascPath = normalized;
            updated = true;
            break;
        }
    }
    if (!updated) {
        SystemProfile profile;
        profile.alias = generateNextSystemProfileAlias();
        profile.ascPath = normalized;
        profile.maxGrad = kUnsetLimit;
        profile.maxSlew = kUnsetLimit;
        profile.maxB1 = kUnsetLimit;
        next.prepend(profile);
        m_activeSystemProfileAlias = profile.alias;
    }
    setSystemProfiles(next);
}

void Settings::setPnsAscHistory(const QStringList& history)
{
    QStringList normalized;
    for (const QString& p : history) {
        const QString trimmed = p.trimmed();
        if (trimmed.isEmpty() || normalized.contains(trimmed))
            continue;
        normalized.append(trimmed);
        if (normalized.size() >= kMaxAscHistoryItems)
            break;
    }

    QVector<SystemProfile> next = m_systemProfiles;
    for (const QString& path : normalized) {
        bool exists = false;
        for (const SystemProfile& profile : next) {
            if (profile.ascPath.trimmed() == path) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            SystemProfile profile;
            profile.alias = generateNextSystemProfileAlias();
            profile.ascPath = path;
            profile.maxGrad = kUnsetLimit;
            profile.maxSlew = kUnsetLimit;
            profile.maxB1 = kUnsetLimit;
            next.append(profile);
        }
    }
    setSystemProfiles(next);
}

void Settings::setPnsAscNickname(const QString& path, const QString& nickname)
{
    const QString key = path.trimmed();
    if (key.isEmpty()) {
        return;
    }

    QVector<SystemProfile> next = m_systemProfiles;
    for (SystemProfile& profile : next) {
        if (profile.ascPath.trimmed() == key) {
            profile.alias = nickname.trimmed();
            setSystemProfiles(next);
            return;
        }
    }

    SystemProfile profile;
    profile.alias = nickname.trimmed();
    profile.ascPath = key;
    profile.maxGrad = kUnsetLimit;
    profile.maxSlew = kUnsetLimit;
    profile.maxB1 = kUnsetLimit;
    next.append(profile);
    setSystemProfiles(next);
}

bool Settings::removePnsAscHistoryPath(const QString& path)
{
    const QString normalized = path.trimmed();
    if (normalized.isEmpty()) {
        return false;
    }

    QVector<SystemProfile> next;
    bool removed = false;
    for (const SystemProfile& profile : m_systemProfiles) {
        if (profile.ascPath.trimmed() == normalized) {
            removed = true;
            continue;
        }
        next.append(profile);
    }
    if (!removed)
        return false;
    setSystemProfiles(next);
    return true;
}

int Settings::removeInvalidPnsAscHistoryPaths()
{
    QVector<SystemProfile> next;
    int removed = 0;
    for (const SystemProfile& profile : m_systemProfiles) {
        const QString path = profile.ascPath.trimmed();
        if (!path.isEmpty() && !QFileInfo::exists(path)) {
            ++removed;
            continue;
        }
        next.append(profile);
    }
    if (removed > 0)
        setSystemProfiles(next);
    return removed;
}

QString Settings::normalizeSystemAlias(const QString& alias) const
{
    return alias.trimmed();
}

QString Settings::makeUniqueSystemAlias(const QString& preferredAlias, const QString& currentAlias) const
{
    QString base = normalizeSystemAlias(preferredAlias);
    if (base.isEmpty())
        base = QStringLiteral("system");

    QSet<QString> used;
    for (const SystemProfile& profile : m_systemProfiles) {
        const QString alias = profile.alias.trimmed();
        if (!alias.isEmpty() && alias.compare(currentAlias, Qt::CaseInsensitive) != 0)
            used.insert(alias.toCaseFolded());
    }

    if (base != QStringLiteral("system") && !used.contains(base.toCaseFolded()))
        return base;

    if (base == QStringLiteral("system")) {
        for (int i = 1; ; ++i) {
            const QString candidate = QStringLiteral("system%1").arg(i);
            if (!used.contains(candidate.toCaseFolded()))
                return candidate;
        }
    }

    for (int i = 2; ; ++i) {
        const QString candidate = QStringLiteral("%1_%2").arg(base).arg(i);
        if (!used.contains(candidate.toCaseFolded()))
            return candidate;
    }
}

void Settings::sanitizeSystemProfiles()
{
    QVector<SystemProfile> sanitized;
    QSet<QString> seenAliases;
    for (const SystemProfile& original : m_systemProfiles) {
        SystemProfile profile = original;
        profile.ascPath = profile.ascPath.trimmed();
        profile.alias = normalizeSystemAlias(profile.alias);

        if (!isLimitConfigured(profile.maxGrad))
            profile.maxGrad = kUnsetLimit;
        if (!isLimitConfigured(profile.maxSlew))
            profile.maxSlew = kUnsetLimit;
        if (!isLimitConfigured(profile.maxB1))
            profile.maxB1 = kUnsetLimit;

        QString alias = profile.alias;
        if (alias.isEmpty()) {
            int idx = 1;
            QString candidate = QStringLiteral("system%1").arg(idx);
            while (seenAliases.contains(candidate.toCaseFolded())) {
                ++idx;
                candidate = QStringLiteral("system%1").arg(idx);
            }
            alias = candidate;
        } else if (seenAliases.contains(alias.toCaseFolded())) {
            int idx = 2;
            QString candidate = QStringLiteral("%1_%2").arg(alias).arg(idx);
            while (seenAliases.contains(candidate.toCaseFolded())) {
                ++idx;
                candidate = QStringLiteral("%1_%2").arg(alias).arg(idx);
            }
            alias = candidate;
        }

        profile.alias = alias;
        seenAliases.insert(alias.toCaseFolded());
        sanitized.append(profile);
    }

    m_systemProfiles = sanitized;
    if (m_systemProfiles.isEmpty()) {
        m_activeSystemProfileAlias.clear();
        return;
    }

    bool activeFound = false;
    for (const SystemProfile& profile : m_systemProfiles) {
        if (profile.alias == m_activeSystemProfileAlias) {
            activeFound = true;
            break;
        }
    }
    if (!activeFound)
        m_activeSystemProfileAlias = m_systemProfiles.first().alias;
}

void Settings::migrateLegacyPnsSettings(const QJsonObject& obj)
{
    const QString currentPath = obj.value("pnsAscPath").toString().trimmed();
    QStringList history;
    if (obj.value("pnsAscHistory").isArray()) {
        const QJsonArray arr = obj.value("pnsAscHistory").toArray();
        for (const QJsonValue& v : arr) {
            if (!v.isString())
                continue;
            const QString path = v.toString().trimmed();
            if (!path.isEmpty() && !history.contains(path))
                history.append(path);
        }
    }
    if (!currentPath.isEmpty() && !history.contains(currentPath))
        history.prepend(currentPath);

    QMap<QString, QString> nicknames;
    if (obj.value("pnsAscNicknames").isObject()) {
        const QJsonObject nickObj = obj.value("pnsAscNicknames").toObject();
        for (auto it = nickObj.constBegin(); it != nickObj.constEnd(); ++it) {
            if (it.value().isString())
                nicknames.insert(it.key().trimmed(), it.value().toString().trimmed());
        }
    }

    for (const QString& path : history) {
        SystemProfile profile;
        profile.alias = nicknames.value(path).trimmed();
        profile.ascPath = path;
        profile.maxGrad = kUnsetLimit;
        profile.maxSlew = kUnsetLimit;
        profile.maxB1 = kUnsetLimit;
        m_systemProfiles.append(profile);
    }
}

bool Settings::isLimitConfigured(double value)
{
    return std::isfinite(value) && value > 0.0;
}

void Settings::setPnsChannelVisibleX(bool visible)
{
    if (m_pnsShowX != visible) {
        m_pnsShowX = visible;
        saveSettings();
        emit settingsChanged();
    }
}

void Settings::setPnsChannelVisibleY(bool visible)
{
    if (m_pnsShowY != visible) {
        m_pnsShowY = visible;
        saveSettings();
        emit settingsChanged();
    }
}

void Settings::setPnsChannelVisibleZ(bool visible)
{
    if (m_pnsShowZ != visible) {
        m_pnsShowZ = visible;
        saveSettings();
        emit settingsChanged();
    }
}

void Settings::setPnsChannelVisibleNorm(bool visible)
{
    if (m_pnsShowNorm != visible) {
        m_pnsShowNorm = visible;
        saveSettings();
        emit settingsChanged();
    }
}

bool Settings::getPnsChannelVisibleX() const
{
    return m_pnsShowX;
}

bool Settings::getPnsChannelVisibleY() const
{
    return m_pnsShowY;
}

bool Settings::getPnsChannelVisibleZ() const
{
    return m_pnsShowZ;
}

bool Settings::getPnsChannelVisibleNorm() const
{
    return m_pnsShowNorm;
}
