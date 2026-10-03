#include "RuntimeContext.h"
#include "PulseqLoader.h"
#include <QList>

Settings::SystemProfile RuntimeContext::systemProfile(const PulseqLoader* loader)
{
    // The selected profile in Settings > Safety is always in effect; a sequence's
    // SystemName can only switch that selection when it is loaded.
    Q_UNUSED(loader);
    return Settings::getInstance().globalSystemProfile();
}
