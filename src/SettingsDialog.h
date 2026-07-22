#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QTabWidget>
#include <QScrollArea>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include "Settings.h"

QT_BEGIN_NAMESPACE
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;
class QHBoxLayout;
class QShowEvent;
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
    void onSystemProfileChanged(int index);
    void onAddSystemProfileClicked();
    void onRemoveSystemProfileClicked();
    void onBrowseSystemAscPath();
    void onClearSystemAscPath();
    void onAxisOrderSelectionChanged();
    void onMoveAxisUpClicked();
    void onMoveAxisDownClicked();
    void onMoveAxisTopClicked();
    void onMoveAxisBottomClicked();
    void onAxisOrderRowsMoved();

private:
    void setupUI();
    void loadCurrentSettings();
    bool applySettings();
    void showCustomGammaDialog();
    void updateInteractionControlsForExclusivity();
    void loadCurrentSystemProfileIntoEditor();
    void syncCurrentSystemProfileDraft(bool updateComboText = true);
    int currentSystemProfileIndex() const;
    Settings::SystemProfile buildCurrentSystemProfileFromEditor(bool* ok = nullptr, QString* error = nullptr) const;
    QString resolvedAliasForInput(const QString& alias, int profileIndex) const;
    void populateAxisOrderList(const QStringList& order);
    QStringList currentAxisOrderFromList() const;
    void moveSelectedAxisItem(int delta);
    void updateAxisOrderButtons();
    void showEvent(QShowEvent* event) override;

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

    // Misc tab
    QComboBox* m_logLevelCombo;
    QLabel* m_settingsPathValue;
    QCheckBox* m_autoReloadOnFileChangeCheck;

    // Interaction tab
    QComboBox* m_zoomModeCombo;
    QCheckBox* m_panDragCheck;
    QCheckBox* m_panWheelCheck;
    QLabel* m_shortcutInfoLabel;

    // Extension tab - label visibility controls
    QCheckBox* m_showExtensionTooltipCheck;
    QCheckBox* m_enableRoosPtxHackAutoDetectionCheck;
    QMap<QString, QCheckBox*> m_extensionLabelCheckboxes;

    // Safety/System tab
    QComboBox* m_systemProfileCombo;
    QPushButton* m_addSystemProfileButton;
    QPushButton* m_removeSystemProfileButton;
    QLineEdit* m_systemAliasEdit;
    QLineEdit* m_systemAscPathEdit;
    QLineEdit* m_systemB0Edit;
    QPushButton* m_systemAscBrowseButton;
    QPushButton* m_systemAscClearButton;
    QLineEdit* m_systemMaxGradEdit;
    QLineEdit* m_systemMaxSlewEdit;
    QLineEdit* m_systemMaxB1Edit;
    QCheckBox* m_pnsShowXCheck;
    QCheckBox* m_pnsShowYCheck;
    QCheckBox* m_pnsShowZCheck;
    QCheckBox* m_pnsShowNormCheck;

    // Layout tab
    QListWidget* m_axisOrderList;
    QPushButton* m_moveAxisTopButton;
    QPushButton* m_moveAxisUpButton;
    QPushButton* m_moveAxisDownButton;
    QPushButton* m_moveAxisBottomButton;

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
    bool m_originalEnableRoosPtxHackAutoDetection {true};
    QMap<QString, bool> m_originalExtensionLabelStates;
    double m_originalGamma;
    Settings::LogLevel m_originalLogLevel;
    Settings::ZoomInputMode m_originalZoomInputMode;
    bool m_originalPanWheelEnabled;
    bool m_originalAutoReloadOnFileChange {false};
    QVector<Settings::SystemProfile> m_originalSystemProfiles;
    QString m_originalActiveSystemProfileAlias;
    QVector<Settings::SystemProfile> m_systemProfilesDraft;
    bool m_originalPnsShowX {false};
    bool m_originalPnsShowY {false};
    bool m_originalPnsShowZ {true};
    bool m_originalPnsShowNorm {true};
};

#endif // SETTINGSDIALOG_H
