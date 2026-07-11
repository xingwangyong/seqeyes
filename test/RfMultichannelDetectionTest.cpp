#include <QtTest/QtTest>
#include <QtWidgets>

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

QTEST_MAIN(RfMultichannelDetectionTest)
#include "RfMultichannelDetectionTest.moc"
