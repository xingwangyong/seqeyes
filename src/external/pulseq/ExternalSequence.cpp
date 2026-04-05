/** @file ExternalSequence.cpp - Version selector implementation */

#include "ExternalSequence.h"
#include <iostream>

// Version-specific includes
#include "v14x/ExternalSequence.h"
#include "v151/ExternalSequence.h"

// Factory function implementation
std::unique_ptr<ExternalSequence> CreateLoaderForVersion(int version_major, int version_minor)
{
    // Use v1.5.1 loader for all supported versions (v1.4.x and v1.5.1)
    // v1.5.1 loader is backward compatible with v1.4.x and earlier
    if (version_major == 1 && version_minor <= 5)
    {
        // Use v1.5.1 loader for all versions from v1.4.x to v1.5.1
        return std::make_unique<ExternalSequence>();
    }

    // Future versions not supported yet
    std::cerr << "ERROR: Unsupported Pulseq file version " << version_major << "." << version_minor << std::endl;
    std::cerr << "This loader supports: v1.4.x and v1.5.1 (using v1.5.1 loader)" << std::endl;
    return nullptr;
}
