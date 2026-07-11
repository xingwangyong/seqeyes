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
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <QElapsedTimer>

class ReopenEquivalenceTest : public QObject
{
    Q_OBJECT

private:
    static bool verboseLogs()
    {
        return qEnvironmentVariableIsSet("REOPEN_TEST_VERBOSE");
    }

    static void logStep(const QString& msg)
    {
        if (!verboseLogs())
            return;
        const QByteArray line = QStringLiteral("[reopen] %1\n").arg(msg).toLocal8Bit();
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
        std::fflush(stderr);
    }

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
        QElapsedTimer timer;
        timer.start();
        logStep(QStringLiteral("settle: waiting for background computations"));
        loader->waitForBackgroundComputations(60000);
        // Normalize the viewport identically before every snapshot so that the
        // decimated waveform graphs are a function of the file only, not of
        // whatever range happened to be active.
        if (w.getTRManager())
            w.getTRManager()->setRenderModeWholeSequence();
        QApplication::processEvents();
        QTest::qWait(150);
        QApplication::processEvents();
        logStep(QStringLiteral("settle: done in %1 ms").arg(timer.elapsed()));
    }

    static bool loadAndSettle(MainWindow& w, const QString& seqPath, const QString& phase)
    {
        logStep(QStringLiteral("%1: loading %2").arg(phase, seqPath));
        if (!w.getPulseqLoader()->LoadPulseqFile(seqPath))
            return false;
        settle(w);
        logStep(QStringLiteral("%1: load complete").arg(phase));
        return true;
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

        // Component-agnostic canvas inventory. Instead of naming components, we
        // walk the widget tree and capture the *containers* of visible output:
        // every QCustomPlot's plottables and items, and every QLabel's text.
        // Whatever component leaks state ends up in one of these containers, so
        // an orphan/leaked plottable or item (or stale label text) shows up as
        // an extra/changed entry -- no need to know which component produced it.
        //
        // We deliberately record per-plottable point COUNT (g->dataCount()), not
        // the decimated data values, because the decimated graph data is pixel-
        // width sensitive and varies with layout timing (a false-positive
        // source). Value-level correctness is covered by the raw hashes above.
        QStringList canvasLines;
        const QList<QCustomPlot*> plots = w.findChildren<QCustomPlot*>();
        for (QCustomPlot* plot : plots)
        {
            const QString pid = plot->objectName().isEmpty()
                                    ? QStringLiteral("plot")
                                    : plot->objectName();

            for (int i = 0; i < plot->plottableCount(); ++i)
            {
                QCPAbstractPlottable* pl = plot->plottable(i);
                if (!pl)
                    continue;
                QString row = QStringLiteral("?");
                if (pl->valueAxis() && pl->valueAxis()->axisRect())
                {
                    if (QCPAxis* la = pl->valueAxis()->axisRect()->axis(QCPAxis::atLeft))
                        row = la->label();
                }
                int n = -1;
                if (QCPGraph* g = qobject_cast<QCPGraph*>(pl))
                    n = g->dataCount();
                // No name: QCustomPlot auto-names graphs "Graph N" by creation
                // order, which is not a stable identity across reopen (graphs are
                // removed/re-added), so it would create false positives. The
                // sorted multiset of (type,row,vis,n) is the stable signal; an
                // orphan/leaked plottable still shows up as an extra entry.
                canvasLines << QStringLiteral("%1 plottable type=%2 row=%3 vis=%4 n=%5")
                                   .arg(pid)
                                   .arg(QString::fromLatin1(pl->metaObject()->className()))
                                   .arg(row)
                                   .arg(pl->visible() ? 1 : 0)
                                   .arg(n);
            }

            for (int i = 0; i < plot->itemCount(); ++i)
            {
                QCPAbstractItem* item = plot->item(i);
                if (!item)
                    continue;
                QString posStr;
                const auto positions = item->positions();
                for (QCPItemPosition* pos : positions)
                    posStr += QStringLiteral("(%1,%2)").arg(pos->key(), 0, 'g', 8)
                                  .arg(pos->value(), 0, 'g', 8);
                canvasLines << QStringLiteral("%1 item type=%2 vis=%3 pos=%4")
                                   .arg(pid)
                                   .arg(QString::fromLatin1(item->metaObject()->className()))
                                   .arg(item->visible() ? 1 : 0)
                                   .arg(posStr);
            }
        }
        canvasLines.sort();
        out += canvasLines;

        // Every text label (status bar coord readout, version, TR labels, ...).
        // For A-B-A on the same file these must match; a stale label from the
        // previous file would differ here.
        QStringList labelLines;
        for (QLabel* lbl : w.findChildren<QLabel*>())
        {
            const QString id = lbl->objectName().isEmpty() ? QStringLiteral("?") : lbl->objectName();
            labelLines << QStringLiteral("label[%1]=%2").arg(id, lbl->text());
        }
        labelLines.sort();
        out += labelLines;

        return out;
    }

    static void failSnapshotMismatch(const QString& label,
                                     const QStringList& actual,
                                     const QStringList& expected)
    {
        logStep(QStringLiteral("%1: snapshot mismatch actual=%2 expected=%3")
                    .arg(label)
                    .arg(actual.size())
                    .arg(expected.size()));

        const int shared = std::min(actual.size(), expected.size());
        for (int i = 0; i < shared; ++i)
        {
            if (actual[i] != expected[i])
            {
                logStep(QStringLiteral("%1: first diff at index %2").arg(label).arg(i));
                logStep(QStringLiteral("%1: actual   %2").arg(label, actual[i]));
                logStep(QStringLiteral("%1: expected %2").arg(label, expected[i]));
                QFAIL(qPrintable(QStringLiteral("%1: snapshot mismatch at index %2").arg(label).arg(i)));
            }
        }

        if (actual.size() != expected.size())
        {
            if (shared < actual.size())
                logStep(QStringLiteral("%1: first extra actual line %2").arg(label, actual[shared]));
            if (shared < expected.size())
                logStep(QStringLiteral("%1: first missing actual line expected=%2").arg(label, expected[shared]));
            QFAIL(qPrintable(QStringLiteral("%1: snapshot size mismatch actual=%2 expected=%3")
                                 .arg(label)
                                 .arg(actual.size())
                                 .arg(expected.size())));
        }

        QFAIL(qPrintable(QStringLiteral("%1: snapshot mismatch").arg(label)));
    }

    static void assertSnapshotsEqual(const QString& label,
                                     const QStringList& actual,
                                     const QStringList& expected)
    {
        if (actual == expected)
        {
            logStep(QStringLiteral("%1: snapshots match").arg(label));
            return;
        }
        failSnapshotMismatch(label, actual, expected);
    }

private slots:
    void initTestCase()
    {
        m_fileA = seqFromEnv("REOPEN_SEQ_A", QStringLiteral("writeGradientEcho_label.seq"));
        m_fileB = seqFromEnv("REOPEN_SEQ_B", QStringLiteral("writeFid.seq"));
        logStep(QStringLiteral("initTestCase: fileA=%1").arg(m_fileA));
        logStep(QStringLiteral("initTestCase: fileB=%1").arg(m_fileB));
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

        QVERIFY2(loadAndSettle(w, m_fileB, QStringLiteral("A_then_B/cold_B")), qPrintable(m_fileB));
        const QStringList coldB = captureSnapshot(w);
        logStep(QStringLiteral("A_then_B: cold_B snapshot lines=%1").arg(coldB.size()));

        QVERIFY2(loadAndSettle(w, m_fileA, QStringLiteral("A_then_B/open_A")), qPrintable(m_fileA));

        QVERIFY2(loadAndSettle(w, m_fileB, QStringLiteral("A_then_B/reopen_B")), qPrintable(m_fileB));
        const QStringList reopenB = captureSnapshot(w);
        logStep(QStringLiteral("A_then_B: reopen_B snapshot lines=%1").arg(reopenB.size()));

        assertSnapshotsEqual(QStringLiteral("A_then_B"), reopenB, coldB);
    }

    // open(B) cold  ==  open(B); open(B)   (same-file reopen)
    void test_same_file_reopen()
    {
        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY2(loadAndSettle(w, m_fileB, QStringLiteral("same_file/first_B")), qPrintable(m_fileB));
        const QStringList first = captureSnapshot(w);
        logStep(QStringLiteral("same_file: first snapshot lines=%1").arg(first.size()));

        QVERIFY2(loadAndSettle(w, m_fileB, QStringLiteral("same_file/second_B")), qPrintable(m_fileB));
        const QStringList second = captureSnapshot(w);
        logStep(QStringLiteral("same_file: second snapshot lines=%1").arg(second.size()));

        assertSnapshotsEqual(QStringLiteral("same_file"), second, first);
    }

    // Symmetric direction: open(A) cold  ==  open(B); open(A)
    void test_history_independence_B_then_A()
    {
        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY2(loadAndSettle(w, m_fileA, QStringLiteral("B_then_A/cold_A")), qPrintable(m_fileA));
        const QStringList coldA = captureSnapshot(w);
        logStep(QStringLiteral("B_then_A: cold_A snapshot lines=%1").arg(coldA.size()));

        QVERIFY2(loadAndSettle(w, m_fileB, QStringLiteral("B_then_A/open_B")), qPrintable(m_fileB));

        QVERIFY2(loadAndSettle(w, m_fileA, QStringLiteral("B_then_A/reopen_A")), qPrintable(m_fileA));
        const QStringList reopenA = captureSnapshot(w);
        logStep(QStringLiteral("B_then_A: reopen_A snapshot lines=%1").arg(reopenA.size()));

        assertSnapshotsEqual(QStringLiteral("B_then_A"), reopenA, coldA);
    }

private:
    QString m_fileA;
    QString m_fileB;
};

static void reopenRawLog(const char* msg)
{
    std::fwrite(msg, 1, std::strlen(msg), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

static void reopenRawLogEnv(const char* name)
{
    const QByteArray value = qgetenv(name);
    const QByteArray line = QByteArray("[reopen-main] ") + name + "=" + value;
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

static int reopenInternalTimeoutMs()
{
    const QByteArray value = qgetenv("REOPEN_TEST_INTERNAL_TIMEOUT_MS");
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok && parsed > 0 ? parsed : 210000;
}

int main(int argc, char** argv)
{
    std::thread([timeoutMs = reopenInternalTimeoutMs()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        reopenRawLog("[reopen-main] internal timeout before normal QtTest exit");
        std::_Exit(124);
    }).detach();

    reopenRawLogEnv("QT_QPA_PLATFORM");
    reopenRawLogEnv("QT_QPA_PLATFORM_PLUGIN_PATH");
    reopenRawLog("[reopen-main] before QApplication");
    QApplication app(argc, argv);
    reopenRawLog("[reopen-main] after QApplication");

    ReopenEquivalenceTest tc;
    reopenRawLog("[reopen-main] before qExec");
    const int rc = QTest::qExec(&tc, argc, argv);
    reopenRawLog("[reopen-main] after qExec");
    return rc;
}

#include "ReopenEquivalenceTest.moc"
