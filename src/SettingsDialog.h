#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QScrollArea>
#include <QCheckBox>
#include <QMap>
#include "Settings.h"

QT_BEGIN_NAMESPACE
class QFormLayout;
class QGroupBox;
class QLabel;
class QVBoxLayout;
class QHBoxLayout;
QT_END_NAMESPACE

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog();

private slots:
    void onApplyClicked();
    void onOKClicked();
    void onCancelClicked();
    void onResetClicked();
    void onGammaComboChanged(int index);
    void onZoomModeChanged(int index);
    void onPanWheelToggled(bool checked);

private:
    void setupUI();
    void loadCurrentSettings();
    void applySettings();
    void showCustomGammaDialog();
    void updateInteractionControlsForExclusivity();
    
    // UI components - Ribbon style
    QTabWidget* m_tabWidget;
    
    // Display Units tab
    QComboBox* m_gradientUnitCombo;
    QComboBox* m_slewUnitCombo;
    QComboBox* m_timeUnitCombo;
    QComboBox* m_trajectoryUnitCombo;
    QComboBox* m_trajectoryColormapCombo;
    
    // Physics Parameters tab
    QComboBox* m_gammaCombo;
    
    // Logging tab
    QComboBox* m_logLevelCombo;
    QLabel* m_settingsPathValue;

    // Interaction tab
    QComboBox* m_zoomModeCombo;
    QCheckBox* m_panDragCheck;
    QCheckBox* m_panWheelCheck;
    QLabel* m_shortcutInfoLabel;

    // Extension tab - label visibility controls
    QCheckBox* m_showExtensionTooltipCheck;
    QMap<QString, QCheckBox*> m_extensionLabelCheckboxes;
    
    // Buttons
    QPushButton* m_applyButton;
    QPushButton* m_okButton;
    QPushButton* m_cancelButton;
    QPushButton* m_resetButton;
    
    // Store original settings for cancel functionality
    Settings::GradientUnit m_originalGradientUnit;
    Settings::SlewUnit m_originalSlewUnit;
    Settings::TimeUnit m_originalTimeUnit;
    Settings::TrajectoryUnit m_originalTrajectoryUnit;
    Settings::TrajectoryColormap m_originalTrajectoryColormap;
    bool m_originalShowExtensionTooltip;
    QMap<QString, bool> m_originalExtensionLabelStates;
    double m_originalGamma;
    Settings::LogLevel m_originalLogLevel;
    Settings::ZoomInputMode m_originalZoomInputMode;
    bool m_originalPanWheelEnabled;
};

#endif // SETTINGSDIALOG_H
