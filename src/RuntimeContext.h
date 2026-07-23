#ifndef RUNTIMECONTEXT_H
#define RUNTIMECONTEXT_H

#include "Settings.h"

class PulseqLoader;

class RuntimeContext {
public:
    // Returns the effective SystemProfile given the current sequence context
    static Settings::SystemProfile systemProfile(const PulseqLoader* loader);

    // Returns true if the effective profile is overriding the global default
    static bool isProfileOverridden(const PulseqLoader* loader);
};

#endif // RUNTIMECONTEXT_H
