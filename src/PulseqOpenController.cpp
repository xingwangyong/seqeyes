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
        return OpenResult::noOp();

    QFileInfo fileInfo(candidatePath);
    const QString normalizedPath = fileInfo.absoluteFilePath();

    if (!fileInfo.exists())
    {
        const QString message = QStringLiteral("File does not exist:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return OpenResult::warning(QStringLiteral("File Error"), message);
    }

    if (!fileInfo.isFile() || fileInfo.suffix().compare(QStringLiteral("seq"), Qt::CaseInsensitive) != 0)
    {
        const QString message = QStringLiteral("Please select a .seq file:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return OpenResult::warning(QStringLiteral("File Error"), message);
    }

    m_loader.m_sLastOpenDirectory = fileInfo.absolutePath();
    m_loader.saveLastOpenDirectory();

    OpenResult result = m_loader.LoadPulseqFileResult(normalizedPath);
    if (!result.ok)
    {
        qWarning() << "Failed to load file:" << normalizedPath;
        return result;
    }
    m_loadedPath = result.loadedPath;
    m_reopenPath = result.loadedPath;
    return result;
}

OpenResult PulseqOpenController::reopen()
{
    const QString reopenPath = m_reopenPath;
    if (reopenPath.isEmpty())
        return OpenResult::noOp();
    return openPath(reopenPath);
}
