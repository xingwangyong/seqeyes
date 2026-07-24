// M1Calculator.cpp
// =============================================================================
// Continuous M1 (first gradient moment) bookkeeping for pulse sequences.
//
// Design notes
// ------------
// 1) We replicate the piecewise-linear gradient integration math from the
//    reference MATLAB helper (continuousMomentFromPolylineWindow +
//    continuousMomentFromPolyline). The math is closed-form and exact for
//    piecewise-linear waveforms; no numerical quadrature is needed.
//    Internally we accumulate both M0 and M1 about the RF/reset center, then
//    report M1 about each output sample time:
//       M1_about_t(t) = M1_about_reset(t) - (t - t_reset) * M0(t).
//
// 2) Bookkeeping model (per the spec):
//      - Reset events (excitation 'e'/'E', saturation 's'/'S'):
//          * Cumulative M1 collapses to 0 at the event time.
//          * Subsequent integration starts a new epoch (t_reset = t_event).
//          * Sign returns to +1.
//      - Sign-flip events (refocusing 'r'/'R'):
//          * Cumulative M1 is preserved (NOT zeroed).
//          * Subsequent integration has its sign flipped (spin-echo physics:
//            phase evolution gets conjugated across the pi pulse).
//      - Ignored events:
//          * Inversion ('i'/'I'): no transverse signal of interest for the
//            main pathway; bookkeeping continues unchanged.
//          * Preparation ('p'/'P'): treated as a reset in v1, with a
//            warning emitted because real prep modules (T2-prep, flow-comp,
//            diffusion-prep, MT-prep) may either spoil or preserve encoding.
//          * Unknown ('u'/'U'): ignored, with a warning.
//
// 3) Per-axis cumulative M1 curves are sampled on a regular raster grid
//    (gradientRasterUs), with explicit output points at every reset / flip
//    event time so the plot shows exact zero crossings and slope changes.
//
// 4) Limitations (echoed in the header):
//    - No model for coherent steady-state sequences (bSSFP / unspoiled SSFP /
//      FISP). Warnings are emitted when multiple closely-spaced excitation RF
//      events are detected, which is a strong hint of a steady-state train.
//    - The M1 math assumes piecewise-linear gradient waveforms between
//      support points; arbitrary (non-linear) gradients are first rasterized
//      to piecewise-linear form by SeriesBuilder before integration.
// =============================================================================

#include "M1Calculator.h"
#include "SeriesBuilder.h"
#include "Settings.h"
#include "ExternalSequence.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <QElapsedTimer>
#include "LogManager.h"

namespace M1Calculator
{

namespace
{

// -----------------------------------------------------------------------------
// Closed-form piecewise-linear integration. Direct 1:1 port of the reference
// MATLAB continuousMomentFromPolyline(times, amps). Returns the integrals of
// G(t) (m0Abs) and t*G(t) (m1Abs) about t=0 over the polyline span.
// Caller is responsible for clipping to the desired [tStart, tEnd] window
// and for the moment-shift adjustment (m1 = m1Abs - tRef*m0Abs).
// -----------------------------------------------------------------------------
void continuousMomentFromPolyline(const QVector<double>& times,
                                  const QVector<double>& amps,
                                  double& m0Abs,
                                  double& m1Abs)
{
    m0Abs = 0.0;
    m1Abs = 0.0;
    const int n = times.size();
    if (n != amps.size() || n < 2)
    {
        return;
    }
    for (int i = 0; i < n - 1; ++i)
    {
        const double t0 = times[i];
        const double t1 = times[i + 1];
        const double g0 = amps[i];
        const double g1 = amps[i + 1];
        if (!(t1 > t0))
        {
            // Non-strictly-increasing or zero-length segment -> skip.
            continue;
        }
        const double m = (g1 - g0) / (t1 - t0);
        const double b = g0 - m * t0;
        m0Abs += 0.5 * m * (t1 * t1 - t0 * t0) + b * (t1 - t0);
        m1Abs += (m / 3.0) * (t1 * t1 * t1 - t0 * t0 * t0) + 0.5 * b * (t1 * t1 - t0 * t0);
    }
}

// -----------------------------------------------------------------------------
// Wrapper: integrate G(t) and (t-tRef)*G(t) over the window [tStart, tEnd],
// clipping the polyline endpoints with linear interpolation. Direct port of
// continuousMomentFromPolylineWindow.
// -----------------------------------------------------------------------------
void continuousMomentFromPolylineWindow(const QVector<double>& times,
                                        const QVector<double>& amps,
                                        double tStart,
                                        double tEnd,
                                        double tRef,
                                        double& m0Out,
                                        double& m1Out)
{
    m0Out = 0.0;
    m1Out = 0.0;
    if (!(tEnd > tStart))
    {
        return;
    }
    // Build the clipped polyline: [tStart, ...interior times..., tEnd].
    QVector<double> tClip;
    QVector<double> aClip;
    tClip.reserve(times.size() + 2);
    aClip.reserve(times.size() + 2);
    tClip.append(tStart);
    aClip.append(0.0); // placeholder; will be set by interp1 below
    for (int i = 0; i < times.size(); ++i)
    {
        const double ti = times[i];
        if (ti > tStart && ti < tEnd)
        {
            tClip.append(ti);
            aClip.append(0.0);
        }
    }
    tClip.append(tEnd);
    aClip.append(0.0);

    // Linear interpolation at clip points (extrapolate as 0 outside the
    // original time span, matching MATLAB interp1 'linear', 0).
    auto interpAt = [&](double t) -> double {
        const int n = times.size();
        if (n == 0 || t < times.first() || t > times.last())
        {
            return 0.0;
        }
        // Binary search for the segment containing t.
        int lo = 0;
        int hi = n - 1;
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            if (times[mid] <= t) lo = mid;
            else hi = mid;
        }
        const double t0 = times[lo];
        const double t1 = times[hi];
        if (t1 == t0) return amps[lo];
        const double alpha = (t - t0) / (t1 - t0);
        return amps[lo] + (amps[hi] - amps[lo]) * alpha;
    };
    for (int i = 0; i < tClip.size(); ++i)
    {
        aClip[i] = interpAt(tClip[i]);
    }

    double m0Abs = 0.0;
    double m1Abs = 0.0;
    continuousMomentFromPolyline(tClip, aClip, m0Abs, m1Abs);
    m0Out = m0Abs;
    // Moment-shift: M1 about tRef equals M1 about 0 minus tRef times M0.
    m1Out = m1Abs - tRef * m0Abs;
}

// -----------------------------------------------------------------------------
// Strip non-finite entries and collapse near-duplicate timestamps. Mirrors
// KSpaceTrajectory::sanitizeGradientSeries closely, kept local so M1Calculator
// has no extra header dependency.
// -----------------------------------------------------------------------------
void sanitizeGradientSeries(QVector<double>& times, QVector<double>& values)
{
    if (times.size() != values.size())
    {
        const int n = std::min(times.size(), values.size());
        times.resize(n);
        values.resize(n);
    }
    int writeIdx = 0;
    double prevTime = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < times.size(); ++i)
    {
        const double t = times[i];
        const double val = values[i];
        if (!std::isfinite(t) || !std::isfinite(val))
        {
            continue;
        }
        if (writeIdx > 0 && t < prevTime - 1e-15)
        {
            continue;
        }
        if (writeIdx > 0 && std::abs(t - prevTime) <= 1e-15)
        {
            // Duplicate timestamp: average the values (a true physical
            // duplicate would carry the same value; this is defensive).
            values[writeIdx - 1] = 0.5 * (values[writeIdx - 1] + val);
            continue;
        }
        times[writeIdx] = t;
        values[writeIdx] = val;
        ++writeIdx;
        prevTime = t;
    }
    times.resize(writeIdx);
    values.resize(writeIdx);
}

// -----------------------------------------------------------------------------
// RF event classification (subset of KSpaceTrajectory::classifyRfUse logic
// adapted for our needs). Returns:
//   'e' excitation, 'r' refocusing, 's' saturation,
//   'i' inversion,  'p' preparation, 'u' unknown, '\0' no RF.
// -----------------------------------------------------------------------------
char classifyRfUse(SeqBlock* blk)
{
    if (!blk || !blk->isRF()) return '\0';
    const RFEvent& rf = blk->GetRFEvent();
    const char c = rf.use;
    if (c == 'e' || c == 'E') return 'e';
    if (c == 'r' || c == 'R') return 'r';
    if (c == 's' || c == 'S') return 's';
    if (c == 'i' || c == 'I') return 'i';
    if (c == 'p' || c == 'P') return 'p';
    return 'u';
}

inline double internalToSec(double v, double tFactor)
{
    if (tFactor == 0.0)
    {
        return v;
    }
    return (v / tFactor) * 1e-6;
}

void convertInternalTimesToSec(QVector<double>& times, double tFactor)
{
    for (double& t : times)
    {
        t = internalToSec(t, tFactor);
    }
}

// RF center time (seconds), matching KSpaceTrajectory::rfCenterUs semantics
// but returning seconds directly.
double rfCenterSec(SeqBlock* blk)
{
    if (!blk || !blk->isRF()) return 0.0;
    const RFEvent& rf = blk->GetRFEvent();
    double centerUs = rf.center;
    if (centerUs < 0.0)
    {
        // Legacy fallback: peak-based estimate from the RF amplitude array.
        const int length = blk->GetRFLength();
        const float* ampPtr = blk->GetRFAmplitudePtr();
        float dwell = blk->GetRFDwellTime();
        if (dwell <= 0.0f) dwell = 1.0f;
        if (length > 0 && ampPtr)
        {
            int peakIdx = 0;
            double peakVal = 0.0;
            for (int i = 0; i < length; ++i)
            {
                const double v = std::fabs(static_cast<double>(ampPtr[i]));
                if (v > peakVal) { peakVal = v; peakIdx = i; }
            }
            centerUs = (static_cast<double>(peakIdx) + 0.5) * static_cast<double>(dwell);
        }
        else
        {
            centerUs = 0.0;
        }
    }
    return (rf.delay + centerUs) * 1e-6;
}

struct RfEventRec
{
    double tSec = 0.0;
    char   use  = '\0';
};

// -----------------------------------------------------------------------------
// Scan blocks and emit one RfEventRec per RF block.
// -----------------------------------------------------------------------------
void collectRfEvents(const std::vector<SeqBlock*>& blocks,
                     const QVector<double>& blockEdges,
                     double tFactor,
                     std::vector<RfEventRec>& out,
                     QStringList& warnings)
{
    out.clear();
    out.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        SeqBlock* blk = blocks[i];
        if (!blk || !blk->isRF()) continue;
        const double edgeSec = internalToSec(blockEdges[static_cast<int>(i)], tFactor);
        const double centerSec = rfCenterSec(blk);
        const char use = classifyRfUse(blk);
        if (use == '\0') continue;
        RfEventRec rec;
        rec.tSec = edgeSec + centerSec;
        rec.use  = use;
        out.push_back(rec);
        if (use == 'u')
        {
            warnings << QStringLiteral("Unknown RF use 'u' at t=%1 s; M1 bookkeeping "
                                       "treats it as no-op.").arg(rec.tSec, 0, 'f', 6);
        }
        else if (use == 'p')
        {
            warnings << QStringLiteral("Preparation module 'p' at t=%1 s; treated as "
                                       "M1 reset (simplified handling; prep modules "
                                       "that preserve phase encoding will give wrong "
                                       "results).").arg(rec.tSec, 0, 'f', 6);
        }
    }
}

// Event kinds for the time-ordered walker.
enum class EvKind { Reset, Flip };

struct WalkerEvent
{
    double  tSec = 0.0;
    EvKind  kind = EvKind::Reset;
};

// Build a sorted event list with reset events before flip events at equal times.
void buildEventList(const std::vector<RfEventRec>& rfs,
                    std::vector<WalkerEvent>& out)
{
    out.clear();
    out.reserve(rfs.size());
    for (const auto& rf : rfs)
    {
        EvKind k = EvKind::Flip;
        if (rf.use == 'e' || rf.use == 's') k = EvKind::Reset;
        // 'i' and 'u' are explicitly ignored (no event emitted).
        // 'p' is treated as Reset (with warning already issued by collectRfEvents).
        else if (rf.use == 'p')               k = EvKind::Reset;
        // 'r' stays Flip.
        WalkerEvent ev;
        ev.tSec = rf.tSec;
        ev.kind = k;
        out.push_back(ev);
    }
    std::sort(out.begin(), out.end(), [](const WalkerEvent& a, const WalkerEvent& b) {
        if (a.tSec != b.tSec)
        {
            return a.tSec < b.tSec;
        }
        // On ties: Reset precedes Flip so the M1 plot shows the zero crossing
        // before the slope change.
        return a.kind == EvKind::Reset && b.kind == EvKind::Flip;
    });
}

} // anonymous namespace

// =============================================================================
// Public entry point
// =============================================================================
Result compute(const Input& input)
{
    QElapsedTimer perfTimer;
    perfTimer.start();
    Result result;
    if (input.blocks.empty() || input.blockEdges.size() < 2)
    {
        result.error = QStringLiteral("Empty or invalid block list.");
        result.ok = false;
        return result;
    }

    // -- Step 1: gradient series per axis (seconds, Hz/m) --------------------
    QVector<double> gxTime, gxVal, gyTime, gyVal, gzTime, gzVal;
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges,
                                       input.tFactor, 0,
                                       gxTime, gxVal, input.gradientRasterUs);
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges,
                                       input.tFactor, 1,
                                       gyTime, gyVal, input.gradientRasterUs);
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges,
                                       input.tFactor, 2,
                                       gzTime, gzVal, input.gradientRasterUs);
    sanitizeGradientSeries(gxTime, gxVal);
    sanitizeGradientSeries(gyTime, gyVal);
    sanitizeGradientSeries(gzTime, gzVal);
    convertInternalTimesToSec(gxTime, input.tFactor);
    convertInternalTimesToSec(gyTime, input.tFactor);
    convertInternalTimesToSec(gzTime, input.tFactor);

    auto timeRangeSec = [&](const QVector<double>& t) -> std::pair<double,double> {
        if (t.isEmpty()) return {0.0, 0.0};
        return {t.first(), t.last()};
    };
    const auto [gxMin, gxMax] = timeRangeSec(gxTime);
    const auto [gyMin, gyMax] = timeRangeSec(gyTime);
    const auto [gzMin, gzMax] = timeRangeSec(gzTime);
    const double tMin = std::min({gxMin, gyMin, gzMin});
    const double tMax = std::max({gxMax, gyMax, gzMax});

    // -- Step 2: RF events ----------------------------------------------------
    std::vector<RfEventRec> rfs;
    collectRfEvents(input.blocks, input.blockEdges, input.tFactor, rfs, result.warnings);

    // Track excitation / refocusing lists for downstream consumers.
    for (const auto& rf : rfs)
    {
        if (rf.use == 'e') result.excitationTimesSec.append(rf.tSec);
        else if (rf.use == 'r') result.refocusingTimesSec.append(rf.tSec);
    }

    std::vector<WalkerEvent> events;
    buildEventList(rfs, events);

    // -- Step 3: emit warnings -----------------------------------------------
    // Steady-state suspicion: more than 8 excitations spaced <100 ms apart.
    int recentExcCount = 0;
    double lastExcT = -1.0e9;
    for (const auto& rf : rfs)
    {
        if (rf.use == 'e')
        {
            if (rf.tSec - lastExcT < 0.100) ++recentExcCount;
            lastExcT = rf.tSec;
        }
    }
    if (recentExcCount > 8)
    {
        result.warnings << QStringLiteral(
            "Sequence shows %1 closely-spaced (<100 ms) excitation events. This "
            "pattern is consistent with a steady-state sequence (e.g. bSSFP, "
            "unspoiled SSFP, FISP) for which the simplified reset/flip "
            "bookkeeping does NOT model coherent pathway interference. Treat "
            "the resulting M1 curve as advisory only.").arg(recentExcCount);
    }
    if (result.excitationTimesSec.isEmpty())
    {
        result.warnings << QStringLiteral(
            "No excitation RF events found in sequence. M1 will be integrated "
            "from t=%1 s (sequence start) with no signal basis.").arg(tMin, 0, 'f', 6);
    }

    // -- Step 4: sample time grid --------------------------------------------
    const double rasterSec = (input.gradientRasterUs > 0.0)
                              ? input.gradientRasterUs * 1e-6
                              : 10e-6; // 10 us default, matching KSpaceTrajectory
    if (rasterSec <= 0.0)
    {
        result.error = QStringLiteral("gradientRasterUs must be positive.");
        result.ok = false;
        return result;
    }

    QVector<double> samples;
    const int nSamples = static_cast<int>(std::floor((tMax - tMin) / rasterSec)) + 1;
    samples.reserve(nSamples + 4);
    for (int i = 0; i < nSamples; ++i)
    {
        samples.append(tMin + i * rasterSec);
    }
    // Always emit a final sample at tMax so the curve closes properly.
    if (samples.isEmpty() || samples.last() < tMax - 1e-15)
    {
        samples.append(tMax);
    }

    // -- Step 5: state-machine walker ----------------------------------------
    // One walker instance per axis; emits (t, m1) points where t is shared
    // across axes by construction (same event/samples schedule, same order).
    auto walkAndCollect = [&](const QVector<double>& gTime,
                              const QVector<double>& gVal,
                              QVector<double>& outT,
                              QVector<double>& outM1)
    {
        outT.clear();
        outM1.clear();
        outT.reserve(samples.size() + events.size() + 4);
        outM1.reserve(samples.size() + events.size() + 4);

        double sign = +1.0;
        double tReset = result.excitationTimesSec.isEmpty()
                          ? tMin
                          : result.excitationTimesSec.first();
        // If the first sample is before tReset (e.g. no excitation), start
        // from the sample time itself so we don't integrate across nothing.
        if (!samples.isEmpty() && samples.first() < tReset)
        {
            tReset = samples.first();
        }
        double currentT = tReset;
        double unsignedM0 = 0.0;
        double unsignedM1 = 0.0;

        auto sampleGradientAt = [&](double t) -> double {
            const int n = std::min(gTime.size(), gVal.size());
            if (n <= 0 || t < gTime.first() || t > gTime[n - 1])
            {
                return 0.0;
            }
            if (n == 1 || t <= gTime.first())
            {
                return gVal.first();
            }
            if (t >= gTime[n - 1])
            {
                return gVal[n - 1];
            }
            auto it = std::lower_bound(gTime.constBegin(), gTime.constBegin() + n, t);
            const int i1 = static_cast<int>(it - gTime.constBegin());
            if (i1 <= 0)
            {
                return gVal[0];
            }
            const int i0 = i1 - 1;
            const double t0 = gTime[i0];
            const double t1 = gTime[i1];
            if (!(t1 > t0))
            {
                return gVal[i0];
            }
            const double alpha = (t - t0) / (t1 - t0);
            return gVal[i0] + alpha * (gVal[i1] - gVal[i0]);
        };

        auto nextGradientBreakpoint = [&](double t, double target) -> double {
            const int n = gTime.size();
            if (n <= 1 || t >= gTime[n - 1])
            {
                return target;
            }
            auto it = std::upper_bound(gTime.constBegin(), gTime.constEnd(), t + 1e-15);
            if (it == gTime.constEnd())
            {
                return target;
            }
            return std::min(target, *it);
        };

        auto integrateLinearSegment = [](double a,
                                         double b,
                                         double tRef,
                                         double ga,
                                         double gb,
                                         double& m0Out,
                                         double& m1Out) {
            m0Out = 0.0;
            m1Out = 0.0;
            const double h = b - a;
            if (!(h > 0.0))
            {
                return;
            }
            const double slope = (gb - ga) / h;
            const double aRel = a - tRef;
            m0Out = ga * h + 0.5 * slope * h * h;
            m1Out = ga * (aRel * h + 0.5 * h * h)
                  + slope * (0.5 * aRel * h * h + (h * h * h) / 3.0);
        };

        auto reportedM1At = [&](double t) {
            return sign * (unsignedM1 - (t - tReset) * unsignedM0);
        };

        auto advanceTo = [&](double targetT) {
            if (!(targetT > currentT + 1e-15))
            {
                return;
            }
            while (currentT < targetT - 1e-15)
            {
                double nextT = nextGradientBreakpoint(currentT, targetT);
                if (!(nextT > currentT))
                {
                    nextT = targetT;
                }
                const double ga = sampleGradientAt(currentT);
                const double gb = sampleGradientAt(nextT);
                double m0Seg = 0.0;
                double m1Seg = 0.0;
                integrateLinearSegment(currentT, nextT, tReset, ga, gb, m0Seg, m1Seg);
                unsignedM0 += m0Seg;
                unsignedM1 += m1Seg;
                currentT = nextT;
            }
        };

        size_t ei = 0;
        size_t si = 0;
        while (ei < events.size() || si < samples.size())
        {
            const double nextEvtT = (ei < events.size()) ? events[ei].tSec
                                                         : std::numeric_limits<double>::infinity();
            const double nextSampT = (si < samples.size()) ? samples[si]
                                                           : std::numeric_limits<double>::infinity();

            if (nextEvtT <= nextSampT)
            {
                // Advance cumulative M1 up to the event time.
                advanceTo(nextEvtT);
                if (events[ei].kind == EvKind::Reset)
                {
                    // Drop M1 to 0 at reset moments, emit zero, restart epoch.
                    if (outM1.isEmpty() || outT.last() < nextEvtT - 1e-15)
                    {
                        outT.append(nextEvtT);
                        outM1.append(0.0);
                    }
                    else
                    {
                        // Same timestamp as previous emission; overwrite with 0.
                        outT.last() = nextEvtT;
                        outM1.last() = 0.0;
                    }
                    sign = +1.0;
                    tReset = nextEvtT;
                    currentT = nextEvtT;
                    unsignedM0 = 0.0;
                    unsignedM1 = 0.0;
                }
                else // EvKind::Flip
                {
                    outT.append(nextEvtT);
                    outM1.append(reportedM1At(nextEvtT));
                    sign = -sign;
                }
                ++ei;
            }
            else
            {
                // Plain raster-grid sample point.
                advanceTo(nextSampT);
                outT.append(nextSampT);
                outM1.append(reportedM1At(nextSampT));
                ++si;
            }
        }
    };
    
    if (Settings::getInstance().getPerformanceDebugEnabled()) {
        LOG_DEBUG_CAT("Performance", QString("M1 preparation took %1 ms").arg(perfTimer.restart()));
    }

    // First pass: x populates the canonical t-stream and the M1x curve.
    walkAndCollect(gxTime, gxVal, result.tSec, result.m1x);
    // Subsequent passes reuse the same event/samples schedule, so t streams
    // are identical in length and ordering. We discard their t-output and
    // keep only the per-axis m1 vector.
    QVector<double> tTmpDiscard;
    walkAndCollect(gyTime, gyVal, tTmpDiscard, result.m1y);
    walkAndCollect(gzTime, gzVal, tTmpDiscard, result.m1z);
    // Light sanity check: per-axis output lengths should match.
    if (result.m1x.size() != result.m1y.size() ||
        result.m1x.size() != result.m1z.size())
    {
        result.warnings << QStringLiteral(
            "Internal warning: per-axis M1 output sizes disagree (%1, %2, %3). "
            "Plot may be inconsistent.").arg(result.m1x.size())
                                          .arg(result.m1y.size())
                                          .arg(result.m1z.size());
    }
    
    if (Settings::getInstance().getPerformanceDebugEnabled()) {
        LOG_DEBUG_CAT("Performance", QString("M1 core integration and collection took %1 ms").arg(perfTimer.restart()));
    }

    result.valid = true;
    result.ok = true;
    return result;
}

} // namespace M1Calculator
