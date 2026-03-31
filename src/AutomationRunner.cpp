#include "AutomationRunner.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "WaveformDrawer.h"
#include "InteractionHandler.h"
#include "Settings.h"
#include "TRManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>

static bool readJsonFile(const QString& path, QJsonObject& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err; auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = doc.object();
    return true;
}

static bool writeTrajectoryFile(const QString& path,
                                const QVector<double>& kx,
                                const QVector<double>& ky,
                                const QVector<double>& kz)
{
    if (kx.size() != ky.size() || kx.size() != kz.size() || kx.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream ts(&file);
    ts.setRealNumberNotation(QTextStream::ScientificNotation);
    ts.setRealNumberPrecision(12);
    for (int i = 0; i < kx.size(); ++i)
    {
        ts << kx[i] << "\t" << ky[i] << "\t" << kz[i] << "\n";
    }
    return true;
}

int AutomationRunner::run(MainWindow& window, const QString& scenarioJsonPath)
{
    QJsonObject root;
    if (!readJsonFile(scenarioJsonPath, root)) {
        qWarning() << "[AUTOMATION] Failed to read scenario:" << scenarioJsonPath;
        return 2;
    }
    if (!root.contains("actions") || !root.value("actions").isArray()) {
        qWarning() << "[AUTOMATION] Missing actions array";
        return 3;
    }

    QJsonArray actions = root.value("actions").toArray();
    for (const auto& aVal : actions) {
        if (!aVal.isObject()) continue;
        QJsonObject a = aVal.toObject();
        QString type = a.value("type").toString();
        QVariantMap params = a.toVariantMap();
        int rc = runAction(window, type, params);
        if (rc != 0) return rc;
    }
    return 0;
}

int AutomationRunner::runAction(MainWindow& window, const QString& type, const QVariantMap& params)
{
    if (type == "open_file") {
        QString p = params.value("path").toString();
        if (p.isEmpty()) { qWarning() << "[AUTOMATION] open_file: missing path"; return 10; }
        if (auto* loader = window.getPulseqLoader()) {
            loader->setSilentMode(true);
        }
        window.openFileFromCommandLine(p);
        return 0;
    }

    if (type == "reset_view") {
        if (auto* drawer = window.getWaveformDrawer()) {
            drawer->ResetView();
            return 0;
        }
        return 11;
    }

    if (type == "configure_pns") {
        const QString ascPath = params.value("asc_path").toString().trimmed();
        if (ascPath.isEmpty()) {
            qWarning() << "[AUTOMATION] configure_pns: missing asc_path";
            return 13;
        }
        if (!QFileInfo::exists(ascPath)) {
            qWarning() << "[AUTOMATION] configure_pns: asc_path does not exist:" << ascPath;
            return 14;
        }

        const bool showPns = params.value("show_pns", true).toBool();
        const bool showX = params.value("show_x", true).toBool();
        const bool showY = params.value("show_y", true).toBool();
        const bool showZ = params.value("show_z", true).toBool();
        const bool showNorm = params.value("show_norm", true).toBool();

        Settings& s = Settings::getInstance();
        s.setPnsAscPath(ascPath);
        s.setPnsChannelVisibleX(showX);
        s.setPnsChannelVisibleY(showY);
        s.setPnsChannelVisibleZ(showZ);
        s.setPnsChannelVisibleNorm(showNorm);

        if (auto* loader = window.getPulseqLoader()) {
            loader->recomputePnsFromSettings();
        }
        if (auto* tr = window.getTRManager()) {
            tr->setShowPns(showPns);
        }
        return 0;
    }

    if (type == "measure_zoom_by_factor") {
        double factor = params.value("factor", 0.5).toDouble();
        if (factor <= 0.0 || factor >= 1.0) { qWarning() << "[AUTOMATION] measure_zoom_by_factor: invalid factor"; return 12; }
        // baseline
        QCPRange r = window.ui->customPlot->xAxis->range();
        double center = 0.5 * (r.lower + r.upper);
        double width  = r.size();
        double newWidth = width * factor;
        QCPRange newRange(center - newWidth/2.0, center + newWidth/2.0);
        // measure via interaction path
        InteractionHandler* ih = window.getInteractionHandler();
        QElapsedTimer t; t.start();
        if (ih) {
            ih->synchronizeXAxes(newRange);
        } else {
            // fallback if handler not available
            window.ui->customPlot->xAxis->setRange(newRange);
            window.ui->customPlot->replot(QCustomPlot::rpQueuedReplot);
            qApp->processEvents();
        }
        qint64 ms = t.elapsed();
        QTextStream(stdout) << "ZOOM_MS: " << ms << "\n";
        return 0;
    }

    if (type == "export_trajectory") {
        QString outDir = params.value("dir").toString().trimmed();
        if (outDir.isEmpty()) { qWarning() << "[AUTOMATION] export_trajectory: missing dir"; return 15; }

        QDir dir(outDir);
        if (!dir.exists() && !QDir().mkpath(outDir)) {
            qWarning() << "[AUTOMATION] export_trajectory: cannot create dir:" << outDir;
            return 16;
        }

        PulseqLoader* loader = window.getPulseqLoader();
        if (!loader) {
            qWarning() << "[AUTOMATION] export_trajectory: loader unavailable";
            return 17;
        }
        loader->ensureTrajectoryPrepared();

        const QVector<double>& kx = loader->getTrajectoryKx();
        const QVector<double>& ky = loader->getTrajectoryKy();
        const QVector<double>& kz = loader->getTrajectoryKz();
        const QVector<double>& kxAdc = loader->getTrajectoryKxAdc();
        const QVector<double>& kyAdc = loader->getTrajectoryKyAdc();
        const QVector<double>& kzAdc = loader->getTrajectoryKzAdc();
        if (kx.isEmpty() || ky.isEmpty() || kz.isEmpty()) {
            qWarning() << "[AUTOMATION] export_trajectory: empty ktraj";
            return 18;
        }
        if (kxAdc.isEmpty() || kyAdc.isEmpty() || kzAdc.isEmpty()) {
            qWarning() << "[AUTOMATION] export_trajectory: empty ktraj_adc";
            return 19;
        }

        const QString ktrajPath = dir.filePath("ktraj.txt");
        const QString ktrajAdcPath = dir.filePath("ktraj_adc.txt");
        if (!writeTrajectoryFile(ktrajPath, kx, ky, kz)) {
            qWarning() << "[AUTOMATION] export_trajectory: failed writing" << ktrajPath;
            return 20;
        }
        if (!writeTrajectoryFile(ktrajAdcPath, kxAdc, kyAdc, kzAdc)) {
            qWarning() << "[AUTOMATION] export_trajectory: failed writing" << ktrajAdcPath;
            return 21;
        }
        qInfo() << "[AUTOMATION] exported trajectory to" << QDir::toNativeSeparators(outDir);
        return 0;
    }

    qWarning() << "[AUTOMATION] Unknown action type:" << type;
    return 99;
}


