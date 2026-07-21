#include <QtTest/QtTest>

#include "IPulseqLoadUi.h"
#include "mainwindow.h"
#include "PulseqLoader.h"
#include "PulseqOpenController.h"

#include <QTemporaryDir>
#include <QFile>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

class FakePulseqLoadUi : public IPulseqLoadUi
{
public:
    struct Message
    {
        QString title;
        QString text;
    };

    QWidget* dialogParent() const override { return nullptr; }
    void setBusy(bool busy) override { busyStates.append(busy); }
    void clearLoadedState() override
    {
        ++clearLoadedStateCount;
        hideProgress();
    }
    void clearWindowFilePath() override { ++clearWindowFilePathCount; }
    void hideVersionLabel() override { ++hideVersionLabelCount; }
    void showProgress() override { ++showProgressCount; }
    void setProgressValue(int value) override { progressValues.append(value); }
    void hideProgress() override { ++hideProgressCount; }
    void showWarning(const QString& title, const QString& message) override { warnings.append({title, message}); }
    void showCritical(const QString& title, const QString& message) override { criticals.append({title, message}); }
    void setLoadedTitle(const QString& path) override { loadedTitles.append(path); }
    void refreshTrajectoryPlot() override { ++refreshTrajectoryPlotCount; }

    QVector<bool> busyStates;
    QVector<int> progressValues;
    QVector<Message> warnings;
    QVector<Message> criticals;
    QStringList loadedTitles;
    int clearLoadedStateCount {0};
    int clearWindowFilePathCount {0};
    int hideVersionLabelCount {0};
    int showProgressCount {0};
    int hideProgressCount {0};
    int refreshTrajectoryPlotCount {0};
};

class PulseqOpenControllerFakeUiTest : public QObject
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

private slots:
    void emptyPath_isNoOpWithoutUi()
    {
        PulseqLoader loader(nullptr);
        FakePulseqLoadUi ui;
        PulseqOpenController controller(loader, ui);

        const OpenResult result = controller.openPath(QStringLiteral("  "));

        QVERIFY(!result.ok);
        QVERIFY(result.errorTitle.isEmpty());
        QVERIFY(ui.warnings.isEmpty());
        QVERIFY(ui.criticals.isEmpty());
        QVERIFY(ui.busyStates.isEmpty());
    }

    void missingPath_returnsStructuredWarning()
    {
        PulseqLoader loader(nullptr);
        FakePulseqLoadUi ui;
        PulseqOpenController controller(loader, ui);

        const QString missingPath = QDir::temp().absoluteFilePath(QStringLiteral("seqeyes_missing_file.seq"));
        QFile::remove(missingPath);

        const OpenResult result = controller.openPath(missingPath);

        QVERIFY(!result.ok);
        QCOMPARE(result.errorTitle, QStringLiteral("File Error"));
        QVERIFY(result.errorMessage.contains(QStringLiteral("File does not exist")));
        QCOMPARE(ui.warnings.size(), 1);
        QCOMPARE(ui.warnings[0].title, QStringLiteral("File Error"));
        QVERIFY(ui.warnings[0].text.contains(missingPath));
        QVERIFY(ui.criticals.isEmpty());
        QVERIFY(ui.busyStates.isEmpty());
    }

    void nonSeqFile_returnsStructuredWarning()
    {
        PulseqLoader loader(nullptr);
        FakePulseqLoadUi ui;
        PulseqOpenController controller(loader, ui);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString textPath = dir.filePath(QStringLiteral("not_a_sequence.txt"));
        QFile file(textPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("not a sequence\n");
        file.close();

        const OpenResult result = controller.openPath(textPath);

        QVERIFY(!result.ok);
        QCOMPARE(result.errorTitle, QStringLiteral("File Error"));
        QVERIFY(result.errorMessage.contains(QStringLiteral("Please select a .seq file")));
        QCOMPARE(ui.warnings.size(), 1);
        QCOMPARE(ui.warnings[0].title, QStringLiteral("File Error"));
        QVERIFY(ui.criticals.isEmpty());
        QVERIFY(ui.busyStates.isEmpty());
    }

    void successPath_drivesBusyProgressAndTitleInOrder()
    {
        MainWindow window;
        PulseqLoader* loader = window.getPulseqLoader();
        loader->setAutoStartTrajectoryAfterLoad(false);
        loader->setAutoStartPnsAfterLoad(false);

        auto fakeUi = std::make_unique<FakePulseqLoadUi>();
        FakePulseqLoadUi* ui = fakeUi.get();
        loader->setLoadUiForTesting(std::move(fakeUi));
        PulseqOpenController controller(*loader, *ui);

        const QString path = resolveSeq(QStringLiteral("writeFid.seq"));
        QVERIFY2(QFile::exists(path), qPrintable(path));

        const OpenResult result = controller.openPath(path);

        QVERIFY(result.ok);
        QCOMPARE(result.loadedPath, path);
        QCOMPARE(ui->busyStates, QVector<bool>({true, false}));
        QCOMPARE(ui->showProgressCount, 1);
        QVERIFY(!ui->progressValues.isEmpty());
        QCOMPARE(ui->progressValues.first(), 0);
        QCOMPARE(ui->progressValues.last(), 100);
        QVERIFY(ui->hideProgressCount >= 1);
        QCOMPARE(ui->loadedTitles.size(), 1);
        QCOMPARE(ui->loadedTitles[0], path);
        QVERIFY(ui->warnings.isEmpty());
        QVERIFY(ui->criticals.isEmpty());
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Loaded);
    }

    void failurePath_restoresBusyHidesProgressAndShowsCritical()
    {
        MainWindow window;
        PulseqLoader* loader = window.getPulseqLoader();
        loader->setAutoStartTrajectoryAfterLoad(false);
        loader->setAutoStartPnsAfterLoad(false);

        auto fakeUi = std::make_unique<FakePulseqLoadUi>();
        FakePulseqLoadUi* ui = fakeUi.get();
        loader->setLoadUiForTesting(std::move(fakeUi));
        PulseqOpenController controller(*loader, *ui);

        const QString path = resolveSeq(QStringLiteral("no_version.seq"));
        QVERIFY2(QFile::exists(path), qPrintable(path));

        const OpenResult result = controller.openPath(path);

        QVERIFY(!result.ok);
        QCOMPARE(result.errorTitle, QStringLiteral("Load Error"));
        QVERIFY(result.errorMessage.contains(QStringLiteral("Failed to read version information")));
        QCOMPARE(ui->busyStates, QVector<bool>({true, false}));
        QVERIFY(ui->hideProgressCount >= 1);
        QVERIFY(ui->loadedTitles.isEmpty());
        QCOMPARE(ui->warnings.size(), 0);
        QCOMPARE(ui->criticals.size(), 1);
        QCOMPARE(ui->criticals[0].title, QStringLiteral("Load Error"));
        QVERIFY(ui->criticals[0].text.contains(QStringLiteral("Failed to read version information")));
        QCOMPARE(loader->getSequenceLoadState(), PulseqLoader::SequenceLoadState::Blank);
    }

    void criticalError_usesUiAdapter()
    {
        MainWindow window;
        PulseqLoader* loader = window.getPulseqLoader();
        loader->setAutoStartTrajectoryAfterLoad(false);
        loader->setAutoStartPnsAfterLoad(false);

        auto fakeUi = std::make_unique<FakePulseqLoadUi>();
        FakePulseqLoadUi* ui = fakeUi.get();
        loader->setLoadUiForTesting(std::move(fakeUi));
        PulseqOpenController controller(*loader, *ui);

        const QString path = resolveSeq(QStringLiteral("unsupported_version.seq"));
        QVERIFY2(QFile::exists(path), qPrintable(path));

        const OpenResult result = controller.openPath(path);

        QVERIFY(!result.ok);
        QCOMPARE(result.errorTitle, QStringLiteral("Load Error"));
        QVERIFY(result.errorMessage.contains(QStringLiteral("Unsupported Pulseq file version")));
        QCOMPARE(ui->criticals.size(), 1);
        QCOMPARE(ui->criticals[0].title, QStringLiteral("Load Error"));
        QVERIFY(ui->criticals[0].text.contains(QStringLiteral("Unsupported Pulseq file version")));
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    PulseqOpenControllerFakeUiTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "PulseqOpenControllerFakeUiTest.moc"
