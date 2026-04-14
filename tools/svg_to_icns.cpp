#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QStringList>

namespace
{
struct IconSpec
{
    int baseSize;
    const char* baseName;
    const char* retinaName;
};

const IconSpec kSpecs[] = {
    {16,  "icon_16x16.png",   "icon_16x16@2x.png"},
    {32,  "icon_32x32.png",   "icon_32x32@2x.png"},
    {128, "icon_128x128.png", "icon_128x128@2x.png"},
    {256, "icon_256x256.png", "icon_256x256@2x.png"},
    {512, "icon_512x512.png", "icon_512x512@2x.png"},
};

bool renderSvgToPng(const QString& svgPath, const QString& pngPath, int px)
{
    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid())
        return false;

    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    renderer.render(&painter);
    painter.end();

    return image.save(pngPath, "PNG");
}

bool runProcess(const QString& program, const QStringList& args, QString* stdErrOut)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(-1))
    {
        if (stdErrOut)
            *stdErrOut = QStringLiteral("Process timed out or failed to start: %1").arg(program);
        return false;
    }

    const bool ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
    if (!ok && stdErrOut)
        *stdErrOut = QString::fromLocal8Bit(proc.readAllStandardError());
    return ok;
}

void printUsage(const QString& appName)
{
    QTextStream ts(stdout);
    ts << "Usage:\n"
       << "  " << appName << " <input.svg> <output.icns>\n\n"
       << "Example:\n"
       << "  " << appName << " resources/images/logo.svg resources/images/logo.icns\n";
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

#ifdef Q_OS_MAC
    const QStringList args = app.arguments();
    if (args.size() != 3)
    {
        printUsage(QFileInfo(args.value(0)).fileName());
        return 1;
    }

    const QString inputSvg = QFileInfo(args[1]).absoluteFilePath();
    const QString outputIcns = QFileInfo(args[2]).absoluteFilePath();

    if (!QFileInfo::exists(inputSvg))
    {
        QTextStream(stderr) << "Input SVG does not exist: " << inputSvg << "\n";
        return 2;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid())
    {
        QTextStream(stderr) << "Failed to create temporary directory\n";
        return 3;
    }

    const QString iconsetDir = tmp.path() + "/app.iconset";
    if (!QDir().mkpath(iconsetDir))
    {
        QTextStream(stderr) << "Failed to create iconset directory: " << iconsetDir << "\n";
        return 4;
    }

    for (const IconSpec& spec : kSpecs)
    {
        const QString basePath = iconsetDir + "/" + spec.baseName;
        const QString retinaPath = iconsetDir + "/" + spec.retinaName;

        if (!renderSvgToPng(inputSvg, basePath, spec.baseSize))
        {
            QTextStream(stderr) << "Failed to render: " << basePath << "\n";
            return 5;
        }
        if (!renderSvgToPng(inputSvg, retinaPath, spec.baseSize * 2))
        {
            QTextStream(stderr) << "Failed to render: " << retinaPath << "\n";
            return 6;
        }
    }

    QDir().mkpath(QFileInfo(outputIcns).absolutePath());

    QString iconutilErr;
    if (!runProcess("iconutil", {"-c", "icns", iconsetDir, "-o", outputIcns}, &iconutilErr))
    {
        QTextStream(stderr) << "iconutil failed\n";
        if (!iconutilErr.isEmpty())
            QTextStream(stderr) << iconutilErr << "\n";
        return 7;
    }

    QTextStream(stdout) << "Created: " << outputIcns << "\n";
    return 0;
#else
    Q_UNUSED(app)
    QTextStream(stderr)
        << "This tool requires macOS because it uses iconutil to create .icns files.\n";
    return 10;
#endif
}
