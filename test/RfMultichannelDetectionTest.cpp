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

    static QString buildMixedRfShimAndRoosFile(const QString& sourcePath)
    {
        QFile f(sourcePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();

        QString text = QString::fromUtf8(f.readAll());
        f.close();

        text.replace(QStringLiteral("minor 4"), QStringLiteral("minor 5"));

        QRegularExpression firstBlockRe(
            QStringLiteral("^(\\s*1\\s+418\\s+1\\s+0\\s+0\\s+1\\s+0\\s+)0(\\s*)$"),
            QRegularExpression::MultilineOption);
        text.replace(firstBlockRe, QStringLiteral("\\11\\2"));

        const QString extSection =
            QStringLiteral("[EXTENSIONS]\n"
                           "1 1 1 0\n"
                           "extension RF_SHIMS 1\n"
                           "1 1 1.0 0.0\n\n");
        text.replace(QStringLiteral("[RF]\n"), extSection + QStringLiteral("[RF]\n"));

        QTemporaryFile tmp(QDir::tempPath() + "/seqeyes_roos_rfshim_XXXXXX.seq");
        tmp.setAutoRemove(false);
        if (!tmp.open())
            return QString();
        tmp.write(text.toUtf8());
        tmp.close();
        return tmp.fileName();
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

    void test_rejects_mixed_rf_shim_and_roos()
    {
        Settings::getInstance().setEnableRoosPtxHackAutoDetection(true);

        for (const QString& roosSeq : m_roosSeqs) {
            const QString mixedSeq = buildMixedRfShimAndRoosFile(roosSeq);
            QVERIFY2(!mixedSeq.isEmpty(), qPrintable("Failed to build temporary mixed RF sample from: " + roosSeq));

            MainWindow w;
            w.getPulseqLoader()->setSilentMode(true);
            w.show();
            QTest::qWait(50);

            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression(QStringLiteral("Illegal RF multi-channel combination detected")));
            QVERIFY2(!w.getPulseqLoader()->LoadPulseqFile(mixedSeq), qPrintable(mixedSeq));

            QFile::remove(mixedSeq);
        }
    }

private:
    QStringList m_roosSeqs;
    QString m_normalSeq;
};

QTEST_MAIN(RfMultichannelDetectionTest)
#include "RfMultichannelDetectionTest.moc"
