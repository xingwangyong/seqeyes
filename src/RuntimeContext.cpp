#include "RuntimeContext.h"
#include "PulseqLoader.h"
#include <QList>

Settings::SystemProfile RuntimeContext::systemProfile(const PulseqLoader* loader)
{
    Settings::SystemProfile globalProfile = Settings::getInstance().globalSystemProfile();

    if (!loader) {
        return globalProfile;
    }

    QString requiredSystem = loader->getSequenceSystemName().trimmed();
    if (!requiredSystem.isEmpty()) {
        QList<Settings::SystemProfile> profiles = Settings::getInstance().getSystemProfiles();
        for (const auto& profile : profiles) {
            if (profile.alias.trimmed().compare(requiredSystem, Qt::CaseInsensitive) == 0) {
                return profile;
            }
        }
    }

    return globalProfile;
}

bool RuntimeContext::isProfileOverridden(const PulseqLoader* loader)
{
    if (!loader) {
        return false;
    }

    QString requiredSystem = loader->getSequenceSystemName().trimmed();
    if (requiredSystem.isEmpty()) {
        return false;
    }

    QList<Settings::SystemProfile> profiles = Settings::getInstance().getSystemProfiles();
    bool existsInSettings = false;
    for (const auto& profile : profiles) {
        if (profile.alias.trimmed().compare(requiredSystem, Qt::CaseInsensitive) == 0) {
            existsInSettings = true;
            break;
        }
    }

    if (!existsInSettings) {
        return false; // If the required system doesn't exist, we fallback to global, so no override is active
    }

    QString globalAlias = Settings::getInstance().globalSystemProfileAlias().trimmed();
    return (requiredSystem.compare(globalAlias, Qt::CaseInsensitive) != 0);
}
