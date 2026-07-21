#ifndef PULSEQLOADTRANSACTION_H
#define PULSEQLOADTRANSACTION_H

#include "LoadResult.h"

#include <QPair>
#include <QString>

class PulseqLoader;

// Transaction wrapper for Pulseq sequence loading.
//
// prepare() builds version/parser/decode results in local staged state. commit()
// is the only point that publishes that staged sequence into PulseqLoader live
// members before the remaining cache/UI work runs.
class PulseqLoadTransaction
{
public:
    explicit PulseqLoadTransaction(PulseqLoader& loader);

    // Run the full load pipeline: prepare -> commit, or rollback on failure.
    LoadResult load(const QString& path);

private:
    // Build the staged sequence state without publishing it to live members.
    bool prepare(const QString& path);

    // Finalize the successfully prepared state.
    bool commit(const QString& path);

    // Ensure the loader is in a clean blank state after a failed prepare.
    LoadResult rollback();

    PulseqLoader& m_loader;
    LoadError m_error;
    QPair<double, double> m_initialRange;

    LoadedSequenceState m_staged;
};

#endif
