#include "version_info.h"

#include "seqeyes_version.h"

QString seqeyesAppVersionPlain()
{
    return QString::fromLatin1(SEQEYES_APP_VERSION_PLAIN);
}

QString seqeyesAppVersion()
{
    return QString::fromLatin1(SEQEYES_APP_VERSION);
}

QString seqeyesVersionSummary()
{
    return QStringLiteral("SeqEyes version %1, %2, %3")
        .arg(seqeyesAppVersion())
        .arg(seqeyesGitDate())
        .arg(seqeyesGitHash());
}

QString seqeyesCliVersionText()
{
    return QStringLiteral("%1\nBuilt with Qt6, QCustomPlot, CMake\nModified from PulseqViewer\n")
        .arg(seqeyesVersionSummary());
}
