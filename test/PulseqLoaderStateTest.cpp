#include <QtTest/QtTest>
#include <QtWidgets>

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "PulseqLoader.h"

#include <memory>
#include <QSettings>

class PulseqLoaderStateTest : public QObject
{
    Q_OBJECT

private:
    static QString writeInvalidSeq(QTemporaryDir& dir, const QString& name)
    {
        const QString invalidPath = dir.filePath(name);
        QFile invalidFile(invalidPath);
        if (!invalidFile.open(QIODevice::WriteOnly | QIODevice::Text))
            return QString();
        invalidFile.write("[DEFINITIONS]\nName invalid\n");
        invalidFile.close();
        return invalidPath;
    }

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
        PulseqLoader* loader = window->getPulseqLoader();
        loader->setSilentMode(true);
        loader->setAutoStartTrajectoryAfterLoad(false);
        loader->setAutoStartPnsAfterLoad(false);
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

    static void verifyBlank(PulseqLoader* loader, const QString& expectedReopenPath = QString())
    {
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(loader->getLoadedPulseqFilePath().isEmpty());
        QCOMPARE(loader->getReopenPulseqFilePath(), expectedReopenPath);
        QVERIFY(!loader->getSequence());
        QVERIFY(loader->getDecodedSeqBlocks().empty());
        QVERIFY(loader->getBlockEdges().isEmpty());
        QCOMPARE(loader->getTotalDuration_us(), 0.0);
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

    static QStringList recentActionPaths(MainWindow* window)
    {
        QStringList paths;
        if (!window || !window->ui || !window->ui->menuRecent_Files)
            return paths;

        for (QAction* action : window->ui->menuRecent_Files->actions()) {
            const QString path = action->data().toString();
            if (!path.isEmpty())
                paths << QFileInfo(path).absoluteFilePath();
        }
        return paths;
    }

    static void dropFileOnWindow(MainWindow* window, const QString& path)
    {
        auto mimeData = std::make_unique<QMimeData>();
        mimeData->setUrls({QUrl::fromLocalFile(path)});

        QDragEnterEvent dragEnterEvent(
            QPoint(10, 10),
            Qt::CopyAction,
            mimeData.get(),
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(window, &dragEnterEvent);
        QVERIFY(dragEnterEvent.isAccepted());

        QDropEvent dropEvent(
            QPointF(10, 10),
            Qt::CopyAction,
            mimeData.get(),
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(window, &dropEvent);
    }

private slots:
    void initTestCase()
    {
        m_fileA = resolveSeq(QStringLiteral("writeGradientEcho.seq"));
        m_fileB = resolveSeq(QStringLiteral("writeFid.seq"));
        QVERIFY2(QFile::exists(m_fileA), qPrintable("Missing test file A: " + m_fileA));
        QVERIFY2(QFile::exists(m_fileB), qPrintable("Missing test file B: " + m_fileB));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
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
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(loader->getLoadedPulseqFilePath().isEmpty());
        QCOMPARE(loader->getReopenPulseqFilePath(), reopenPath);
        QVERIFY(!loader->getSequence());
        QVERIFY(loader->getDecodedSeqBlocks().empty());
    }

    void failedLoad_fromBlankDoesNotCommitCandidatePath()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(loader->getLoadedPulseqFilePath().isEmpty());
        QVERIFY(loader->getReopenPulseqFilePath().isEmpty());
        QVERIFY(!loader->getSequence());
        QVERIFY(loader->getDecodedSeqBlocks().empty());
    }

    void decodeFailure_fromBlankLeavesSameBlankSnapshot()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString path = dir.filePath(QStringLiteral("decode_failure.seq"));
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Text), qPrintable(path));
        file.write(
            "[VERSION]\n"
            "major 1\n"
            "minor 5\n"
            "revision 1\n\n"
            "[DEFINITIONS]\n"
            "AdcRasterTime 1e-07\n"
            "BlockDurationRaster 1e-05\n"
            "GradientRasterTime 1e-05\n"
            "RadiofrequencyRasterTime 1e-06\n\n"
            "[BLOCKS]\n"
            "1 10 1 0 0 0 0 0\n\n"
            "[RF]\n"
            "1 1 1 2 3 0 0 0 0 0 0 e\n\n"
            "[SHAPES]\n"
            "shape_id 1\n"
            "num_samples 5\n"
            "0\n"
            "0\n"
            "0.5\n"
            "\n"
            "shape_id 2\n"
            "num_samples 2\n"
            "0\n"
            "0\n"
            "\n"
            "shape_id 3\n"
            "num_samples 2\n"
            "0\n"
            "1\n"
            "\n"
            "[SIGNATURE]\n"
            "Type md5\n"
            "Hash ignored\n");
        file.close();

        QVERIFY(!loader->OpenPulseqFilePath(path));
        verifyBlank(loader);
    }

    void derivedMetadataFailure_afterValidLoad_returnsBlankAndPreservesReopenPath()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        const QString reopenPath = loader->getReopenPulseqFilePath();

        loader->setForceDerivedMetadataFailureForTesting(true);
        const OpenResult result = loader->LoadPulseqFileResult(m_fileB);
        loader->setForceDerivedMetadataFailureForTesting(false);

        QVERIFY(!result.ok);
        QCOMPARE(result.errorTitle, QStringLiteral("Pulseq Load Error"));
        QVERIFY(result.errorMessage.contains(QStringLiteral("derived Pulseq metadata")));
        verifyBlank(loader, reopenPath);
    }

    void openSuccessAndFailure_leaveWindowEnabled()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QVERIFY(window->isEnabled());

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        QVERIFY(window->isEnabled());

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(window->isEnabled());
    }

    void progressHidden_afterSuccessAndFailure()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        QVERIFY(window->getProgressBar());
        QVERIFY(!window->getProgressBar()->isVisible());

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QVERIFY(!window->getProgressBar()->isVisible());
    }

    void titleSetOnlyAfterSuccessAndClearedOnBlankFailure()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();
        QCOMPARE(window->windowTitle(), QStringLiteral("SeqEyes"));

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        QVERIFY(window->windowTitle().startsWith(QStringLiteral("SeqEyes - ")));

        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");

        QVERIFY(!loader->OpenPulseqFilePath(invalidPath));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
        QCOMPARE(window->windowTitle(), QStringLiteral("SeqEyes"));
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

    void dragDropOpen_commitsSameState()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        dropFileOnWindow(window.get(), m_fileB);
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

    void reopenAction_commitsSameState()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        const int firstBlockCount = static_cast<int>(loader->getDecodedSeqBlocks().size());

        loader->ReOpenPulseqFile();
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        QCOMPARE(static_cast<int>(loader->getDecodedSeqBlocks().size()), firstBlockCount);
    }

    void recentListOrder_isMostRecentFirst()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileB);

        const QStringList paths = recentActionPaths(window.get());
        QVERIFY2(paths.size() >= 2, qPrintable(QStringLiteral("Recent path count: %1").arg(paths.size())));
        QCOMPARE(paths[0], QFileInfo(m_fileB).absoluteFilePath());
        QCOMPARE(paths[1], QFileInfo(m_fileA).absoluteFilePath());
    }

    void failedRecentOpen_keepsExistingRecentEntryAndPreservesReopenPath()
    {
        QTemporaryDir dir;
        QVERIFY2(dir.isValid(), "Could not create temporary directory");
        const QString invalidPath = writeInvalidSeq(dir, QStringLiteral("recent_invalid.seq"));
        QVERIFY2(!invalidPath.isEmpty(), "Could not create invalid sequence file");
        const QString normalizedInvalidPath = QFileInfo(invalidPath).absoluteFilePath();

        QSettings settings;
        settings.setValue(QStringLiteral("recentPulseqFiles"), QStringList{normalizedInvalidPath});

        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        QVERIFY(loader->waitForBackgroundComputations());
        verifyLoaded(loader, m_fileA);
        const QString reopenPath = loader->getReopenPulseqFilePath();

        QAction* invalidAction = findRecentAction(window.get(), normalizedInvalidPath);
        QVERIFY2(invalidAction, qPrintable("Recent menu did not contain invalid path: " + normalizedInvalidPath));
        invalidAction->trigger();

        verifyBlank(loader, reopenPath);
        QVERIFY(recentActionPaths(window.get()).contains(normalizedInvalidPath));
    }

    void staleAsyncResults_afterNewLoadDoNotOverwriteCurrentState()
    {
        std::unique_ptr<MainWindow> window(makeWindow());
        PulseqLoader* loader = window->getPulseqLoader();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileA), qPrintable(m_fileA));
        verifyLoaded(loader, m_fileA);
        const std::uint64_t oldGeneration = loader->asyncSequenceGenerationForTesting();
        const std::uint64_t oldTrajectoryRequest = loader->activeTrajectoryRequestIdForTesting();
        const std::uint64_t oldPnsRequest = loader->activePnsRequestIdForTesting();
        const std::uint64_t oldM1Request = loader->activeM1RequestIdForTesting();

        QVERIFY2(loader->OpenPulseqFilePath(m_fileB), qPrintable(m_fileB));
        verifyLoaded(loader, m_fileB);
        const std::uint64_t currentGeneration = loader->asyncSequenceGenerationForTesting();
        QVERIFY(currentGeneration != oldGeneration);

        KSpaceTrajectory::Result currentTrajectory;
        currentTrajectory.kx = {2.0};
        currentTrajectory.ky = {2.0};
        currentTrajectory.kz = {2.0};
        currentTrajectory.t = {0.002};
        loader->injectTrajectoryResultForTesting(
            currentTrajectory,
            currentGeneration,
            loader->activeTrajectoryRequestIdForTesting());
        QCOMPARE(loader->getTrajectoryKx(), QVector<double>({2.0}));

        KSpaceTrajectory::Result staleTrajectory;
        staleTrajectory.kx = {99.0};
        staleTrajectory.ky = {99.0};
        staleTrajectory.kz = {99.0};
        staleTrajectory.t = {0.099};
        loader->injectTrajectoryResultForTesting(staleTrajectory, oldGeneration, oldTrajectoryRequest);
        QCOMPARE(loader->getTrajectoryKx(), QVector<double>({2.0}));

        PnsCalculator::Result currentPns;
        currentPns.valid = true;
        currentPns.ok = true;
        currentPns.timeSec = {0.002};
        currentPns.pnsNorm = {2.0};
        loader->injectPnsResultForTesting(
            currentPns,
            currentGeneration,
            loader->activePnsRequestIdForTesting());
        QCOMPARE(loader->getPnsNorm(), QVector<double>({2.0}));

        PnsCalculator::Result stalePns;
        stalePns.valid = true;
        stalePns.ok = true;
        stalePns.timeSec = {0.099};
        stalePns.pnsNorm = {99.0};
        loader->injectPnsResultForTesting(stalePns, oldGeneration, oldPnsRequest);
        QCOMPARE(loader->getPnsNorm(), QVector<double>({2.0}));

        M1Calculator::Result currentM1;
        currentM1.ok = true;
        currentM1.valid = true;
        currentM1.tSec = {0.002};
        currentM1.m1x = {2.0};
        currentM1.m1y = {2.0};
        currentM1.m1z = {2.0};
        loader->injectM1ResultForTesting(
            currentM1,
            currentGeneration,
            loader->activeM1RequestIdForTesting());
        QCOMPARE(loader->getM1X(), QVector<double>({2.0}));

        M1Calculator::Result staleM1;
        staleM1.ok = true;
        staleM1.valid = true;
        staleM1.tSec = {0.099};
        staleM1.m1x = {99.0};
        staleM1.m1y = {99.0};
        staleM1.m1z = {99.0};
        loader->injectM1ResultForTesting(staleM1, oldGeneration, oldM1Request);
        QCOMPARE(loader->getM1X(), QVector<double>({2.0}));
    }

private:
    QString m_fileA;
    QString m_fileB;
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("SeqEyesTest"));
    app.setApplicationName(QStringLiteral("PulseqLoaderStateTest"));
    PulseqLoaderStateTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "PulseqLoaderStateTest.moc"
