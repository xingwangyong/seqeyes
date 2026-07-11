#include <QtTest/QtTest>
#include <QtWidgets>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#include "mainwindow.h"
#include "PulseqLoader.h"
#include "Settings.h"

class RfMultichannelDetectionTest : public QObject
{
    Q_OBJECT

private:
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

    static QStringList roosSeqCandidates()
    {
        return {
            QStringLiteral("gre2d_pTxSingleChan_8Tx.seq"),
            QStringLiteral("v142/gre2d_pTxSingleChan_8Tx.seq")
        };
    }

    static void settle(MainWindow& w)
    {
        PulseqLoader* loader = w.getPulseqLoader();
        loader->waitForBackgroundComputations(60000);
        QApplication::processEvents();
        QTest::qWait(100);
        QApplication::processEvents();
    }

private slots:
    void initTestCase()
    {
        m_normalSeq = resolveSeq(QStringLiteral("writeGradientEcho.seq"));
        QVERIFY2(QFile::exists(m_normalSeq), qPrintable("Missing normal sample: " + m_normalSeq));
        for (const QString& candidate : roosSeqCandidates()) {
            const QString resolved = resolveSeq(candidate);
            QVERIFY2(QFile::exists(resolved), qPrintable("Missing Roos sample: " + resolved));
            m_roosSeqs.push_back(resolved);
        }
    }

    void cleanup()
    {
        Settings::getInstance().setEnableRoosPtxHackAutoDetection(true);
    }

    void test_detects_roos_pattern()
    {
        Settings::getInstance().setEnableRoosPtxHackAutoDetection(true);
        for (const QString& roosSeq : m_roosSeqs) {
            MainWindow w;
            w.getPulseqLoader()->setSilentMode(true);
            w.show();
            QTest::qWait(50);

            QVERIFY2(w.getPulseqLoader()->LoadPulseqFile(roosSeq), qPrintable(roosSeq));
            settle(w);

            QVERIFY2(w.getPulseqLoader()->hasDetectedRoosPtxHack(), qPrintable(roosSeq));
            QCOMPARE(w.getPulseqLoader()->getUnifiedRfChannelCount(), 8);
            QVERIFY2(w.getPulseqLoader()->getUnifiedRfStatusMessage().contains(QStringLiteral("Detected RoosPtxHack")),
                      qPrintable(roosSeq));
        }
    }

    void test_does_not_false_positive_on_normal_sequence()
    {
        Settings::getInstance().setEnableRoosPtxHackAutoDetection(true);

        MainWindow w;
        w.getPulseqLoader()->setSilentMode(true);
        w.show();
        QTest::qWait(50);

        QVERIFY(w.getPulseqLoader()->LoadPulseqFile(m_normalSeq));
        settle(w);

        QVERIFY(!w.getPulseqLoader()->hasDetectedRoosPtxHack());
    }

private:
    QStringList m_roosSeqs;
    QString m_normalSeq;
};

static void rfTestRawLog(const char* msg)
{
    std::fwrite(msg, 1, std::strlen(msg), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

static void rfTestLogEnv(const char* name)
{
    const QByteArray value = qgetenv(name);
    const QByteArray line = QByteArray("[rf-test-main] ") + name + "=" + value;
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

static int rfTestInternalTimeoutMs()
{
    const QByteArray value = qgetenv("RF_TEST_INTERNAL_TIMEOUT_MS");
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok && parsed > 0 ? parsed : 210000;
}

int main(int argc, char** argv)
{
    std::thread([timeoutMs = rfTestInternalTimeoutMs()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        rfTestRawLog("[rf-test-main] internal timeout before normal QtTest exit");
        std::_Exit(124);
    }).detach();

    rfTestLogEnv("QT_QPA_PLATFORM");
    rfTestLogEnv("QT_QPA_PLATFORM_PLUGIN_PATH");
    rfTestRawLog("[rf-test-main] before QApplication");
    QApplication app(argc, argv);
    rfTestRawLog("[rf-test-main] after QApplication");

    RfMultichannelDetectionTest tc;
    rfTestRawLog("[rf-test-main] before qExec");
    const int rc = QTest::qExec(&tc, argc, argv);
    rfTestRawLog("[rf-test-main] after qExec");
    return rc;
}

#include "RfMultichannelDetectionTest.moc"
