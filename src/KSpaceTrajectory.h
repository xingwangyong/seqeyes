#ifndef KSPACE_TRAJECTORY_H
#define KSPACE_TRAJECTORY_H

#include <QVector>
#include <QString>
#include <vector>

class SeqBlock;

namespace KSpaceTrajectory
{

struct Input
{
    // blocks/blockEdges are references: Input does not own this data, it only
    // points at a vector living elsewhere. Caller must guarantee that data stays
    // alive AND unmodified for the whole duration of compute(). On the same
    // thread (synchronous compute) binding them to loader members is fine.
    // When handing Input to another thread (async compute), DO NOT bind them to
    // data the main thread can touch (e.g. PulseqLoader::m_vecDecodeSeqBlocks /
    // vecBlockEdges): a concurrent reopen clears/reallocates those and the worker
    // would read freed memory (use-after-free). Bind to thread-owned copies
    // instead. See PulseqLoader::startTrajectoryComputationAsync().
    const std::vector<SeqBlock*>& blocks;
    const QVector<double>& blockEdges;
    double tFactor = 1.0;
    bool supportsRfUseMetadata = false;
    double rfRasterUs = 1.0;       // microseconds
    double gradientRasterUs = -1.0; // microseconds
    QVector<double> adcEventTimesInternal;
    // Optional system parameters for RF-use guessing (v1.4.x fallback)
    double b0Tesla = 0.0;          // If 0, ppm fallback from freqOffset is disabled
};

struct Result
{
    QVector<double> kx;
    QVector<double> ky;
    QVector<double> kz;
    QVector<double> t;       // seconds
    QVector<double> t_adc;   // seconds
    QVector<double> kx_adc;
    QVector<double> ky_adc;
    QVector<double> kz_adc;
    QVector<double> excitationTimesInternal;
    QVector<double> refocusingTimesInternal;
    QVector<char>   rfUsePerBlock;   // per block rf.use ('e','r','s','i','p','u' or 0)
    bool rfUseGuessed = false;
    QString warning;
};

Result compute(const Input& input);

} // namespace KSpaceTrajectory

#endif // KSPACE_TRAJECTORY_H
