#include "PulseqOpenController.h"

#include "IPulseqLoadUi.h"
#include "PulseqLoader.h"

#include <QFileInfo>
#include <QDebug>

PulseqOpenController::PulseqOpenController(PulseqLoader& loader, IPulseqLoadUi& ui)
    : m_loader(loader),
      m_ui(ui)
{
}

OpenResult PulseqOpenController::openPath(QString candidatePath)
{
    candidatePath = candidatePath.trimmed();
    if (candidatePath.isEmpty())
        return {};

    QFileInfo fileInfo(candidatePath);
    const QString normalizedPath = fileInfo.absoluteFilePath();

    if (!fileInfo.exists())
    {
        const QString message = QStringLiteral("File does not exist:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return {false, QString(), QStringLiteral("File Error"), message};
    }

    if (!fileInfo.isFile() || fileInfo.suffix().compare(QStringLiteral("seq"), Qt::CaseInsensitive) != 0)
    {
        const QString message = QStringLiteral("Please select a .seq file:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return {false, QString(), QStringLiteral("File Error"), message};
    }

    m_loader.m_sLastOpenDirectory = fileInfo.absolutePath();
    m_loader.saveLastOpenDirectory();

    const bool loaded = m_loader.LoadPulseqFile(normalizedPath);
    if (!loaded)
    {
        qWarning() << "Failed to load file:" << normalizedPath;
        return {false, QString(), QStringLiteral("Load Error"), QStringLiteral("Failed to load file: %1").arg(normalizedPath)};
    }
    return {true, normalizedPath, QString(), QString()};
}

OpenResult PulseqOpenController::reopen()
{
    const QString reopenPath = m_loader.m_sPulseqFilePathCache;
    if (reopenPath.isEmpty())
        return {};
    return openPath(reopenPath);
}
