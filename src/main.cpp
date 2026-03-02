#include "mainwindow.h"
#include "PulseqLoader.h"

#include <QApplication>
#include <QStyleFactory>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>
#include "WaveformDrawer.h"
#include "InteractionHandler.h"
#include "TRManager.h"
#include "PulseqLoader.h"
#include "AutomationRunner.h"
#include <QDebug>
//#pragma comment(linker, "/subsystem:windows /entry:mainCRTStartup")

// Optional: filter Qt logs according to Settings log level
#include "Settings.h"
#include "ExternalSequence.h"
#include "LogManager.h"
#include <QMessageBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QTextStream>
#ifdef _WIN32
#  include <Windows.h>
#endif

// Use a semantic minimum level rather than QtMsgType ordering (QtMsgType values are non-monotonic)
static Settings::LogLevel g_minLogLevel = Settings::LogLevel::Critical;
static void qtLogFilter(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    auto shouldEmit = [](QtMsgType t)->bool {
        switch (g_minLogLevel) {
            case Settings::LogLevel::Debug:
                return true; // all messages
            case Settings::LogLevel::Info:
                return t == QtInfoMsg || t == QtWarningMsg || t == QtCriticalMsg || t == QtFatalMsg || t == QtDebugMsg; // include debug too for completeness with Qt's macros
            case Settings::LogLevel::Warning:
                return t == QtWarningMsg || t == QtCriticalMsg || t == QtFatalMsg;
            case Settings::LogLevel::Critical:
                return t == QtCriticalMsg || t == QtFatalMsg;
            case Settings::LogLevel::Fatal:
                return t == QtFatalMsg;
            default:
                return t == QtCriticalMsg || t == QtFatalMsg;
        }
    };
    if (!shouldEmit(type))
        return;

    // Route all messages through LogManager so they appear in the GUI log window.
    LogManager::getInstance().appendFromQt(type, ctx, msg);

    // Keep writing to stderr for headless / test binaries.
    fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    fflush(stderr);
}

static void updateQtLogThresholdFromSettings()
{
    using LL = Settings::LogLevel;
    LL lvl = Settings::getInstance().getLogLevel();
    g_minLogLevel = lvl;
}

// Route ExternalSequence internal prints into Qt/LogManager so they appear in the Log window.
// The legacy library only provides a plain string, so we do a lightweight severity guess.
static void externalSeqLogPrinter(const std::string &str)
{
    QString msg = QString::fromStdString(str);
    QString trimmed = msg.trimmed();

    // Map obvious prefixes to warning/error; everything else as informational.
    const bool isError   = trimmed.startsWith(QStringLiteral("*** ERROR"),   Qt::CaseInsensitive);
    const bool isWarning = trimmed.startsWith(QStringLiteral("*** WARNING"), Qt::CaseInsensitive);

    if (isError)
    {
        qWarning().noquote() << "[ExternalSequence]" << trimmed;
    }
    else if (isWarning)
    {
        qWarning().noquote() << "[ExternalSequence]" << trimmed;
    }
    else
    {
        qInfo().noquote() << "[ExternalSequence]" << trimmed;
    }
}

// Centralized CLI registration and helpers
static void registerOptions(QCommandLineParser& parser, bool includeBuiltInHelpVersion)
{
    // Built-in standard options
    // QCommandLineParser may auto-handle built-in help/version and exit (or show a dialog on Windows GUI apps).
    // We disable built-ins for the early CLI-only path and handle printing ourselves.
    if (includeBuiltInHelpVersion)
    {
        parser.addHelpOption();
        parser.addVersionOption();
    }
    else
    {
        // Add non-builtin equivalents so they appear in help text without triggering auto-exit behavior.
        parser.addOption(QCommandLineOption(QStringList() << "h" << "help", "Show this help text and exit"));
        parser.addOption(QCommandLineOption(QStringList() << "v" << "version", "Show version information and exit"));
    }

    // Window/control
    parser.addOption(QCommandLineOption("maximized", "Start maximized"));
    parser.addOption(QCommandLineOption(QStringList() << "name", "Set window title to the specified name", "title"));

    // Visibility toggles
    parser.addOption(QCommandLineOption("no-ADC", "Hide ADC/labels axis"));
    parser.addOption(QCommandLineOption("no-RFmag", "Hide RF magnitude axis"));
    parser.addOption(QCommandLineOption("no-RFphase", "Hide RF/ADC phase axis"));
    parser.addOption(QCommandLineOption("no-Gx", "Hide Gx axis"));
    parser.addOption(QCommandLineOption("no-Gy", "Hide Gy axis"));
    parser.addOption(QCommandLineOption("no-Gz", "Hide Gz axis"));

    // Render mode
    parser.addOption(QCommandLineOption("TR-segmented", "TR-Segmented Mode"));
    parser.addOption(QCommandLineOption("Whole-sequence", "Whole-Sequence Mode"));

    // Ranges
    parser.addOption(QCommandLineOption(QStringList() << "TR-range", "TR range as start~end (1-based)", "start~end"));
    parser.addOption(QCommandLineOption(QStringList() << "time-range", "Time range as start~end in ms", "start~end"));

    // Layout
    parser.addOption(QCommandLineOption("layout", "Subplot layout as abc (e.g., 211, Matlab subplot style)", "abc"));

    // Headless/test
    parser.addOption(QCommandLineOption("headless", "Do not show GUI (for testing/CLI)"));
    parser.addOption(QCommandLineOption("exit-after-load", "Exit after loading file (no event loop). Implies --headless."));
    parser.addOption(QCommandLineOption("automation", "Run automation scenario JSON (implies --headless)", "scenario.json"));
    parser.addOption(QCommandLineOption(QStringList() << "capture-snapshots", "Capture sequence and trajectory snapshots to the specified directory and exit (implies --headless)", "out_dir"));

    // Positional argument for file
    parser.addPositionalArgument("file", "Pulseq sequence file (.seq) to open", "[file]");
}

static bool isHeadless(const QCommandLineParser& parser)
{
    return parser.isSet("headless") || parser.isSet("exit-after-load") || parser.isSet("automation") || parser.isSet("capture-snapshots");
}

// Git version info generated by CMake (commit date YYYYMMDD and commit hash)
#include <version_autogen.h>
// Manual app semantic version
#include "seqeyes_version.h"
void showVersion()
{
    qDebug() << "SeqEyes version" << SEQEYES_APP_VERSION << "," << SEQEYE_GIT_DATE << "," << SEQEYE_GIT_HASH;
    qDebug() << "Built with Qt6, QCustomPlot, CMake";
    qDebug() << "Modified from PulseqViewer";
}

static QString versionText()
{
    return QStringLiteral("SeqEyes version %1, %2, %3\nBuilt with Qt6, QCustomPlot, CMake\nModified from PulseqViewer\n")
        .arg(QString::fromLatin1(SEQEYES_APP_VERSION))
        .arg(QString::fromLatin1(SEQEYE_GIT_DATE))
        .arg(QString::fromLatin1(SEQEYE_GIT_HASH));
}

static bool wantsCliHelpOrVersion(int argc, char* argv[], bool& outHelp, bool& outVersion)
{
    outHelp = false;
    outVersion = false;
    for (int i = 1; i < argc; ++i)
    {
        const QString a = QString::fromLocal8Bit(argv[i]).trimmed();
        if (a == "-h" || a == "--help" || a == "--help-all")
            outHelp = true;
        if (a == "-v" || a == "--version")
            outVersion = true;
    }
    return outHelp || outVersion;
}

static void cliWriteRaw(const QByteArray& bytes, bool toStderr)
{
#ifdef _WIN32
    // GUI-subsystem apps may not have a console attached; try to attach to parent.
    HANDLE h = GetStdHandle(toStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE)
    {
        // Attach to the parent console if possible; if it fails (e.g. started from Explorer), we still fall back to stderr via Qt below.
        AttachConsole(ATTACH_PARENT_PROCESS);
        h = GetStdHandle(toStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    }
    if (h && h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        return;
    }
#endif
    FILE* f = toStderr ? stderr : stdout;
    if (f)
    {
        fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), f);
        fflush(f);
    }
}

static void cliWriteText(const QString& text, bool toStderr = false)
{
    cliWriteRaw(text.toUtf8(), toStderr);
}

int main(int argc, char *argv[])
{
    // CLI help/version should never create GUI windows.
    // On Windows, GUI-subsystem apps may show a message box for --help/--version; print to stdout and exit instead.
    {
        bool wantHelp = false;
        bool wantVersion = false;
        if (wantsCliHelpOrVersion(argc, argv, wantHelp, wantVersion))
        {
            QCoreApplication coreApp(argc, argv);
            coreApp.setApplicationName("SeqEyes");
            coreApp.setOrganizationName("SeqEyes");
            coreApp.setApplicationVersion(SEQEYES_APP_VERSION_PLAIN);

            QCommandLineParser parser;
            parser.setApplicationDescription("SeqEyes - Pulseq Sequence Viewer");
            registerOptions(parser, /*includeBuiltInHelpVersion*/ false);
            parser.process(coreApp);

            if (wantHelp)
            {
                cliWriteText(parser.helpText(), false);
                return 0;
            }
            if (wantVersion)
            {
                cliWriteText(versionText(), false);
                return 0;
            }
        }
    }

    // Install global Qt log filter early, default to Error-only
    qInstallMessageHandler(qtLogFilter);
    g_minLogLevel = Settings::LogLevel::Critical;

    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QApplication app(argc, argv);
    app.setApplicationName("SeqEyes");
    // Avoid platform/Qt auto-appending the application display name to window titles
    // (e.g. "Title - SeqEyes"), because we fully control the main window title ourselves.
    app.setOrganizationName("SeqEyes");
    app.setApplicationVersion(SEQEYES_APP_VERSION_PLAIN);
    // Force LTR across the whole app to avoid inverted scrollbars/RTL behavior on some platforms/styles.
    app.setLayoutDirection(Qt::LeftToRight);
    
    // Set up command line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("SeqEyes - Pulseq Sequence Viewer");
    registerOptions(parser, /*includeBuiltInHelpVersion*/ true);
    
    // Parse command line arguments
    parser.process(app);

    // Initialize Settings (reads JSON) and update log filter threshold
    // Ensure this happens after parser.process but before heavy work
    (void)Settings::getInstance(); // construct/load once
    updateQtLogThresholdFromSettings();

    // Route verbose prints from ExternalSequence into Qt logging (and therefore the Log window)
    ExternalSequence::SetPrintFunction(&externalSeqLogPrinter);
    
    // Get positional arguments
    const QStringList args = parser.positionalArguments();
    QString fileToOpen;
    if (!args.isEmpty()) {
        fileToOpen = args.first();
    }
    
    // Create main window
    MainWindow window;

    // Apply CLI options to window state (visibility/mode), may be partially deferred until file load
    window.applyCommandLineOptions(parser);

    // Headless/test paths
    bool headless = isHeadless(parser);

    // Note: No test-specific commands beyond minimal generic flags.

    if (!headless) {
        // Show window
        if (parser.isSet("maximized")) {
            window.showMaximized();
        } else {
#ifdef RELEASE
            window.showMaximized();
#else
            window.show();
#endif
        }
        // Known issues dialog on startup (per-user)
        if (Settings::getInstance().getShowKnownIssuesDialog()) {
            qWarning().noquote() << "[Known issues] RF/ADC phases are not accurate.";
            qWarning().noquote() << "[Known issues] UI might be laggy for large sequence; try reducing slices/repetitions/diffusion directions.";
            QMessageBox msg;
            msg.setIcon(QMessageBox::Warning);
            msg.setWindowTitle("Known issues");
            msg.setText("1) RF/ADC phases are not accurate.\n"
                        "2) On linux, sometimes the ADC channel rendered strangely, \n"
                        "e.g. adjcent ADCs are connected, you may need to zoom in to see the ADC correctly rendered.\n"
                        "3) UI might be laggy for large sequence.\n"
                        "   Try to make the sequence smaller (reduce #slices, #repetitions, #diffusion directions, etc.).");
            QCheckBox* cb = new QCheckBox("Do not show again");
            msg.setCheckBox(cb);
            msg.addButton(QMessageBox::Ok);
            msg.exec();
            if (cb->isChecked()) {
                Settings::getInstance().setShowKnownIssuesDialog(false);
            }
        }
    }

    // Open file if specified
    if (!fileToOpen.isEmpty()) {
        // Silent mode if headless/exit-after-load
        if (headless) window.getPulseqLoader()->setSilentMode(true);
        window.openFileFromCommandLine(fileToOpen);
        // Re-apply options that depend on loaded data (ranges)
        window.applyCommandLineOptions(parser);
        
        if (parser.isSet("capture-snapshots")) {
            QString outDir = parser.value("capture-snapshots");
            window.captureSnapshotsAndExit(outDir);
            // We do NOT return here, we let app.exec() run the singleShot timer inside captureSnapshotsAndExit
        } else if (parser.isSet("exit-after-load")) {
            return 0;
        }
    }

    // Automation scenario (headless)
    if (parser.isSet("automation")) {
        const QString scen = parser.value("automation");
        if (scen.isEmpty()) { qWarning() << "--automation requires a JSON path"; return 2; }
        int rc = AutomationRunner::run(window, scen);
        return rc;
    }

    // If headless without file or automation or capture-snapshots, just exit
    if (headless && !parser.isSet("automation") && !parser.isSet("capture-snapshots")) {
        return 0;
    }

    return app.exec();
}
