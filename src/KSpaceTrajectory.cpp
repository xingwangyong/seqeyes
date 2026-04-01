#include "KSpaceTrajectory.h"

#include "SeriesBuilder.h"
#include "ExternalSequence.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <limits>
#include <numeric>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QtGlobal>
#include <QTextStream>
#include "Settings.h"
#include <QDebug>

namespace KSpaceTrajectory
{
namespace
{
    bool ktrajDebugEnabled()
    {
        const QByteArray v = qgetenv("SEQEYES_KTRAJ_DEBUG");
        if (v.isEmpty())
            return false;
        const QByteArray lower = v.trimmed().toLower();
        return (lower == "1" || lower == "true" || lower == "yes" || lower == "on");
    }

    QString ktrajDebugDir()
    {
        const QByteArray custom = qgetenv("SEQEYES_KTRAJ_DEBUG_DIR");
        if (!custom.isEmpty())
            return QString::fromUtf8(custom);
        return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../test/ktraj_debug");
    }

    bool writeTextFile(const QString& path, const QString& content)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        QTextStream ts(&f);
        ts.setRealNumberNotation(QTextStream::ScientificNotation);
        ts.setRealNumberPrecision(16);
        ts << content;
        return true;
    }

    double internalToSeconds(double value, double tFactor)
    {
        if (tFactor == 0.0)
            return value;
        double microseconds = value / tFactor;
        return microseconds * 1e-6;
    }

    double rfCenterUs(SeqBlock* blk, const RFEvent& rf)
    {
        if (!blk)
            return 0.0;
        if (rf.center >= 0.0)
            return rf.center;

        // Legacy fallback path (mainly Pulseq <= v1.4.x):
        // RFEvent has no explicit 'center' field in older formats, so we
        // estimate it from the RF magnitude peak. This path should not be
        // used for v1.5+ files where rf.center is provided by the sequence.
        static bool s_warnedLegacyRfCenterFallback = false;
        if (!s_warnedLegacyRfCenterFallback)
        {
            qWarning() << "RF center metadata missing; using legacy RF-center fallback"
                       << "(peak-based, sample-center timing). This is expected for older"
                       << "Pulseq files (e.g. v1.4.x).";
            s_warnedLegacyRfCenterFallback = true;
        }

        int length = blk->GetRFLength();
        if (length <= 0)
            return 0.0;

        const float* ampPtr = blk->GetRFAmplitudePtr();
        float dwell = blk->GetRFDwellTime();
        if (dwell <= 0.0f)
            dwell = 1.0f;

        std::vector<double> signal(length);
        std::vector<double> times(length);
        for (int i = 0; i < length; ++i)
        {
            signal[i] = std::abs(static_cast<double>(ampPtr[i]));
            // Match MATLAB legacy default RF time raster:
            // rf.t = ((1:N)-0.5) * dwell, i.e. sample centers at (i+0.5)*dwell.
            times[i] = (static_cast<double>(i) + 0.5) * static_cast<double>(dwell);
        }

        double maxVal = 0.0;
        for (double v : signal)
            if (std::abs(v) > std::abs(maxVal))
                maxVal = v;

        std::vector<int> peakIdx;
        for (int i = 0; i < signal.size(); ++i)
        {
            if (std::abs(signal[i]) >= std::abs(maxVal) * 0.99999)
                peakIdx.push_back(i);
        }
        if (peakIdx.empty())
            return 0.0;

        double tc = (times[peakIdx.front()] + times[peakIdx.back()]) / 2.0;
        return tc;
    }

    double estimateFlipAngleDeg(SeqBlock* blk)
    {
        if (!blk || !blk->isRF())
            return 0.0;
        int len = blk->GetRFLength();
        if (len <= 1)
            return 0.0;

        const float* ampPtr = blk->GetRFAmplitudePtr();
        const float* phasePtr = blk->GetRFPhasePtr();
        if (!ampPtr || !phasePtr)
            return 0.0;
        float dwellUs = blk->GetRFDwellTime();
        if (dwellUs <= 0.0f)
            dwellUs = 1.0f;

        // Follow MATLAB logic (left Riemann sum on rf.signal):
        // rf.signal includes amplitude scaling in MATLAB: amplitude * mag .* exp(i*phase)
        // Rebuild those complex samples here and integrate with left rectangles.
        // flipAngleDeg = abs(sum(rf.signal(1:end-1) .* (rf.t(2:end)-rf.t(1:end-1)))) * 360
        // with uniform dt = dwellUs * 1e-6
        const RFEvent& rf = blk->GetRFEvent();
        double dt = static_cast<double>(dwellUs) * 1e-6;
        const double rfScale = static_cast<double>(rf.amplitude);
        std::complex<double> accum(0.0, 0.0);
        for (int i = 0; i < len - 1; ++i)
        {
            double magNorm = static_cast<double>(ampPtr[i]);
            double phaseRad = static_cast<double>(phasePtr[i]);
            if (!std::isfinite(magNorm) || !std::isfinite(phaseRad))
                continue;
            double mag = magNorm * rfScale;
            accum += std::polar(mag, phaseRad) * dt;
        }
        return std::abs(accum) * 360.0;
    }

    char classifyRfUse(SeqBlock* blk, const RFEvent& rf, bool supportsMetadata, bool& guessedUse, double b0Tesla, double gammaHzPerT)
    {
        if (supportsMetadata && rf.use != 0 && rf.use != 'u' && rf.use != 'U')
            return rf.use;
        guessedUse = true;
        double flipAngle = estimateFlipAngleDeg(blk);
        if (flipAngle < 90.01)
            return 'e';
        // MATLAB parity: detect fat-sat ('s') by long duration and off-resonance near -3.45 ppm
        // Duration: derive from RF samples and dwell time
        double dur_s = 0.0;
        if (blk)
        {
            int len = blk->GetRFLength();
            float dwellUs = blk->GetRFDwellTime();
            if (len > 1 && dwellUs > 0.0f)
                dur_s = static_cast<double>(len - 1) * static_cast<double>(dwellUs) * 1e-6;
        }
        // Off-resonance ppm:
        // - v1.5.x: rf.freqPPM populated directly
        // - v1.4.x: compute from freqOffset if B0 is available: ppm = 1e6 * freqOffset / (gamma * B0)
        // If B0 not defined, assume 3T (warn once).
        static bool s_warnedDefaultB0 = false;
        if (b0Tesla <= 0.0)
        {
            b0Tesla = 3.0;
            if (!s_warnedDefaultB0)
            {
                qWarning() << "RF use guess, B0 not defined in sequence; assuming 3.0 T for RF-use detection.";
                s_warnedDefaultB0 = true;
            }
        }
        double freqPPM = static_cast<double>(rf.freqPPM);
        if (std::abs(freqPPM) < 1e-12 && b0Tesla > 0.0 && std::abs(gammaHzPerT) > 0.0)
        {
            freqPPM = 1e6 * static_cast<double>(rf.freqOffset) / (gammaHzPerT * b0Tesla);
        }
        // Widen detection band for saturation pulses to [-4.5, -3.0] ppm
        if (dur_s > 6e-3 && freqPPM >= -4.5 && freqPPM <= -3.0)
            return 's';
        return 'r';
    }

    double sampleGradientAt(const QVector<double>& times, const QVector<double>& values, double t, int& cursor)
    {
        // Robust sampling with clamped endpoints and safe neighbor lookup.
        if (times.isEmpty() || values.isEmpty())
            return 0.0;
        if (times.size() == 1)
            return values.first();
        // Endpoint clamps
        if (t <= times.first())
            return values.first();
        if (t >= times.last())
            return values.last();

        // Find first index where times[i] >= t
        auto it = std::lower_bound(times.begin(), times.end(), t);
        int i1 = static_cast<int>(it - times.begin());
        int i0 = std::max(0, i1 - 1);
        // Cache cursor near current neighborhood for subsequent calls
        cursor = i0;

        double t0 = times[i0];
        double t1 = times[i1];
        double v0 = values[i0];
        double v1 = values[i1];
        if (t1 <= t0)
            return v1;
        double alpha = (t - t0) / (t1 - t0);
        alpha = std::clamp(alpha, 0.0, 1.0);
        return v0 + (v1 - v0) * alpha;
    }

    void sanitizeGradientSeries(QVector<double>& times, QVector<double>& values)
    {
        if (times.size() != values.size())
        {
            int n = std::min(times.size(), values.size());
            times.resize(n);
            values.resize(n);
        }
        int writeIdx = 0;
        double prevTime = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < times.size(); ++i)
        {
            double val = values[i];
            double t = times[i];
            if (!std::isfinite(t))
                continue;

            bool isBreak = !std::isfinite(val);
            if (writeIdx > 0)
            {
                if (t < prevTime - 1e-15)
                    continue;
                if (std::abs(t - prevTime) <= 1e-15)
                {
                    // Preserve duplicate timestamps if one of them is a break marker.
                    bool prevBreak = !std::isfinite(values[writeIdx - 1]);
                    if (isBreak && prevBreak)
                        continue;
                    if (isBreak || prevBreak)
                    {
                        times[writeIdx] = t;
                        values[writeIdx] = isBreak ? std::numeric_limits<double>::quiet_NaN() : val;
                        prevTime = t;
                        ++writeIdx;
                        continue;
                    }
                    times[writeIdx - 1] = t;
                    values[writeIdx - 1] = val;
                    prevTime = t;
                    continue;
                }
            }
            times[writeIdx] = t;
            values[writeIdx] = isBreak ? std::numeric_limits<double>::quiet_NaN() : val;
            prevTime = t;
            ++writeIdx;
        }
        times.resize(writeIdx);
        values.resize(writeIdx);
    }

    double trapezoidGradientValue(const GradEvent& grad, double localSec)
    {
        if (localSec < 0.0)
            return 0.0;
        double rampUpSec = static_cast<double>(grad.rampUpTime) * 1e-6;
        double flatSec = static_cast<double>(grad.flatTime) * 1e-6;
        double rampDownSec = static_cast<double>(grad.rampDownTime) * 1e-6;
        double totalSec = rampUpSec + flatSec + rampDownSec;
        if (localSec > totalSec || totalSec <= 0.0)
            return 0.0;
        double amp = static_cast<double>(grad.amplitude);
        if (localSec <= rampUpSec && rampUpSec > 0.0)
            return amp * (localSec / rampUpSec);
        if (localSec <= rampUpSec + flatSec)
            return amp;
        if (rampDownSec > 0.0)
        {
            double t = localSec - rampUpSec - flatSec;
            if (t <= rampDownSec)
                return amp * (1.0 - t / rampDownSec);
        }
        return 0.0;
    }

    double arbitraryGradientValue(SeqBlock* blk,
                                  int channel,
                                  const GradEvent& grad,
                                  double localSec,
                                  double gradientRasterUs,
                                  double firstOverridePhys = std::numeric_limits<double>::quiet_NaN(),
                                  double lastOverridePhys = std::numeric_limits<double>::quiet_NaN())
    {
        if (localSec < 0.0)
            return 0.0;
        int numSamples = blk->GetArbGradNumSamples(channel);
        const float* shapePtr = blk->GetArbGradShapePtr(channel);
        if (numSamples <= 0 || !shapePtr)
            return 0.0;
        double rasterUs = (gradientRasterUs > 0.0 ? gradientRasterUs : 10.0);
        double rasterSec = rasterUs * 1e-6;
        auto deriveEdgeSample = [&](bool first) -> double {
            if (numSamples <= 0)
                return 0.0;
            if (numSamples == 1)
                return static_cast<double>(shapePtr[0]);
            if (first)
                return 0.5 * (3.0 * static_cast<double>(shapePtr[0]) - static_cast<double>(shapePtr[1]));
            return 0.5 * (3.0 * static_cast<double>(shapePtr[numSamples - 1]) -
                          static_cast<double>(shapePtr[numSamples - 2]));
        };

        const bool hasFirst = (grad.first != FLOAT_UNDEFINED);
        const bool hasLast = (grad.last != FLOAT_UNDEFINED);
        const double amp = static_cast<double>(grad.amplitude);
        double firstPhys = 0.0;
        double lastPhys = 0.0;
        if (std::isfinite(firstOverridePhys))
        {
            firstPhys = firstOverridePhys;
        }
        else
        {
            double firstVal = hasFirst ? static_cast<double>(grad.first) : deriveEdgeSample(true);
            if (hasFirst && std::abs(firstVal) > 1.0 + 1e-6 && std::abs(amp) > 0.0)
                firstVal /= amp;
            firstPhys = firstVal * amp;
        }
        if (std::isfinite(lastOverridePhys))
        {
            lastPhys = lastOverridePhys;
        }
        else
        {
            double lastVal = hasLast ? static_cast<double>(grad.last) : deriveEdgeSample(false);
            if (hasLast && std::abs(lastVal) > 1.0 + 1e-6 && std::abs(amp) > 0.0)
                lastVal /= amp;
            lastPhys = lastVal * amp;
        }
        // Robust v1.5.x oversampling detection:
        // use parser helper plus raw grad.timeShape sentinel (-1).
        const bool oversampled = blk->isArbGradWithOversampling(channel) ||
                                 (grad.timeShape == -1);
        const double totalSec = oversampled
            ? (static_cast<double>(numSamples) + 1.0) * 0.5 * rasterSec
            : static_cast<double>(numSamples) * rasterSec;
        if (localSec > totalSec)
            return 0.0;
        if (localSec <= 0.0)
            return firstPhys;
        if (localSec >= totalSec)
            return lastPhys;

        if (oversampled)
        {
            // Oversampled arbitrary semantics:
            // t=0 -> first, t=(j+1)*0.5*dt -> shape[j], t=(N+1)*0.5*dt -> last.
            const double halfRasterSec = 0.5 * rasterSec;
            const double s = localSec / halfRasterSec; // sample-domain coordinate

            if (s < 1.0)
            {
                // first -> sample[0] on [0, 0.5*dt]
                const double alpha = std::clamp(s, 0.0, 1.0);
                const double s0 = static_cast<double>(shapePtr[0]) * amp;
                return firstPhys + (s0 - firstPhys) * alpha;
            }

            if (s >= static_cast<double>(numSamples))
            {
                // sample[N-1] -> last on [N*0.5*dt, (N+1)*0.5*dt]
                const double alpha = std::clamp(s - static_cast<double>(numSamples), 0.0, 1.0);
                const double sLast = static_cast<double>(shapePtr[numSamples - 1]) * amp;
                return sLast + (lastPhys - sLast) * alpha;
            }

            // Interior sample-to-sample interpolation.
            // sample index i lives at s=i+1.
            const int idx0 = std::clamp(static_cast<int>(std::floor(s)) - 1, 0, numSamples - 1);
            const int idx1 = std::clamp(idx0 + 1, 0, numSamples - 1);
            const double s0 = static_cast<double>(idx0 + 1);
            const double alpha = std::clamp(s - s0, 0.0, 1.0);
            const double v0 = static_cast<double>(shapePtr[idx0]) * amp;
            const double v1 = static_cast<double>(shapePtr[idx1]) * amp;
            return v0 + (v1 - v0) * alpha;
        }

        const double u = localSec / rasterSec;
        if (u < 0.5)
        {
            const double alpha = u / 0.5;
            const double s0 = static_cast<double>(shapePtr[0]) * amp;
            return firstPhys + (s0 - firstPhys) * alpha;
        }

        const double uLastCenter = static_cast<double>(numSamples) - 0.5;
        if (u >= uLastCenter)
        {
            const double alpha = std::clamp((u - uLastCenter) / 0.5, 0.0, 1.0);
            const double sLast = static_cast<double>(shapePtr[numSamples - 1]) * amp;
            return sLast + (lastPhys - sLast) * alpha;
        }

        const int idx0 = static_cast<int>(std::floor(u - 0.5));
        const int idx1 = idx0 + 1;
        const double t0 = static_cast<double>(idx0) + 0.5;
        const double alpha = std::clamp(u - t0, 0.0, 1.0);
        const double v0 = static_cast<double>(shapePtr[idx0]) * amp;
        const double v1 = static_cast<double>(shapePtr[idx1]) * amp;
        return v0 + (v1 - v0) * alpha;
    }

    double extTrapGradientValue(SeqBlock* blk, int channel, const GradEvent& grad, double localSec)
    {
        if (localSec < 0.0)
            return 0.0;
        const std::vector<long>& timesUs = blk->GetExtTrapGradTimes(channel);
        const std::vector<float>& shape = blk->GetExtTrapGradShape(channel);
        if (timesUs.empty() || shape.empty() || timesUs.size() != shape.size())
            return 0.0;
        double localUs = localSec * 1e6;
        if (localUs <= static_cast<double>(timesUs.front()))
            return static_cast<double>(shape.front()) * static_cast<double>(grad.amplitude);
        if (localUs >= static_cast<double>(timesUs.back()))
            return static_cast<double>(shape.back()) * static_cast<double>(grad.amplitude);
        int idx1 = -1;
        for (int j = 1; j < static_cast<int>(timesUs.size()); ++j)
        {
            if (localUs <= static_cast<double>(timesUs[j]))
            {
                idx1 = j;
                break;
            }
        }
        if (idx1 <= 0)
            return static_cast<double>(shape.front()) * static_cast<double>(grad.amplitude);
        int idx0 = idx1 - 1;
        double t0 = static_cast<double>(timesUs[idx0]) * 1e-6;
        double t1 = static_cast<double>(timesUs[idx1]) * 1e-6;
        double span = t1 - t0;
        if (span <= 0.0)
            return static_cast<double>(shape[idx1]) * static_cast<double>(grad.amplitude);
        double alpha = (localSec - t0) / span;
        alpha = std::clamp(alpha, 0.0, 1.0);
        double v0 = static_cast<double>(shape[idx0]);
        double v1 = static_cast<double>(shape[idx1]);
        return (v0 + (v1 - v0) * alpha) * static_cast<double>(grad.amplitude);
    }

    double gradientValueFromBlock(SeqBlock* blk,
                                  int channel,
                                  double timeSec,
                                  double blockStartSec,
                                  double gradientRasterUs,
                                  double firstOverridePhys = std::numeric_limits<double>::quiet_NaN(),
                                  double lastOverridePhys = std::numeric_limits<double>::quiet_NaN())
    {
        if (!blk)
            return 0.0;
        if (!(blk->isTrapGradient(channel) || blk->isArbitraryGradient(channel) || blk->isExtTrapGradient(channel)))
            return 0.0;
        const GradEvent& grad = blk->GetGradEvent(channel);
        double eventStartSec = blockStartSec + static_cast<double>(grad.delay) * 1e-6;
        double localSec = timeSec - eventStartSec;
        if (blk->isTrapGradient(channel))
            return trapezoidGradientValue(grad, localSec);
        if (blk->isArbitraryGradient(channel))
            return arbitraryGradientValue(blk, channel, grad, localSec, gradientRasterUs, firstOverridePhys, lastOverridePhys);
        if (blk->isExtTrapGradient(channel))
            return extTrapGradientValue(blk, channel, grad, localSec);
        return 0.0;
    }

    double sampleLocalLinear(const QVector<double>& tLocal, const QVector<double>& vLocal, double t)
    {
        if (tLocal.isEmpty() || vLocal.isEmpty())
            return 0.0;
        if (tLocal.size() == 1)
            return vLocal.first();
        if (t < 0.0)
            return 0.0;
        if (t <= tLocal.first())
            return vLocal.first();
        if (t >= tLocal.last())
            return vLocal.last();
        auto it = std::lower_bound(tLocal.begin(), tLocal.end(), t);
        int i1 = static_cast<int>(it - tLocal.begin());
        int i0 = std::max(0, i1 - 1);
        double t0 = tLocal[i0];
        double t1 = tLocal[i1];
        if (t1 <= t0)
            return vLocal[i1];
        double a = (t - t0) / (t1 - t0);
        a = std::clamp(a, 0.0, 1.0);
        return vLocal[i0] + (vLocal[i1] - vLocal[i0]) * a;
    }

    void mergeTimeVectors(QVector<double>& base, const QVector<double>& extra, double tFactor)
    {
        for (double v : extra)
        {
            double sec = internalToSeconds(v, tFactor);
            base.append(sec);
        }
    }

}

Result compute(const Input& input)
{
    Result result;
    const bool debugDump = ktrajDebugEnabled();
    if (input.blocks.empty() || input.blockEdges.size() < 2)
        return result;

    const double tacc = 1e-10;
    auto roundAcc = [&](double sec) -> double {
        if (!std::isfinite(sec))
            return sec;
        return tacc * std::llround(sec / tacc);
    };
    auto clampNonNegative = [](double sec) -> double {
        return sec < 0.0 ? 0.0 : sec;
    };
    auto internalToSecRounded = [&](double internal) -> double {
        double sec = internalToSeconds(internal, input.tFactor);
        return clampNonNegative(roundAcc(sec));
    };

    QVector<double> gxTime, gxValue;
    QVector<double> gyTime, gyValue;
    QVector<double> gzTime, gzValue;
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges, input.tFactor, 0, gxTime, gxValue, input.gradientRasterUs);
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges, input.tFactor, 1, gyTime, gyValue, input.gradientRasterUs);
    SeriesBuilder::buildGradientSeries(input.blocks, input.blockEdges, input.tFactor, 2, gzTime, gzValue, input.gradientRasterUs);
    sanitizeGradientSeries(gxTime, gxValue);
    sanitizeGradientSeries(gyTime, gyValue);
    sanitizeGradientSeries(gzTime, gzValue);

    QVector<double> gxTimeSec(gxTime.size());
    QVector<double> gyTimeSec(gyTime.size());
    QVector<double> gzTimeSec(gzTime.size());
    for (int i = 0; i < gxTime.size(); ++i) gxTimeSec[i] = internalToSeconds(gxTime[i], input.tFactor);
    for (int i = 0; i < gyTime.size(); ++i) gyTimeSec[i] = internalToSeconds(gyTime[i], input.tFactor);
    for (int i = 0; i < gzTime.size(); ++i) gzTimeSec[i] = internalToSeconds(gzTime[i], input.tFactor);

    QVector<double> excitationSecondsRounded;
    QVector<double> refocusSecondsRounded;
    bool guessedAny = false;

    double gammaHzPerT = Settings::getInstance().getGamma();
    QVector<char> rfUsePerBlock;
    rfUsePerBlock.resize(static_cast<int>(input.blocks.size()));
    std::fill(rfUsePerBlock.begin(), rfUsePerBlock.end(), 0);
    for (int i = 0; i < input.blocks.size(); ++i)
    {
        SeqBlock* blk = input.blocks[i];
        if (!blk)
            continue;
        if (!blk->isRF())
        {
            rfUsePerBlock[i] = 0;
            continue;
        }

        const RFEvent& rf = blk->GetRFEvent();
        bool guessed = false;
        char useChar = classifyRfUse(blk, rf, input.supportsRfUseMetadata, guessed,
                                     input.b0Tesla, gammaHzPerT);
        guessedAny |= guessed;
        rfUsePerBlock[i] = useChar ? useChar : 'u';

        double centerUs = rfCenterUs(blk, rf);
        double internalTime = input.blockEdges[i] + (rf.delay + centerUs) * input.tFactor;

        if (useChar == 'e' || useChar == 'E')
        {
            result.excitationTimesInternal.append(internalTime);
            excitationSecondsRounded.append(internalToSecRounded(internalTime));
        }
        else if (useChar == 'r' || useChar == 'R')
        {
            result.refocusingTimesInternal.append(internalTime);
            refocusSecondsRounded.append(internalToSecRounded(internalTime));
        }
    }

    result.rfUseGuessed = guessedAny;
    if (guessedAny)
    {
        result.warning = QStringLiteral("No RF use in seq file, probably seq file version is older than v1.5.0. Now we have to guess RF use, the trajectory may not be accurate.");
    }
    result.rfUsePerBlock = rfUsePerBlock;

    double rfRasterSec = input.rfRasterUs > 0.0 ? input.rfRasterUs * 1e-6 : 0.0;
    double gradRasterSec = input.gradientRasterUs > 0.0 ? input.gradientRasterUs * 1e-6 : 0.0;
    double totalDurationSec = internalToSecRounded(input.blockEdges.last());

    QVector<double> adcSecondsRounded;
    adcSecondsRounded.reserve(input.adcEventTimesInternal.size());
    for (double v : input.adcEventTimesInternal)
    {
        adcSecondsRounded.append(internalToSecRounded(v));
    }
    result.t_adc = adcSecondsRounded;

    QVector<double> timeCandidates;
    timeCandidates.reserve(excitationSecondsRounded.size() * 3
                           + refocusSecondsRounded.size() * 2
                           + adcSecondsRounded.size() + 8);

    auto addCandidate = [&](double sec){
        if (!std::isfinite(sec))
            return;
        sec = roundAcc(sec);
        sec = clampNonNegative(sec);
        timeCandidates.append(sec);
    };

    // Build continuous trajectory time base from gradient waveform support points.
    // Avoid forcing dense 0..T raster, which can diverge from MATLAB-style trajectory sampling.
    for (double sec : gxTimeSec) addCandidate(sec);
    for (double sec : gyTimeSec) addCandidate(sec);
    for (double sec : gzTimeSec) addCandidate(sec);
    if (gradRasterSec > 0.0 && std::isfinite(totalDurationSec) && totalDurationSec > 0.0)
    {
        const int nSteps = std::max(1, static_cast<int>(std::llround(totalDurationSec / gradRasterSec)));
        for (int i = 0; i <= nSteps; ++i)
            addCandidate(static_cast<double>(i) * gradRasterSec);
    }

    addCandidate(0.0);
    addCandidate(totalDurationSec);

    for (double sec : excitationSecondsRounded)
    {
        addCandidate(sec);
        const double dtRf = (rfRasterSec > 0.0 ? rfRasterSec : 1e-6);
        addCandidate(clampNonNegative(sec - dtRf));
        addCandidate(clampNonNegative(sec - 2.0 * dtRf));
    }
    for (double sec : refocusSecondsRounded)
    {
        addCandidate(sec);
        const double dtRf = (rfRasterSec > 0.0 ? rfRasterSec : 1e-6);
        addCandidate(clampNonNegative(sec - dtRf));
    }
    for (double sec : adcSecondsRounded) addCandidate(sec);

    std::sort(timeCandidates.begin(), timeCandidates.end());
    QVector<double> timeGrid;
    timeGrid.reserve(timeCandidates.size());
    for (double v : timeCandidates)
    {
        if (timeGrid.isEmpty() || std::abs(v - timeGrid.back()) > tacc * 0.5)
            timeGrid.append(v);
    }
    if (timeGrid.size() < 2)
    {
        double extra = timeGrid.isEmpty() ? tacc : (timeGrid.back() + tacc);
        timeGrid.append(extra);
    }
    result.t = timeGrid;

    QVector<double> blockEdgesSec(input.blockEdges.size());
    for (int i = 0; i < input.blockEdges.size(); ++i)
        blockEdgesSec[i] = internalToSecRounded(input.blockEdges[i]);

    const int nBlocks = static_cast<int>(input.blocks.size());
    QVector<std::array<double, 3>> legacyFirstOverride(nBlocks);
    QVector<std::array<double, 3>> legacyLastOverride(nBlocks);
    QVector<std::array<QVector<double>, 3>> legacyRestoredTimes(nBlocks);
    QVector<std::array<QVector<double>, 3>> legacyRestoredValues(nBlocks);
    QVector<std::array<bool, 3>> legacyUseRestored(nBlocks);
    for (int i = 0; i < nBlocks; ++i)
    {
        legacyFirstOverride[i] = { qQNaN(), qQNaN(), qQNaN() };
        legacyLastOverride[i] = { qQNaN(), qQNaN(), qQNaN() };
        legacyUseRestored[i] = { false, false, false };
    }

    // Legacy compatibility for files where arbitrary gradients do not carry
    // explicit first/last fields (e.g. old v1.4.x sources). MATLAB read()
    // reconstructs these using channel continuity and odd-step integration.
    // Reproduce the same semantics here for trajectory computation.
    {
        static bool s_warnedLegacyGradEdgeRecovery = false;
        std::array<double, 3> prevLast = {0.0, 0.0, 0.0};
        for (int bi = 0; bi < nBlocks; ++bi)
        {
            SeqBlock* blk = input.blocks[bi];
            const double blockDurSec = (bi + 1 < blockEdgesSec.size())
                ? (blockEdgesSec[bi + 1] - blockEdgesSec[bi])
                : 0.0;
            for (int ch = 0; ch < 3; ++ch)
            {
                if (!blk)
                {
                    prevLast[ch] = 0.0;
                    continue;
                }
                if (!blk->isArbitraryGradient(ch))
                {
                    prevLast[ch] = 0.0;
                    continue;
                }

                const GradEvent& grad = blk->GetGradEvent(ch);
                const bool hasFirst = (grad.first != FLOAT_UNDEFINED);
                const bool hasLast = (grad.last != FLOAT_UNDEFINED);
                if (hasFirst && hasLast)
                {
                    // Keep prevLast untouched for modern files where metadata exists.
                    continue;
                }

                const int n = blk->GetArbGradNumSamples(ch);
                const float* s = blk->GetArbGradShapePtr(ch);
                if (n <= 0 || !s)
                {
                    prevLast[ch] = 0.0;
                    continue;
                }

                if (!s_warnedLegacyGradEdgeRecovery)
                {
                    qWarning() << "Arbitrary gradient edge metadata (first/last) missing;"
                               << "using legacy continuity-based edge recovery for trajectory.";
                    s_warnedLegacyGradEdgeRecovery = true;
                }

                const double amp = static_cast<double>(grad.amplitude);
                const double firstPhys = (grad.delay > 0) ? 0.0 : prevLast[ch];
                legacyFirstOverride[bi][ch] = firstPhys;

                double lastPhys = static_cast<double>(s[n - 1]) * amp;
                const bool oversampled = blk->isArbGradWithOversampling(ch) ||
                                         (grad.timeShape == -1);
                if (!oversampled)
                {
                    // MATLAB v1.4.x reconstruction for center-raster arbitrary
                    // gradients: odd-step cumulative endpoint recovery.
                    QVector<double> w(n);
                    double maxAbs = 0.0;
                    for (int k = 0; k < n; ++k)
                    {
                        w[k] = static_cast<double>(s[k]) * amp;
                        maxAbs = std::max(maxAbs, std::abs(w[k]));
                    }

                    QVector<double> oddStep2(n + 1);
                    oddStep2[0] = firstPhys; // + sign at index 1 in MATLAB
                    for (int k = 0; k < n; ++k)
                    {
                        oddStep2[k + 1] = 2.0 * w[k];
                    }
                    for (int k = 0; k < oddStep2.size(); ++k)
                    {
                        const double sign = ((k % 2) == 0) ? 1.0 : -1.0;
                        oddStep2[k] *= sign;
                    }
                    QVector<double> oddRest(oddStep2.size());
                    double csum = 0.0;
                    for (int k = 0; k < oddStep2.size(); ++k)
                    {
                        csum += oddStep2[k];
                        const double sign = ((k % 2) == 0) ? 1.0 : -1.0;
                        oddRest[k] = csum * sign;
                    }

                    QVector<double> oddInterp(n + 1);
                    oddInterp[0] = firstPhys;
                    for (int k = 0; k < n - 1; ++k)
                        oddInterp[k + 1] = 0.5 * (w[k] + w[k + 1]);
                    oddInterp[n] = oddRest.last();

                    const double thresh = std::numeric_limits<double>::epsilon() + 2e-5 * maxAbs;
                    QVector<double> odd(oddRest.size());
                    for (int k = 0; k < odd.size(); ++k)
                    {
                        odd[k] = (std::abs(oddRest[k] - oddInterp[k]) <= thresh) ? oddInterp[k] : oddRest[k];
                    }

                    const double halfDt = 0.5 * gradRasterSec;
                    QVector<double>& tLocal = legacyRestoredTimes[bi][ch];
                    QVector<double>& vLocal = legacyRestoredValues[bi][ch];
                    tLocal.clear();
                    vLocal.clear();
                    tLocal.reserve(2 * n);
                    vLocal.reserve(2 * n);

                    // Match MATLAB restoreAdditionalShapeSamples() output:
                    // waveform_os = [first, wave(1), odd(2), wave(2), ..., odd(n), wave(n)]
                    // sampled on tt_os = (0:(2*n-1))*0.5*dt
                    tLocal.append(0.0);
                    vLocal.append(odd[0]); // first
                    for (int k = 0; k < n - 1; ++k)
                    {
                        tLocal.append((2.0 * static_cast<double>(k) + 1.0) * halfDt);
                        vLocal.append(w[k]);
                        tLocal.append((2.0 * static_cast<double>(k) + 2.0) * halfDt);
                        vLocal.append(odd[k + 1]);
                    }
                    tLocal.append((2.0 * static_cast<double>(n - 1) + 1.0) * halfDt);
                    vLocal.append(w[n - 1]);

                    legacyUseRestored[bi][ch] = true;
                    lastPhys = oddRest.last();
                }
                legacyLastOverride[bi][ch] = lastPhys;

                const double gradDurSec = static_cast<double>(grad.delay) * 1e-6
                    + (oversampled
                        ? (static_cast<double>(n) + 1.0) * 0.5 * gradRasterSec
                        : static_cast<double>(n) * gradRasterSec);
                prevLast[ch] = (gradDurSec + 1e-12 < blockDurSec) ? 0.0 : lastPhys;
            }
        }
    }

    auto gradientAtSec = [&](double sec, double& gx, double& gy, double& gz) {
        gx = 0.0; gy = 0.0; gz = 0.0;
        if (blockEdgesSec.size() < 2)
            return;
        if (sec < blockEdgesSec.front() || sec >= blockEdgesSec.back())
            return;
        auto it = std::upper_bound(blockEdgesSec.begin(), blockEdgesSec.end(), sec);
        if (it == blockEdgesSec.begin())
            return;
        int blockIdx = static_cast<int>(it - blockEdgesSec.begin()) - 1;
        if (blockIdx < 0 || blockIdx >= static_cast<int>(input.blocks.size()))
            return;
            
        SeqBlock* blk = input.blocks[blockIdx];
        double blockStartSec = blockEdgesSec[blockIdx];

        double lgx = 0.0, lgy = 0.0, lgz = 0.0;
        for (int ch = 0; ch < 3; ++ch)
        {
            if (legacyUseRestored[blockIdx][ch])
            {
                const GradEvent& g = blk->GetGradEvent(ch);
                const double localSec = sec - (blockStartSec + static_cast<double>(g.delay) * 1e-6);
                double v = 0.0;
                if (localSec >= 0.0)
                    v = sampleLocalLinear(legacyRestoredTimes[blockIdx][ch], legacyRestoredValues[blockIdx][ch], localSec);
                if (ch == 0) lgx = v;
                if (ch == 1) lgy = v;
                if (ch == 2) lgz = v;
            }
            else
            {
                const double v = gradientValueFromBlock(blk, ch, sec, blockStartSec, input.gradientRasterUs,
                                                        legacyFirstOverride[blockIdx][ch],
                                                        legacyLastOverride[blockIdx][ch]);
                if (ch == 0) lgx = v;
                if (ch == 1) lgy = v;
                if (ch == 2) lgz = v;
            }
        }
        
        if (blk && blk->isRotation())
        {
            const RotationEvent& rot = blk->GetRotationEvent();
            double w = rot.rotQuaternion[0];
            double x = rot.rotQuaternion[1];
            double y = rot.rotQuaternion[2];
            double z = rot.rotQuaternion[3];
            
            // Quaternion to 3x3 Rotation Matrix
            double R00 = 1.0 - 2.0*y*y - 2.0*z*z;
            double R01 = 2.0*x*y - 2.0*w*z;
            double R02 = 2.0*x*z + 2.0*w*y;
            
            double R10 = 2.0*x*y + 2.0*w*z;
            double R11 = 1.0 - 2.0*x*x - 2.0*z*z;
            double R12 = 2.0*y*z - 2.0*w*x;
            
            double R20 = 2.0*x*z - 2.0*w*y;
            double R21 = 2.0*y*z + 2.0*w*x;
            double R22 = 1.0 - 2.0*x*x - 2.0*y*y;
            
            gx = R00*lgx + R01*lgy + R02*lgz;
            gy = R10*lgx + R11*lgy + R12*lgz;
            gz = R20*lgx + R21*lgy + R22*lgz;
        }
        else
        {
            gx = lgx;
            gy = lgy;
            gz = lgz;
        }
    };

    QVector<double> kxData(timeGrid.size(), 0.0);
    QVector<double> kyData(timeGrid.size(), 0.0);
    QVector<double> kzData(timeGrid.size(), 0.0);
    QVector<double> gxAtGrid(timeGrid.size(), 0.0);
    QVector<double> gyAtGrid(timeGrid.size(), 0.0);
    QVector<double> gzAtGrid(timeGrid.size(), 0.0);
    for (int i = 0; i < timeGrid.size(); ++i)
    {
        gradientAtSec(timeGrid[i], gxAtGrid[i], gyAtGrid[i], gzAtGrid[i]);
    }
    QVector<double> midT;
    QVector<double> midGx;
    QVector<double> midGy;
    QVector<double> midGz;
    QVector<double> midDt;
    if (debugDump)
    {
        int n = static_cast<int>(timeGrid.size()) - 1;
        if (n < 0)
            n = 0;
        midT.reserve(n);
        midGx.reserve(n);
        midGy.reserve(n);
        midGz.reserve(n);
        midDt.reserve(n);
    }
    for (int i = 1; i < timeGrid.size(); ++i)
    {
        double dt = timeGrid[i] - timeGrid[i - 1];
        if (dt <= 0.0)
        {
            kxData[i] = kxData[i - 1];
            kyData[i] = kyData[i - 1];
            kzData[i] = kzData[i - 1];
            continue;
        }
        double mid = timeGrid[i - 1] + 0.5 * dt;
        double gxMid = 0.5 * (gxAtGrid[i - 1] + gxAtGrid[i]);
        double gyMid = 0.5 * (gyAtGrid[i - 1] + gyAtGrid[i]);
        double gzMid = 0.5 * (gzAtGrid[i - 1] + gzAtGrid[i]);
        if (debugDump)
        {
            midT.append(mid);
            midGx.append(gxMid);
            midGy.append(gyMid);
            midGz.append(gzMid);
            midDt.append(dt);
        }
        
        kxData[i] = kxData[i - 1] + gxMid * dt;
        kyData[i] = kyData[i - 1] + gyMid * dt;
        kzData[i] = kzData[i - 1] + gzMid * dt;
    }
    QVector<double> kxRaw = kxData;
    QVector<double> kyRaw = kyData;
    QVector<double> kzRaw = kzData;

    auto indexForSeconds = [&](double sec) -> int {
        double target = roundAcc(sec);
        auto it = std::lower_bound(timeGrid.begin(), timeGrid.end(), target - tacc * 0.5);
        while (it != timeGrid.end())
        {
            if (std::abs(*it - target) <= tacc * 0.5)
                return static_cast<int>(it - timeGrid.begin());
            if (*it > target + tacc * 0.5)
                break;
            ++it;
        }
        return -1;
    };

    QVector<int> excitationIdx;
    for (double sec : excitationSecondsRounded)
    {
        int idx = indexForSeconds(sec);
        if (idx >= 0)
            excitationIdx.append(idx);
    }
    std::sort(excitationIdx.begin(), excitationIdx.end());
    excitationIdx.erase(std::unique(excitationIdx.begin(), excitationIdx.end()), excitationIdx.end());

    QVector<int> refocusIdx;
    for (double sec : refocusSecondsRounded)
    {
        int idx = indexForSeconds(sec);
        if (idx >= 0)
            refocusIdx.append(idx);
    }
    std::sort(refocusIdx.begin(), refocusIdx.end());
    refocusIdx.erase(std::unique(refocusIdx.begin(), refocusIdx.end()), refocusIdx.end());

    QVector<int> boundaries;
    boundaries.append(0);
    boundaries += excitationIdx;
    boundaries += refocusIdx;
    boundaries.append(timeGrid.size() - 1);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    double dkX = -kxData[0];
    double dkY = -kyData[0];
    double dkZ = -kzData[0];
    QString resetLog;
    if (debugDump)
    {
        resetLog = QStringLiteral("event,idx,time_sec,kx_before,ky_before,kz_before,dkx,dky,dkz,kx_after,ky_after,kz_after\n");
    }
    int ptrExc = 0;
    int ptrRef = 0;
    for (int seg = 0; seg < boundaries.size() - 1; ++seg)
    {
        int start = boundaries[seg];
        int endIdx = boundaries[seg + 1];
        bool isExc = (ptrExc < excitationIdx.size() && excitationIdx[ptrExc] == start);
        bool isRef = (ptrRef < refocusIdx.size() && refocusIdx[ptrRef] == start);

        if (isExc)
        {
            double beforeX = kxData[start] + dkX;
            double beforeY = kyData[start] + dkY;
            double beforeZ = kzData[start] + dkZ;
            dkX = -kxData[start];
            dkY = -kyData[start];
            dkZ = -kzData[start];
            if (debugDump)
            {
                resetLog += QString::asprintf(
                    "exc,%d,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e\n",
                    start,
                    timeGrid[start], beforeX, beforeY, beforeZ,
                    dkX, dkY, dkZ,
                    kxData[start] + dkX, kyData[start] + dkY, kzData[start] + dkZ);
            }
            ++ptrExc;
        }
        else if (isRef)
        {
            double beforeX = kxData[start] + dkX;
            double beforeY = kyData[start] + dkY;
            double beforeZ = kzData[start] + dkZ;
            dkX = -2.0 * kxData[start] - dkX;
            dkY = -2.0 * kyData[start] - dkY;
            dkZ = -2.0 * kzData[start] - dkZ;
            if (debugDump)
            {
                resetLog += QString::asprintf(
                    "ref,%d,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e\n",
                    start,
                    timeGrid[start], beforeX, beforeY, beforeZ,
                    dkX, dkY, dkZ,
                    kxData[start] + dkX, kyData[start] + dkY, kzData[start] + dkZ);
            }
            ++ptrRef;
        }

        for (int idx = start; idx < endIdx; ++idx)
        {
            kxData[idx] += dkX;
            kyData[idx] += dkY;
            kzData[idx] += dkZ;
        }
    }

    if (!boundaries.isEmpty())
    {
        int last = boundaries.back();
        kxData[last] += dkX;
        kyData[last] += dkY;
        kzData[last] += dkZ;
    }

    QVector<double> kxPlot = kxData;
    QVector<double> kyPlot = kyData;
    QVector<double> kzPlot = kzData;
    for (int idx : excitationIdx)
    {
        if (idx > 0)
        {
            kxPlot[idx - 1] = qQNaN();
            kyPlot[idx - 1] = qQNaN();
            kzPlot[idx - 1] = qQNaN();
        }
    }

    result.kx = kxPlot;
    result.ky = kyPlot;
    result.kz = kzPlot;

    result.kx_adc.resize(adcSecondsRounded.size());
    result.ky_adc.resize(adcSecondsRounded.size());
    result.kz_adc.resize(adcSecondsRounded.size());

    auto interpolateK = [&](const QVector<double>& data, double ta) -> double {
        if (timeGrid.size() == 1)
            return data.first();
        auto it = std::lower_bound(timeGrid.begin(), timeGrid.end(), ta);
        if (it == timeGrid.begin())
            return data.first();
        if (it == timeGrid.end())
            return data.last();
        int idx1 = static_cast<int>(it - timeGrid.begin());
        if (std::abs(*it - ta) <= tacc * 0.5)
            return data[idx1];
        int idx0 = idx1 - 1;
        double t0 = timeGrid[idx0];
        double t1 = timeGrid[idx1];
        if (t1 <= t0)
            return data[idx1];
        double alpha = (ta - t0) / (t1 - t0);
        return data[idx0] + (data[idx1] - data[idx0]) * alpha;
    };

    for (int ai = 0; ai < adcSecondsRounded.size(); ++ai)
    {
        double ta = adcSecondsRounded[ai];
        int idx = indexForSeconds(ta);
        if (idx >= 0)
        {
            result.kx_adc[ai] = kxData[idx];
            result.ky_adc[ai] = kyData[idx];
            result.kz_adc[ai] = kzData[idx];
        }
        else
        {
            result.kx_adc[ai] = interpolateK(kxData, ta);
            result.ky_adc[ai] = interpolateK(kyData, ta);
            result.kz_adc[ai] = interpolateK(kzData, ta);
        }
    }

    if (debugDump)
    {
        const QString outDir = ktrajDebugDir();
        QDir().mkpath(outDir);
        const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");

        QString gridCsv("idx,time_sec,kx_raw,ky_raw,kz_raw,kx_final,ky_final,kz_final\n");
        for (int i = 0; i < timeGrid.size(); ++i)
        {
            gridCsv += QString::asprintf(
                "%d,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e,%.16e\n",
                i,
                timeGrid[i],
                kxRaw[i], kyRaw[i], kzRaw[i],
                kxData[i], kyData[i], kzData[i]);
        }
        writeTextFile(QDir(outDir).filePath(QString("ktraj_timegrid_%1.csv").arg(stamp)), gridCsv);

        QString midCsv("idx,t_mid_sec,dt_sec,gx_mid,gy_mid,gz_mid\n");
        for (int i = 0; i < midT.size(); ++i)
        {
            midCsv += QString::asprintf(
                "%d,%.16e,%.16e,%.16e,%.16e,%.16e\n",
                i, midT[i], midDt[i], midGx[i], midGy[i], midGz[i]);
        }
        writeTextFile(QDir(outDir).filePath(QString("ktraj_midgrad_%1.csv").arg(stamp)), midCsv);

        writeTextFile(QDir(outDir).filePath(QString("ktraj_resets_%1.csv").arg(stamp)), resetLog);

        QString adcCsv("idx,t_adc_sec,kx_adc,ky_adc,kz_adc\n");
        for (int i = 0; i < adcSecondsRounded.size(); ++i)
        {
            adcCsv += QString::asprintf(
                "%d,%.16e,%.16e,%.16e,%.16e\n",
                i, adcSecondsRounded[i], result.kx_adc[i], result.ky_adc[i], result.kz_adc[i]);
        }
        writeTextFile(QDir(outDir).filePath(QString("ktraj_adc_%1.csv").arg(stamp)), adcCsv);
    }

    return result;
}
} // namespace KSpaceTrajectory
