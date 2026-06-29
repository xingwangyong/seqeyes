#ifndef M1CALCULATOR_H
#define M1CALCULATOR_H

#include <QVector>
#include <QString>
#include <QStringList>
#include <vector>

class SeqBlock;

// M1Calculator
// =============================================================================
// Continuous M1 (first gradient moment) bookkeeping for pulse sequences.
//
// M1(t) = sum over sign-flipped segments of:  sgn * int (tau - t_reset) G(tau) dtau
//
// where:
//   - t_reset is the time of the most recent reset event (excitation or saturation)
//   - sgn flips between +1 and -1 at each refocusing event (spin-echo physics)
//   - Inversion ('i'), preparation ('p'), and unknown ('u') RF events follow
//     simplified rules documented in M1Calculator.cpp.
//
// Scope and limitations (IMPORTANT):
// - This is a simplified gradient-moment model. It tracks only the dominant
//   transverse signal pathway implied by reset/flip events.
// - It does NOT model coherent steady-state sequences (bSSFP, unspoiled SSFP,
//   FISP, etc.) where multiple signal pathways interfere. Use with caution on
//   such sequences; warnings are emitted when suspicious patterns appear.
// - M1 is reported in the same units as the underlying gradient waveform.
//   Pulseq gradients are in Hz/m, so M0 ends up in [1/m] and M1 in [s/m]
//   (NOT [1/m*s] despite the legacy MATLAB reference comment).
// =============================================================================

namespace M1Calculator
{

struct Input
{
    // Snapshot containers by value so async M1 workers don't retain references
    // into PulseqLoader after a new sequence load clears/replaces them.
    std::vector<SeqBlock*> blocks;
    QVector<double> blockEdges;
    double tFactor = 1.0;             // microseconds-to-axis-units scale
    bool supportsRfUseMetadata = false;
    double rfRasterUs = 1.0;          // microseconds
    double gradientRasterUs = -1.0;   // microseconds; if <=0 a default is used
    QVector<double> adcEventTimesInternal;
    double b0Tesla = 0.0;
};

struct Result
{
    bool ok = false;
    bool valid = false;
    QString error;

    // Output sample times (seconds) and per-axis cumulative M1 curves.
    // Lengths of tSec, m1x, m1y, m1z are equal.
    QVector<double> tSec;
    QVector<double> m1x;
    QVector<double> m1y;
    QVector<double> m1z;

    // Diagnostics / advisory messages (non-fatal).
    QStringList warnings;

    // Echo times of excitation ('e'/'E') and refocusing ('r'/'R') RF events,
    // exposed for downstream consumers (UI overlays, debugging, etc.).
    QVector<double> excitationTimesSec;
    QVector<double> refocusingTimesSec;
};

Result compute(const Input& input);

} // namespace M1Calculator

#endif // M1CALCULATOR_H
