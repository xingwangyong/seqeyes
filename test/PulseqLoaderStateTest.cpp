#include <QtTest/QtTest>
#include <QtWidgets>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"

#include <memory>

class PulseqLoaderStateTest : public QObject
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

    static MainWindow* makeWindow()
    {
        MainWindow* window = new MainWindow();
        window->getPulseqLoader()->setSilentMode(true);
        window->show();
        QTest::qWait(50);
        return window;
    }

    static void verifyLoaded(PulseqLoader* loader, const QString& expectedPath)
    {
        const QString normalized = QFileInfo(expectedPath).absoluteFilePath();
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Loaded);
        QCOMPARE(loader->getLoadedPulseqFilePath(), normalized);
        QCOMPARE(loader->getReopenPulseqFilePath(), normalized);
        QVERIFY(loader->getSequence());
        QVERIFY(!loader->getDecodedSeqBlocks().empty());
    }

    static QAction* findRecentAction(MainWindow* window, const QString& path)
    {
        const QString normalized = QFileInfo(path).absoluteFilePath();
        if (!window || !window->ui || !window->ui->menuRecent_Files)
            return nullptr;

        for (QAction* action : window->ui->menuRecent_Files->actions()) {
            if (QFileInfo(action->data().toString()).absoluteFilePath() == normalized)
                return action;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        m_fileA = resolveSeq(QStringLiteral("writeGradientEcho.seq"));
        m_fileB = resolveSeq(QStringLiteral("writeFid.seq"));
        QVERIFY2(QFile::exists(m_fileA), qPrintable("Missing test file A: " + m_fileA));
        QVERIFY2(QFile::exists(m_fileB), qPrintable("Missing test file B: " + m_fileB));
    }

    void loadAThenLoadB_commitsB()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileB);
    }

    void loadSameFileTwice_isStable()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        const int firstBlockCount = static_cast<int>(loader->getDecodedSeqBlocks().size());
        const double firstDuration = loader->getTotalDuration_us();
        verifyLoaded(loader, m_fileB);

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileB);
        QCOMPARE(static_cast<int>(loader->getDecodedSeqBlocks().size()), firstBlockCount);
        QCOMPARE(loader->getTotalDuration_us(), firstDuration);
    }

    void failedLoad_afterValidLoad_returnsBlankAndPreservesReopenPath()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        const QString reopenPath = loader->getReopenPulseqFilePath();

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = dir.filePath(QStringLiteral("invalid.seq"));
        QFile invalidFile(invalidPath);
        QVERIFY2(invalidFile.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(invalidPath));
        invalidFile.write("[DEFINITIONS]\nName invalid\n");
        invalidFile.close();

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(loader->getLoadedPulseqFilePath().isEmpty());
        QCOMPARE(loader->getReopenPulseqFilePath(), reopenPath);
        QVERIFY(!loader->getSequence());
        QVERIFY(loader->getDecodedSeqBlocks().empty());
    }

    void emptyPath_isNoOp()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        QVERIFY(!loader->OpenPulseqFilePath(QString()));
        verifyLoaded(loader, m_fileA);
    }

    void commandLineOpen_commitsSameState()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        QVERIFY2(window->openFileFromCommandLine(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileB);
    }

    void recentFileAction_commitsSameState()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileB);

        QAction* action = findRecentAction(window.get(), m_fileA);
        QVERIFY2(action, qPrintable("Recent menu did not contain: " + m_fileA));
        action->trigger();
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
    }

private:
    QString m_fileA;
    QString m_fileB;
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    PulseqLoaderStateTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "PulseqLoaderStateTest.moc"
