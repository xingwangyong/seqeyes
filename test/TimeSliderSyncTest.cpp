// Minimal GUI test validating time slider sync is silent and accurate in TR mode
#include <QtTest/QtTest>
#include <QtWidgets>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <cmath>
#include "TRManager.h"
#include "WaveformDrawer.h"
#include "PulseqLoader.h"
#include "InteractionHandler.h"
#include "doublerangeslider.h"
#include "qcustomplot.h"

class TimeSliderSyncTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // Nothing
    }

private:
    static QString resolveSeqPath()
    {
        const QString envSeq = qEnvironmentVariable("TIME_SLIDER_TEST_SEQ");
        if (!envSeq.isEmpty())
            return envSeq;

        QString seqPath;
        QStringList args = QCoreApplication::arguments();
        for (int i = 1; i < args.size(); ++i) {
            if (args[i].endsWith(".seq", Qt::CaseInsensitive)) { seqPath = args[i]; break; }
            if (args[i] == "--file" && i+1 < args.size()) { seqPath = args[i+1]; break; }
        }
        if (seqPath.isEmpty()) {
            seqPath = QCoreApplication::applicationDirPath() + "/../test/seq_files/writeEpi.seq";
            if (!QFile::exists(seqPath)) {
                seqPath = QDir(QCoreApplication::applicationDirPath() + "/../../").absoluteFilePath("test/seq_files/writeEpi.seq");
            }
        }
        return seqPath;
    }

private slots:

    void test_zoom_pan_updates_time_slider_silently_TRMode()
    {
        MainWindow w; // do not show GUI
        QString seqPath = resolveSeqPath();
        QVERIFY2(QFile::exists(seqPath), "Test sequence file not found");

        w.getPulseqLoader()->setSilentMode(true);
        w.show(); QTest::qWait(50);
        w.openFileFromCommandLine(seqPath);

        TRManager* tr = w.getTRManager();
        QVERIFY(tr);
        PulseqLoader* loader = w.getPulseqLoader();
        QVERIFY(loader);
        if (!loader->hasRepetitionTime()) {
            QSKIP("Sequence has no TR; skipping TR-mode test");
        }

        // Ensure we are in TR mode
        tr->setRenderModeTrSegmented();

        // Current viewport
        auto rects = w.getWaveformDrawer()->getRects();
        QVERIFY(!rects.isEmpty());
        QCPRange r0 = rects[0]->axis(QCPAxis::atBottom)->range();

        // Simulate user wheel zoom: Ctrl + wheel up at plot center
        QCustomPlot* plot = w.getUI()->customPlot;
        QPoint centerPt = plot->rect().center();
        QWheelEvent zoomEv(
            QPointF(centerPt), QPointF(plot->mapToGlobal(centerPt)),
            QPoint(0, 0), QPoint(0, 120), // angleDelta 120 up
            Qt::NoButton, Qt::ControlModifier, Qt::ScrollBegin, false
        );
        QSignalSpy spy(tr->getTimeRangeSlider(), SIGNAL(valuesChanged(int,int)));
        QApplication::sendEvent(plot, &zoomEv);
        QTest::qWait(20);

        // No emissions expected due to QSignalBlocker
        QCOMPARE(spy.count(), 0);

        // Validate time slider reflects viewport (relative within TR)
        double tFactor = loader->getTFactor();
        QCPRange zr = plot->xAxis->range();
        double absStart_ms = zr.lower / (tFactor * 1000.0);
        int startTr = tr->getTrStartInput()->text().toInt();
        double trAbsStart_ms = (startTr - 1) * loader->getRepetitionTime_us() / 1000.0;
        double relStart_ms = absStart_ms - trAbsStart_ms; if (relStart_ms < 0) relStart_ms = 0;

        int sliderStart = tr->getTimeRangeSlider()->startValue();
        QVERIFY(std::abs(sliderStart - std::lround(relStart_ms)) <= 1);

        // Now pan right by wheel down (negative delta) without Ctrl
        QCPRange r1 = plot->xAxis->range();
        int prevSpy = spy.count();
        QWheelEvent panEv(
            QPointF(centerPt), QPointF(plot->mapToGlobal(centerPt)),
            QPoint(0, 0), QPoint(0, -120), // angleDelta down
            Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false
        );
        QApplication::sendEvent(plot, &panEv);
        QTest::qWait(20);
        QCOMPARE(spy.count(), prevSpy); // still no emissions

        // Validate slider moved accordingly
        QCPRange pr = plot->xAxis->range();
        double absStartPan_ms = pr.lower / (tFactor * 1000.0);
        double relStartPan_ms = absStartPan_ms - trAbsStart_ms; if (relStartPan_ms < 0) relStartPan_ms = 0;
        int sliderStart2 = tr->getTimeRangeSlider()->startValue();
        QVERIFY(std::abs(sliderStart2 - std::lround(relStartPan_ms)) <= 1);

        // Ensure pan actually changed the viewport lower bound (moved right)
        QVERIFY(pr.lower > r1.lower);
    }

    void test_slider_sync_no_signal_on_tr_changes()
    {
        MainWindow w;
        QString seqPath = resolveSeqPath();
        QVERIFY2(QFile::exists(seqPath), "Test sequence file not found");

        w.getPulseqLoader()->setSilentMode(true);
        w.show(); QTest::qWait(50);
        w.openFileFromCommandLine(seqPath);
        TRManager* tr = w.getTRManager();
        QVERIFY(tr);

        // Spy on time slider valuesChanged: changing TR inputs should not emit due to QSignalBlocker
        auto* slider = tr->getTimeRangeSlider();
        QSignalSpy spy(slider, SIGNAL(valuesChanged(int,int)));

        // Change TR start programmatically via input slot
        int startVal = tr->getTrStartInput()->text().toInt();
        tr->getTrStartInput()->setText(QString::number(startVal));
        tr->onTrStartInputChanged();
        tr->getTrEndInput()->setText(tr->getTrEndInput()->text());
        tr->onTrEndInputChanged();

        // No emissions expected
        QCOMPARE(spy.count(), 0);
    }

    void test_pan_respects_boundaries_whole_sequence()
    {
        MainWindow w;
        QString seqPath = resolveSeqPath();
        QVERIFY2(QFile::exists(seqPath), "Test sequence file not found");
        w.getPulseqLoader()->setSilentMode(true);
        w.show(); QTest::qWait(50);
        w.openFileFromCommandLine(seqPath);

        // Force whole-sequence mode
        w.getTRManager()->setRenderModeWholeSequence();

        QCustomPlot* plot = w.getUI()->customPlot;
        QPoint centerPt = plot->rect().center();

        // Aggressively pan right many times
        for (int i = 0; i < 50; ++i) {
            QWheelEvent ev(
                QPointF(centerPt), QPointF(plot->mapToGlobal(centerPt)),
                QPoint(0, 0), QPoint(0, -120),
                Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false
            );
            QApplication::sendEvent(plot, &ev);
        }
        QCPRange r = plot->xAxis->range();
        // Upper bound should not exceed data end
        auto loader = w.getPulseqLoader();
        double maxEnd = 0.0;
        const auto& edges = loader->getBlockEdges();
        if (!edges.isEmpty()) maxEnd = edges.last(); else maxEnd = loader->getTotalDuration_us() * loader->getTFactor();
        QVERIFY(r.upper <= maxEnd + 1e-6);

        // Aggressively pan left: should clamp to 0
        for (int i = 0; i < 100; ++i) {
            QWheelEvent ev(
                QPointF(centerPt), QPointF(plot->mapToGlobal(centerPt)),
                QPoint(0, 0), QPoint(0, 120),
                Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false
            );
            QApplication::sendEvent(plot, &ev);
        }
        r = plot->xAxis->range();
        QVERIFY(r.lower >= 0.0);
    }

    void test_tr_toggle_preserves_relative_window()
    {
        MainWindow w;
        QString seqPath = resolveSeqPath();
        QVERIFY2(QFile::exists(seqPath), "Test sequence file not found");

        w.getPulseqLoader()->setSilentMode(true);
        w.show(); QTest::qWait(50);
        w.openFileFromCommandLine(seqPath);

        TRManager* tr = w.getTRManager();
        PulseqLoader* loader = w.getPulseqLoader();
        QVERIFY(tr && loader);
        if (!loader->hasRepetitionTime()) {
            QSKIP("Sequence has no TR; skipping toggle test");
        }
        tr->setRenderModeTrSegmented();

        // Set a relative window [0, 371] ms via public inputs/slots
        tr->getTimeStartInput()->setText("0");
        tr->onTimeStartInputChanged();
        tr->getTimeEndInput()->setText("371");
        tr->onTimeEndInputChanged();
        QTest::qWait(50);
        // Toggle to TR 2
        tr->getTrStartInput()->setText("2");
        tr->onTrStartInputChanged();
        QTest::qWait(150); // allow debounce to fire

        // Expect slider to preserve previous end (~371ms) within new TR bounds
        int endVal = tr->getTimeRangeSlider()->endValue();
        QVERIFY(endVal >= 360 && endVal <= 380);
    }
};

QTEST_MAIN(TimeSliderSyncTest)
#include "TimeSliderSyncTest.moc"
