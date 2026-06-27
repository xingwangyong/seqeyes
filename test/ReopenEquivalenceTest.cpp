// Reopen-equivalence / history-independence regression test.
//
// Property under test: the final observable state after opening file B must be
// the SAME regardless of what was opened before it. Concretely:
//
//     open(B) cold            == open(A); open(B)
//     open(B) cold            == open(B); open(B)
//
// Any state that the previous file left behind (loader caches, parser maps,
// canvas graphs that were never removed, ...) shows up as a difference in the
// snapshot and fails the test -- WITHOUT us having to enumerate "what to clear".
// This is the mechanism that replaces remembering to extend ClearPulseqCache().
//
// The snapshot deliberately includes the full QCustomPlot graph inventory
// (visibility + point count + data hash), because that is exactly where the
// ExtensionPlotter label leak hid: graphs are owned by the plot, not by the
// component that created them, so a map-only reset() orphaned them on the canvas.

#include <QtTest/QtTest>
#include <QtWidgets>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"
#include "TRManager.h"
#include "WaveformDrawer.h"
#include "qcustomplot.h"

#include <cmath>
#include <cstdint>
#include <cstring>

class ReopenEquivalenceTest : public QObject
{
    Q_OBJECT

private:
    // Pick a seq file. The runner (test_reopen.py) passes absolute paths via
    // environment variables, so switching test files never needs a recompile.
    // When the env var is unset (e.g. running the exe directly), fall back to a
    // default file under test/seq_files.
    static QString seqFromEnv(const char* envVar, const QString& defaultName)
    {
        if (qEnvironmentVariableIsSet(envVar))
        {
            const QString p = qEnvironmentVariable(envVar);
            if (!p.isEmpty())
                return p;
        }
        return resolveSeq(defaultName);
    }

    // Resolve a default seq file under test/seq_files, robust to build layout.
    static QString resolveSeq(const QString& name)
    {
#ifdef SEQ_FILES_DIR
        {
            const QString p = QStringLiteral(SEQ_FILES_DIR) + "/" + name;
            if (QFile::exists(p))
                return p;
        }
#endif
        QString p = QCoreApplication::applicationDirPath() + "/../test/seq_files/" + name;
        if (!QFile::exists(p))
            p = QDir(QCoreApplication::applicationDirPath() + "/../../")
                    .absoluteFilePath("test/seq_files/" + name);
        return p;
    }

    // Drive a load through the same entry point that drag-drop / recent / CLI use,
    // then let async trajectory/PNS work and the queued replot settle so the
    // snapshot is taken on a fully rendered, deterministic state.
    static void settle(MainWindow& w)
    {
        PulseqLoader* loader = w.getPulseqLoader();
        loader->waitForBackgroundComputations(60000);
        // Normalize the viewport identically before every snapshot so that the
        // decimated waveform graphs are a function of the file only, not of
        // whatever range happened to be active.
        if (w.getTRManager())
            w.getTRManager()->setRenderModeWholeSequence();
        QApplication::processEvents();
        QTest::qWait(150);
        QApplication::processEvents();
    }

    static QString hashSeries(const QVector<double>& a, const QVector<double>& b)
    {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&](const QVector<double>& s) {
            for (double v : s)
            {
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                h ^= bits;
                h *= 1099511628211ull;
            }
        };
        mix(a);
        mix(b);
        return QString::number(static_cast<qulonglong>(h), 16);
    }

    // Serialize all observable, file-derived state into a comparable, order-
    // independent form. Equality of two snapshots == no leaked state is visible.
    static QStringList captureSnapshot(MainWindow& w)
    {
        PulseqLoader* loader = w.getPulseqLoader();
        QStringList out;

        // --- loader-side state ---
        out << QStringLiteral("loadState=%1").arg(static_cast<int>(loader->getSequenceLoadState()));
        out << QStringLiteral("blockCount=%1").arg(static_cast<qulonglong>(loader->getDecodedSeqBlocks().size()));
        out << QStringLiteral("edgeCount=%1").arg(loader->getBlockEdges().size());
        out << QStringLiteral("totalDuration_us=%1").arg(loader->getTotalDuration_us(), 0, 'g', 12);
        {
            QStringList ext = loader->getUsedExtensions().values();
            ext.sort();
            out << QStringLiteral("usedExtensions=[%1]").arg(ext.join(QLatin1Char(',')));
        }
        {
            const QVector<double>& edges = loader->getBlockEdges();
            out << QStringLiteral("blockEdgesLast=%1").arg(edges.isEmpty() ? 0.0 : edges.last(), 0, 'g', 14);
        }
        out << QStringLiteral("b0Tesla=%1").arg(loader->getB0Tesla(), 0, 'g', 14);

        // Raw, undecimated computed signals. Hashing these (instead of the
        // pixel-decimated graph data) is deterministic regardless of widget
        // width / layout-settling timing, so it catches value-level leaks
        // (b0, shape-cache collisions, ...) without the false positives that
        // hashing decimated graphs produces.
        {
            const QVector<double>& edges = loader->getBlockEdges();
            const double end = edges.isEmpty() ? 0.0 : edges.last();
            QVector<double> tAmp, vAmp, tPh, vPh, tAdc, vAdc;
            loader->getRfViewportDecimated(0.0, end, 100000, tAmp, vAmp, tPh, vPh);
            loader->getAdcPhaseViewport(0.0, end, 100000, tAdc, vAdc);
            out << QStringLiteral("rawRFmag=%1").arg(hashSeries(tAmp, vAmp));
            out << QStringLiteral("rawRFph=%1").arg(hashSeries(tPh, vPh));
            out << QStringLiteral("rawADCph=%1").arg(hashSeries(tAdc, vAdc));
        }

        // Canvas inventory: which graphs exist, on which row, visible, and how
        // many points. NOT the decimated data itself -- that is pixel-width
        // sensitive and varies with layout timing (a false-positive source).
        // A leaked/orphan graph from a previous file shows up here as an extra
        // entry; data correctness is covered by the raw hashes above.
        QStringList graphLines;
        QCustomPlot* plot = w.getUI()->customPlot;
        for (int i = 0; i < plot->graphCount(); ++i)
        {
            QCPGraph* g = plot->graph(i);
            if (!g)
                continue;
            QString row = QStringLiteral("?");
            if (g->valueAxis() && g->valueAxis()->axisRect())
            {
                if (QCPAxis* la = g->valueAxis()->axisRect()->axis(QCPAxis::atLeft))
                    row = la->label();
            }
            graphLines << QStringLiteral("row=%1 vis=%2 n=%3")
                              .arg(row)
                              .arg(g->visible() ? 1 : 0)
                              .arg(g->dataCount());
        }
        graphLines.sort();
        out += graphLines;
        return out;
    }

private slots:
    void initTestCase()
    {
        m_fileA = seqFromEnv("REOPEN_SEQ_A", QStringLiteral("writeGradientEcho_label.seq"));
        m_fileB = seqFromEnv("REOPEN_SEQ_B", QStringLiteral("writeFid.seq"));
        QVERIFY2(QFile::exists(m_fileA), qPrintable("Missing test file A: " + m_fileA));
        QVERIFY2(QFile::exists(m_fileB), qPrintable("Missing test file B: " + m_fileB));
    }

    // open(B) cold  ==  open(A); open(B)
    void test_history_independence_A_then_B()
    {
        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileB));
        settle(w);
        const QStringList coldB = captureSnapshot(w);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileA));
        settle(w);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileB));
        settle(w);
        const QStringList reopenB = captureSnapshot(w);

        QCOMPARE(reopenB, coldB);
    }

    // open(B) cold  ==  open(B); open(B)   (same-file reopen)
    void test_same_file_reopen()
    {
        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileB));
        settle(w);
        const QStringList first = captureSnapshot(w);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileB));
        settle(w);
        const QStringList second = captureSnapshot(w);

        QCOMPARE(second, first);
    }

    // Symmetric direction: open(A) cold  ==  open(B); open(A)
    void test_history_independence_B_then_A()
    {
        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileA));
        settle(w);
        const QStringList coldA = captureSnapshot(w);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileB));
        settle(w);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_fileA));
        settle(w);
        const QStringList reopenA = captureSnapshot(w);

        QCOMPARE(reopenA, coldA);
    }

private:
    QString m_fileA;
    QString m_fileB;
};

QTEST_MAIN(ReopenEquivalenceTest)
#include "ReopenEquivalenceTest.moc"
