/** @file ExternalSequence.h - Version selector for different Pulseq loaders */

#ifndef _EXTERNAL_SEQUENCE_SELECTOR_H_
#define _EXTERNAL_SEQUENCE_SELECTOR_H_

#include <memory>

// Forward declarations for different version loaders
class ExternalSequence; // Base interface

/**
 * @brief Factory function to create appropriate loader based on file version
 * @param version_major Major version number
 * @param version_minor Minor version number
 * @return Pointer to appropriate loader, or nullptr if version not supported
 */
std::unique_ptr<ExternalSequence> CreateLoaderForVersion(int version_major, int version_minor);

// Include SeqBlock class definition (shared across versions)
// Use v151 version as it's backward compatible with v14x
#include "v151/ExternalSequence.h"

#endif // _EXTERNAL_SEQUENCE_SELECTOR_H_
