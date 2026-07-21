#include "PulseqOpenController.h"

#include "PulseqLoadUiAdapter.h"
#include "PulseqLoader.h"

#include <QFileInfo>
#include <QDebug>

PulseqOpenController::PulseqOpenController(PulseqLoader& loader, PulseqLoadUiAdapter& ui)
    : m_loader(loader),
      m_ui(ui)
{
}

bool PulseqOpenController::openPath(QString candidatePath)
{
    candidatePath = candidatePath.trimmed();
    if (candidatePath.isEmpty())
        return false;

    QFileInfo fileInfo(candidatePath);
    const QString normalizedPath = fileInfo.absoluteFilePath();

    if (!fileInfo.exists())
    {
        const QString message = QStringLiteral("File does not exist:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return false;
    }

    if (!fileInfo.isFile() || fileInfo.suffix().compare(QStringLiteral("seq"), Qt::CaseInsensitive) != 0)
    {
        const QString message = QStringLiteral("Please select a .seq file:\n%1").arg(candidatePath);
        qWarning().noquote() << message;
        if (!m_loader.isSilentMode())
            m_ui.showWarning(QStringLiteral("File Error"), message);
        return false;
    }

    m_loader.m_sLastOpenDirectory = fileInfo.absolutePath();
    m_loader.saveLastOpenDirectory();

    const bool loaded = m_loader.LoadPulseqFile(normalizedPath);
    if (!loaded)
        qWarning() << "Failed to load file:" << normalizedPath;
    return loaded;
}

bool PulseqOpenController::reopen()
{
    const QString reopenPath = m_loader.m_sPulseqFilePathCache;
    if (reopenPath.isEmpty())
        return false;
    return openPath(reopenPath);
}
