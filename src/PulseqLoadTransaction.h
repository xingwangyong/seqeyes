#ifndef PULSEQLOADTRANSACTION_H
#define PULSEQLOADTRANSACTION_H

#include "LoadResult.h"

#include <QPair>
#include <QString>

class PulseqLoader;

// Transaction wrapper for Pulseq sequence loading.
//
// This currently wraps PulseqLoader's existing prepare/commit/rollback phases.
// Full local staging will be introduced after decoded block ownership is
// separated from the live loader members.
class PulseqLoadTransaction
{
public:
    explicit PulseqLoadTransaction(PulseqLoader& loader);

    // Run the full load pipeline: prepare -> commit, or rollback on failure.
    bool load(const QString& path);

private:
    // Run the existing prepare phases. These still write live loader members
    // until LoadedSequenceState staging is completed.
    bool prepare(const QString& path);

    // Finalize the successfully prepared state.
    bool commit(const QString& path);

    // Ensure the loader is in a clean blank state after a failed prepare.
    bool rollback();

    PulseqLoader& m_loader;
    LoadError m_error;
    QPair<double, double> m_initialRange;

    // Reserved for the next step: fully staged sequence state.
    LoadedSequenceState m_staged;
};

#endif
