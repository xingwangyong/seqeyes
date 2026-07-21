#include <QtTest/QtTest>
#include <QtWidgets>

#include "LogManager.h"
#include "PulseqLoader.h"
#include "Settings.h"
#include "mainwindow.h"

#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cmath>
#include <limits>
#include <memory>

class B0SystemProfileTest : public QObject
{
    Q_OBJECT

private:
    static QString resolveSeq(const QString& name)
    {
#ifdef SEQ_FILES_DIR
        {
            const QString path = QStringLiteral(SEQ_FILES_DIR) + "/" + name;
            if (QFile::exists(path))
                return QFileInfo(path).absoluteFilePath();
        }
#endif
        QString path = QCoreApplication::applicationDirPath() + "/../test/seq_files/" + name;
        if (!QFile::exists(path)) {
            path = QDir(QCoreApplication::applicationDirPath() + "/../../")
                       .absoluteFilePath("test/seq_files/" + name);
        }
        return QFileInfo(path).absoluteFilePath();
    }

    static QString readTextFile(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(file.readAll());
    }

    static QString writeTempSeq(QTemporaryDir& dir,
                                const QString& sourcePath,
                                const QString& fileName,
                                const QStringList& extraDefinitions = {},
                                bool patchFirstV15RfAdcPpm = false)
    {
        QString text = readTextFile(sourcePath);
        if (text.isEmpty())
            return QString();

        text.replace(QRegularExpression(QStringLiteral("(?m)^B0\\s+[^\\r\\n]*\\r?\\n")), QString());
        text.replace(QRegularExpression(QStringLiteral("(?m)^SystemName\\s+[^\\r\\n]*\\r?\\n")), QString());

        if (!extraDefinitions.isEmpty()) {
            const QString marker = QStringLiteral("[DEFINITIONS]\n");
            const int pos = text.indexOf(marker);
            if (pos < 0)
                return QString();
            text.insert(pos + marker.size(), extraDefinitions.join(QStringLiteral("\n")) + QStringLiteral("\n"));
        }

        if (patchFirstV15RfAdcPpm) {
            text.replace(QRegularExpression(QStringLiteral("(?m)^1\\s+37\\.2185\\s+1\\s+2\\s+0\\s+1500\\s+100\\s+0\\s+0\\s+0\\s+0\\s+e\\s*$")),
                         QStringLiteral("1      37.2185 1 2 0 1500 100 2 3 10 0.25 e"));
            text.replace(QRegularExpression(QStringLiteral("(?m)^1\\s+128\\s+25000\\s+40\\s+0\\s+0\\s+0\\s+0\\s+0\\s*$")),
                         QStringLiteral("1 128 25000 40 4 5 20 0.5 0"));
        }

        const QString path = dir.filePath(fileName);
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return QString();
        out.write(text.toUtf8());
        out.close();
        return QFileInfo(path).absoluteFilePath();
    }

    static Settings::SystemProfile profile(const QString& name, double b0Tesla)
    {
        Settings::SystemProfile p;
        p.alias = name;
        p.b0Tesla = b0Tesla;
        p.maxGrad = std::numeric_limits<double>::quiet_NaN();
        p.maxSlew = std::numeric_limits<double>::quiet_NaN();
        p.maxB1 = std::numeric_limits<double>::quiet_NaN();
        return p;
    }

    static MainWindow* makeWindow()
    {
        MainWindow* window = new MainWindow();
        PulseqLoader* loader = window->getPulseqLoader();
        loader->setSilentMode(true);
        loader->setAutoStartTrajectoryAfterLoad(false);
        loader->setAutoStartPnsAfterLoad(false);
        window->show();
        QTest::qWait(50);
        return window;
    }

    static QString blockInfoText(PulseqLoader* loader, int blockIndex)
    {
        EventBlockInfoDialog dialog;
        loader->setBlockInfoContent(&dialog, blockIndex);
        QPlainTextEdit* edit = dialog.findChild<QPlainTextEdit*>();
        return edit ? edit->toPlainText() : QString();
    }

    static double extractValueAfterLabel(const QString& text, const QString& label)
    {
        const QRegularExpression rx(QRegularExpression::escape(label)
                                    + QStringLiteral(":\\s*([-+0-9.eE]+)"));
        const QRegularExpressionMatch match = rx.match(text);
        if (!match.hasMatch())
            return std::numeric_limits<double>::quiet_NaN();
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        return ok ? value : std::numeric_limits<double>::quiet_NaN();
    }

    static bool recentLogsContainSince(int firstIndex, const QString& needle)
    {
        const QVector<LogManager::LogEntry> entries = LogManager::getInstance().getBufferedEntries();
        for (int i = std::max(0, firstIndex); i < entries.size(); ++i) {
            if (entries[i].message.contains(needle))
                return true;
        }
        return false;
    }

    static void verifyNear(double actual, double expected, double tolerance = 1e-2)
    {
        QVERIFY2(std::isfinite(actual),
                 qPrintable(QStringLiteral("Actual value is not finite; expected %1").arg(expected, 0, 'g', 14)));
        QVERIFY2(std::abs(actual - expected) <= tolerance,
                 qPrintable(QStringLiteral("Actual %1 differs from expected %2 by more than %3")
                                .arg(actual, 0, 'g', 14)
                                .arg(expected, 0, 'g', 14)
                                .arg(tolerance, 0, 'g', 14)));
    }

    void setProfiles(const QVector<Settings::SystemProfile>& profiles, const QString& activeName)
    {
        Settings& settings = Settings::getInstance();
        settings.setLogLevel(Settings::LogLevel::Warning);
        settings.setSystemProfiles(profiles);
        settings.setActiveSystemProfileAlias(activeName);
    }

private slots:
    void initTestCase()
    {
        m_originalProfiles = Settings::getInstance().getSystemProfiles();
        m_originalActiveProfile = Settings::getInstance().getActiveSystemProfileAlias();
        m_originalLogLevel = Settings::getInstance().getLogLevel();

        m_v142Gre = resolveSeq(QStringLiteral("v142/writeGradientEcho.seq"));
        m_v150Gre = resolveSeq(QStringLiteral("v150/writeGradientEcho.seq"));
        QVERIFY2(QFile::exists(m_v142Gre), qPrintable("Missing v1.4.x test file: " + m_v142Gre));
        QVERIFY2(QFile::exists(m_v150Gre), qPrintable("Missing v1.5 test file: " + m_v150Gre));
    }

    void cleanupTestCase()
    {
        Settings& settings = Settings::getInstance();
        settings.setSystemProfiles(m_originalProfiles);
        settings.setActiveSystemProfileAlias(m_originalActiveProfile);
        settings.setLogLevel(m_originalLogLevel);
    }

    void init()
    {
        setProfiles({profile(QStringLiteral("Active7T"), 7.0)}, QStringLiteral("Active7T"));
    }

    void v14WithoutB0_keepsLegacyOffsetsEvenWithActiveProfileB0()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_v142Gre), qPrintable(m_v142Gre));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 7.0);

        const QString info = blockInfoText(loader, 0);
        QVERIFY2(info.contains(QStringLiteral("RF Event:")), qPrintable(info));
        verifyNear(extractValueAfterLabel(info, QStringLiteral("Frequency Offset")), 0.0);
        verifyNear(extractValueAfterLabel(info, QStringLiteral("Phase Offset")), 0.0);
    }

    void v15PpmWithoutSequenceB0_usesActiveProfileB0ForOffsets()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempSeq(dir, m_v150Gre, QStringLiteral("active_b0.seq"), {}, true);
        QVERIFY2(QFile::exists(path), qPrintable(path));

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY2(loader->OpenPulseqFilePath(path), qPrintable(path));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 7.0);

        const double gamma = Settings::getInstance().getGamma();
        const QString rfInfo = blockInfoText(loader, 0);
        verifyNear(extractValueAfterLabel(rfInfo, QStringLiteral("Frequency Offset")),
                   10.0 + 2.0e-6 * gamma * 7.0);
        verifyNear(extractValueAfterLabel(rfInfo, QStringLiteral("Phase Offset")),
                   0.25 + 3.0e-6 * gamma * 7.0);

        const QString adcInfo = blockInfoText(loader, 3);
        verifyNear(extractValueAfterLabel(adcInfo, QStringLiteral("Frequency Offset")),
                   20.0 + 4.0e-6 * gamma * 7.0);
        verifyNear(extractValueAfterLabel(adcInfo, QStringLiteral("Phase Offset")),
                   0.5 + 5.0e-6 * gamma * 7.0);
    }

    void v15PpmWithSequenceB0_prefersSequenceB0()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempSeq(dir,
                                          m_v150Gre,
                                          QStringLiteral("sequence_b0.seq"),
                                          {QStringLiteral("B0 2.89")},
                                          true);
        QVERIFY2(QFile::exists(path), qPrintable(path));

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY2(loader->OpenPulseqFilePath(path), qPrintable(path));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 2.89);

        const double gamma = Settings::getInstance().getGamma();
        const QString rfInfo = blockInfoText(loader, 0);
        verifyNear(extractValueAfterLabel(rfInfo, QStringLiteral("Frequency Offset")),
                   10.0 + 2.0e-6 * gamma * 2.89);
    }

    void systemNameMatch_usesMatchedProfileB0InsteadOfActiveProfile()
    {
        setProfiles({profile(QStringLiteral("Other"), 7.0),
                     profile(QStringLiteral("Prisma"), 2.89)},
                    QStringLiteral("Other"));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempSeq(dir,
                                          m_v150Gre,
                                          QStringLiteral("system_name_match.seq"),
                                          {QStringLiteral("SystemName Prisma")},
                                          true);
        QVERIFY2(QFile::exists(path), qPrintable(path));

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY2(loader->OpenPulseqFilePath(path), qPrintable(path));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 2.89);
    }

    void systemNameMissing_fallsBackToActiveProfileAndLogsWarning()
    {
        setProfiles({profile(QStringLiteral("Other"), 7.0)}, QStringLiteral("Other"));
        const int logStart = LogManager::getInstance().getBufferedEntries().size();

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempSeq(dir,
                                          m_v150Gre,
                                          QStringLiteral("system_name_missing.seq"),
                                          {QStringLiteral("SystemName Prisma")},
                                          true);
        QVERIFY2(QFile::exists(path), qPrintable(path));

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY2(loader->OpenPulseqFilePath(path), qPrintable(path));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 7.0);
        QVERIFY(recentLogsContainSince(logStart,
                                       QStringLiteral("Sequence requests SystemName \"Prisma\"")));
    }

    void sequenceB0ConflictWithMatchedProfile_logsWarningAndKeepsSequenceB0()
    {
        setProfiles({profile(QStringLiteral("Prisma"), 3.0)}, QStringLiteral("Prisma"));
        const int logStart = LogManager::getInstance().getBufferedEntries().size();

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempSeq(dir,
                                          m_v150Gre,
                                          QStringLiteral("b0_conflict.seq"),
                                          {QStringLiteral("SystemName Prisma"),
                                           QStringLiteral("B0 2.89")},
                                          true);
        QVERIFY2(QFile::exists(path), qPrintable(path));

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY2(loader->OpenPulseqFilePath(path), qPrintable(path));
        QVERIFY(loader->waitForBackgroundComputations());
        QCOMPARE(loader->getB0Tesla(), 2.89);
        QVERIFY(recentLogsContainSince(logStart,
                                       QStringLiteral("SeqEyes will use the sequence-defined B0")));
    }

private:
    QString m_v142Gre;
    QString m_v150Gre;
    QVector<Settings::SystemProfile> m_originalProfiles;
    QString m_originalActiveProfile;
    Settings::LogLevel m_originalLogLevel {Settings::LogLevel::Warning};
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("SeqEyesTest"));
    app.setApplicationName(QStringLiteral("B0SystemProfileTest"));
    B0SystemProfileTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "B0SystemProfileTest.moc"
