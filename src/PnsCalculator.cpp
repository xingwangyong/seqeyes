#include "PnsCalculator.h"

#include "ExternalSequence.h"

#include <QFile>
#include <QTextStream>
#include <QHash>
#include <QStringList>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDir>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
struct ParsedAscValues
{
    QHash<QString, double> scalar;
    QHash<QString, QVector<double>> array;
};

bool parseAscAssignment(const QString& line, QString& keyOut, int& indexOut, double& valueOut)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*([A-Za-z0-9_\\.\\[\\]]+?)(?:\\[(\\d+)\\])?\\s*=\\s*([-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?)\\s*$"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
    {
        return false;
    }
    keyOut = m.captured(1).trimmed();
    indexOut = -1;
    if (!m.captured(2).isEmpty())
    {
        indexOut = m.captured(2).toInt();
    }
    bool ok = false;
    valueOut = m.captured(3).toDouble(&ok);
    return ok;
}

QString normalizeAscKey(QString key)
{
    key = key.trimmed();
    static const QRegularExpression idxRe(QStringLiteral("\\[\\d+\\]"));
    key.replace(idxRe, QString());
    return key;
}

QString parseAscInclude(const QString& line)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*\\$include\\s+([A-Za-z0-9_\\.-]+)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
    {
        return QString();
    }
    return m.captured(1).trimmed();
}

void putArrayValue(QHash<QString, QVector<double>>& arr, const QString& key, int index, double value)
{
    QVector<double> data = arr.value(key);
    if (index < 0)
    {
        return;
    }
    if (data.size() <= index)
    {
        data.resize(index + 1);
    }
    data[index] = value;
    arr.insert(key, data);
}

bool readAscValuesRecursive(const QString& ascPath, ParsedAscValues& out, QSet<QString>& visited, QString* error)
{
    const QFileInfo ascInfo(ascPath);
    const QString canonical = ascInfo.canonicalFilePath().isEmpty() ? ascInfo.absoluteFilePath() : ascInfo.canonicalFilePath();
    if (visited.contains(canonical))
    {
        return true;
    }
    visited.insert(canonical);

    QFile f(canonical);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (error)
        {
            *error = QStringLiteral("Failed to open ASC file: %1").arg(canonical);
        }
        return false;
    }

    QTextStream in(&f);
    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("###"))
        {
            continue;
        }
        const QString dep = parseAscInclude(line);
        if (!dep.isEmpty())
        {
            QString depPath = QFileInfo(ascInfo.dir().absoluteFilePath(dep)).absoluteFilePath();
            if (!QFileInfo::exists(depPath))
            {
                depPath = QFileInfo(ascInfo.dir().absoluteFilePath(dep + ".asc")).absoluteFilePath();
            }
            if (QFileInfo::exists(depPath))
            {
                if (!readAscValuesRecursive(depPath, out, visited, error))
                {
                    return false;
                }
            }
            continue;
        }
        QString key;
        int index = -1;
        double value = 0.0;
        if (!parseAscAssignment(line, key, index, value))
        {
            continue;
        }
        if (index >= 0)
        {
            putArrayValue(out.array, key, index, value);
        }
        else
        {
            out.scalar.insert(key, value);
        }
    }
    return true;
}

bool readAscValues(const QString& ascPath, ParsedAscValues& out, QString* error)
{
    QSet<QString> visited;
    return readAscValuesRecursive(ascPath, out, visited, error);
}

bool getAxisHardware(
    const ParsedAscValues& asc,
    const QString& tauKey,
    const QString& aKey,
    const QString& stimLimitKey,
    const QString& stimThreshKey,
    const QStringList& gScaleScalarKeys,
    PnsCalculator::AxisHardware& outAxis,
    bool* gScaleFoundOut,
    QString* error)
{
    auto findArray = [&](const QString& key, QVector<double>& out) -> bool {
        const QString keyNorm = normalizeAscKey(key);
        if (asc.array.contains(key))
        {
            out = asc.array.value(key);
            return true;
        }
        for (auto it = asc.array.constBegin(); it != asc.array.constEnd(); ++it)
        {
            if (normalizeAscKey(it.key()) == keyNorm)
            {
                out = it.value();
                return true;
            }
        }
        const QString suffix = QStringLiteral(".") + key;
        for (auto it = asc.array.constBegin(); it != asc.array.constEnd(); ++it)
        {
            const QString itNorm = normalizeAscKey(it.key());
            if (itNorm.endsWith(QStringLiteral(".") + keyNorm) || it.key().endsWith(suffix))
            {
                out = it.value();
                return true;
            }
        }
        return false;
    };
    auto findScalar = [&](const QString& key, double& out) -> bool {
        const QString keyNorm = normalizeAscKey(key);
        if (asc.scalar.contains(key))
        {
            out = asc.scalar.value(key);
            return true;
        }
        for (auto it = asc.scalar.constBegin(); it != asc.scalar.constEnd(); ++it)
        {
            if (normalizeAscKey(it.key()) == keyNorm)
            {
                out = it.value();
                return true;
            }
        }
        const QString suffix = QStringLiteral(".") + key;
        for (auto it = asc.scalar.constBegin(); it != asc.scalar.constEnd(); ++it)
        {
            const QString itNorm = normalizeAscKey(it.key());
            if (itNorm.endsWith(QStringLiteral(".") + keyNorm) || it.key().endsWith(suffix))
            {
                out = it.value();
                return true;
            }
        }
        return false;
    };

    QVector<double> tau;
    QVector<double> aa;
    if (!findArray(tauKey, tau) || !findArray(aKey, aa))
    {
        if (error)
        {
            *error = QStringLiteral("Missing ASC arrays for %1 or %2").arg(tauKey, aKey);
        }
        return false;
    }
    if (tau.size() < 3 || aa.size() < 3)
    {
        if (error)
        {
            *error = QStringLiteral("ASC arrays %1/%2 require at least 3 values").arg(tauKey, aKey);
        }
        return false;
    }
    double stimLimit = 0.0;
    double stimThresh = 0.0;
    if (!findScalar(stimLimitKey, stimLimit) || !findScalar(stimThreshKey, stimThresh))
    {
        if (error)
        {
            *error = QStringLiteral("Missing ASC scalar %1 or %2").arg(stimLimitKey, stimThreshKey);
        }
        return false;
    }

    outAxis.tau1Ms = tau[0];
    outAxis.tau2Ms = tau[1];
    outAxis.tau3Ms = tau[2];
    outAxis.a1 = aa[0];
    outAxis.a2 = aa[1];
    outAxis.a3 = aa[2];
    outAxis.stimLimit = stimLimit;
    outAxis.stimThreshold = stimThresh;
    bool gScaleFound = false;
    double gScale = 1.0;
    for (const QString& gk : gScaleScalarKeys)
    {
        if (findScalar(gk, gScale))
        {
            gScaleFound = true;
            break;
        }
    }
    outAxis.gScale = gScale;
    if (gScaleFoundOut)
    {
        *gScaleFoundOut = gScaleFound;
    }
    return true;
}

double lowpassTau(const QVector<double>& in, double tauMs, double dtMs, QVector<double>& out)
{
    out.resize(in.size());
    if (in.isEmpty())
    {
        return 0.0;
    }
    if (tauMs <= 0.0 || dtMs <= 0.0)
    {
        out = in;
        return 1.0;
    }
    const double alpha = dtMs / (tauMs + dtMs);
    out[0] = alpha * in[0];
    for (int i = 1; i < in.size(); ++i)
    {
        out[i] = alpha * in[i] + (1.0 - alpha) * out[i - 1];
    }
    return alpha;
}

QVector<double> safePnsModel(const QVector<double>& dgdt, double dtSec, const PnsCalculator::AxisHardware& hw)
{
    QVector<double> absDgdt(dgdt.size());
    for (int i = 0; i < dgdt.size(); ++i)
    {
        absDgdt[i] = std::abs(dgdt[i]);
    }
    QVector<double> lp1;
    QVector<double> lp2;
    QVector<double> lp3;
    const double dtMs = dtSec * 1000.0;
    lowpassTau(dgdt, hw.tau1Ms, dtMs, lp1);
    lowpassTau(absDgdt, hw.tau2Ms, dtMs, lp2);
    lowpassTau(dgdt, hw.tau3Ms, dtMs, lp3);

    QVector<double> stim(dgdt.size());
    const double denom = (hw.stimLimit > 0.0) ? hw.stimLimit : 1.0;
    for (int i = 0; i < dgdt.size(); ++i)
    {
        const double s1 = hw.a1 * std::abs(lp1[i]);
        const double s2 = hw.a2 * lp2[i];
        const double s3 = hw.a3 * std::abs(lp3[i]);
        // SAFE output in percent
        stim[i] = ((s1 + s2 + s3) / denom) * hw.gScale * 100.0;
    }
    return stim;
}

double trapezoidGradientValue(const GradEvent& grad, double localSec)
{
    if (localSec < 0.0)
    {
        return 0.0;
    }
    const double rampUpSec = static_cast<double>(grad.rampUpTime) * 1e-6;
    const double flatSec = static_cast<double>(grad.flatTime) * 1e-6;
    const double rampDownSec = static_cast<double>(grad.rampDownTime) * 1e-6;
    const double totalSec = rampUpSec + flatSec + rampDownSec;
    if (localSec > totalSec || totalSec <= 0.0)
    {
        return 0.0;
    }
    const double amp = static_cast<double>(grad.amplitude);
    if (localSec <= rampUpSec && rampUpSec > 0.0)
    {
        return amp * (localSec / rampUpSec);
    }
    if (localSec <= rampUpSec + flatSec)
    {
        return amp;
    }
    if (rampDownSec > 0.0)
    {
        const double t = localSec - rampUpSec - flatSec;
        if (t <= rampDownSec)
        {
            return amp * (1.0 - t / rampDownSec);
        }
    }
    return 0.0;
}

double arbitraryGradientValue(SeqBlock* blk, int channel, const GradEvent& grad, double localSec, double gradientRasterUs)
{
    if (localSec < 0.0)
    {
        return 0.0;
    }
    const int numSamples = blk->GetArbGradNumSamples(channel);
    const float* shapePtr = blk->GetArbGradShapePtr(channel);
    if (numSamples <= 0 || !shapePtr)
    {
        return 0.0;
    }
    const double rasterUs = (gradientRasterUs > 0.0 ? gradientRasterUs : 10.0);
    const double rasterSec = rasterUs * 1e-6;
    if (numSamples == 1)
    {
        return static_cast<double>(shapePtr[0]) * static_cast<double>(grad.amplitude);
    }
    const double totalSec = rasterSec * static_cast<double>(numSamples - 1);
    if (localSec > totalSec)
    {
        return 0.0;
    }
    const double pos = localSec / rasterSec;
    const int idx0 = static_cast<int>(std::floor(pos));
    if (idx0 >= numSamples - 1)
    {
        return static_cast<double>(shapePtr[numSamples - 1]) * static_cast<double>(grad.amplitude);
    }
    const double frac = pos - idx0;
    const int idx1 = idx0 + 1;
    const double v0 = static_cast<double>(shapePtr[idx0]);
    const double v1 = static_cast<double>(shapePtr[idx1]);
    return (v0 + (v1 - v0) * frac) * static_cast<double>(grad.amplitude);
}

double extTrapGradientValue(SeqBlock* blk, int channel, const GradEvent& grad, double localSec)
{
    if (localSec < 0.0)
    {
        return 0.0;
    }
    const std::vector<long>& timesUs = blk->GetExtTrapGradTimes(channel);
    const std::vector<float>& shape = blk->GetExtTrapGradShape(channel);
    if (timesUs.empty() || shape.empty() || timesUs.size() != shape.size())
    {
        return 0.0;
    }
    const double localUs = localSec * 1e6;
    if (localUs <= static_cast<double>(timesUs.front()))
    {
        return static_cast<double>(shape.front()) * static_cast<double>(grad.amplitude);
    }
    if (localUs >= static_cast<double>(timesUs.back()))
    {
        return static_cast<double>(shape.back()) * static_cast<double>(grad.amplitude);
    }
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
    {
        return static_cast<double>(shape.front()) * static_cast<double>(grad.amplitude);
    }
    const int idx0 = idx1 - 1;
    const double t0 = static_cast<double>(timesUs[idx0]) * 1e-6;
    const double t1 = static_cast<double>(timesUs[idx1]) * 1e-6;
    const double span = t1 - t0;
    if (span <= 0.0)
    {
        return static_cast<double>(shape[idx1]) * static_cast<double>(grad.amplitude);
    }
    double alpha = (localSec - t0) / span;
    alpha = std::clamp(alpha, 0.0, 1.0);
    const double v0 = static_cast<double>(shape[idx0]);
    const double v1 = static_cast<double>(shape[idx1]);
    return (v0 + (v1 - v0) * alpha) * static_cast<double>(grad.amplitude);
}

double gradientValueFromBlock(SeqBlock* blk, int channel, double timeSec, double blockStartSec, double gradientRasterUs)
{
    if (!blk || !(blk->isTrapGradient(channel) || blk->isArbitraryGradient(channel) || blk->isExtTrapGradient(channel)))
    {
        return 0.0;
    }
    const GradEvent& grad = blk->GetGradEvent(channel);
    const double eventStartSec = blockStartSec + static_cast<double>(grad.delay) * 1e-6;
    const double localSec = timeSec - eventStartSec;
    if (blk->isTrapGradient(channel))
    {
        return trapezoidGradientValue(grad, localSec);
    }
    if (blk->isArbitraryGradient(channel))
    {
        return arbitraryGradientValue(blk, channel, grad, localSec, gradientRasterUs);
    }
    if (blk->isExtTrapGradient(channel))
    {
        return extTrapGradientValue(blk, channel, grad, localSec);
    }
    return 0.0;
}

void applyRotationIfNeeded(SeqBlock* blk, double& gx, double& gy, double& gz)
{
    if (!blk || !blk->isRotation())
    {
        return;
    }
    const RotationEvent& rot = blk->GetRotationEvent();
    const double w = rot.rotQuaternion[0];
    const double x = rot.rotQuaternion[1];
    const double y = rot.rotQuaternion[2];
    const double z = rot.rotQuaternion[3];

    const double r00 = 1.0 - 2.0 * y * y - 2.0 * z * z;
    const double r01 = 2.0 * x * y - 2.0 * w * z;
    const double r02 = 2.0 * x * z + 2.0 * w * y;

    const double r10 = 2.0 * x * y + 2.0 * w * z;
    const double r11 = 1.0 - 2.0 * x * x - 2.0 * z * z;
    const double r12 = 2.0 * y * z - 2.0 * w * x;

    const double r20 = 2.0 * x * z - 2.0 * w * y;
    const double r21 = 2.0 * y * z + 2.0 * w * x;
    const double r22 = 1.0 - 2.0 * x * x - 2.0 * y * y;

    const double lx = gx;
    const double ly = gy;
    const double lz = gz;
    gx = r00 * lx + r01 * ly + r02 * lz;
    gy = r10 * lx + r11 * ly + r12 * lz;
    gz = r20 * lx + r21 * ly + r22 * lz;
}
}

bool PnsCalculator::parseAscFile(const QString& ascPath, Hardware& outHardware, QString* errorMessage)
{
    outHardware = Hardware{};
    ParsedAscValues asc;
    QString err;
    if (!readAscValues(ascPath, asc, &err))
    {
        if (errorMessage)
        {
            *errorMessage = err;
        }
        return false;
    }

    AxisHardware x;
    AxisHardware y;
    AxisHardware z;
    const QStringList gScaleKeysX = {
        "asGPAParameters.sGCParameters.flGScaleFactorX",
        "flGScaleFactorX",
        "flGCGScaleFactorX",
        "GScaleFactorX"
    };
    const QStringList gScaleKeysY = {
        "asGPAParameters.sGCParameters.flGScaleFactorY",
        "flGScaleFactorY",
        "flGCGScaleFactorY",
        "GScaleFactorY"
    };
    const QStringList gScaleKeysZ = {
        "asGPAParameters.sGCParameters.flGScaleFactorZ",
        "flGScaleFactorZ",
        "flGCGScaleFactorZ",
        "GScaleFactorZ"
    };

    bool gxScaleFound = false;
    bool gyScaleFound = false;
    bool gzScaleFound = false;
    if (!getAxisHardware(asc, "flGSWDTauX", "flGSWDAX", "flGSWDStimulationLimitX",
                         "flGSWDStimulationThresholdX", gScaleKeysX, x, &gxScaleFound, &err) ||
        !getAxisHardware(asc, "flGSWDTauY", "flGSWDAY", "flGSWDStimulationLimitY",
                         "flGSWDStimulationThresholdY", gScaleKeysY, y, &gyScaleFound, &err) ||
        !getAxisHardware(asc, "flGSWDTauZ", "flGSWDAZ", "flGSWDStimulationLimitZ",
                         "flGSWDStimulationThresholdZ", gScaleKeysZ, z, &gzScaleFound, &err))
    {
        if (errorMessage)
        {
            *errorMessage = err;
        }
        return false;
    }
    if (!(gxScaleFound && gyScaleFound && gzScaleFound))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("ASC is missing g_scale factors (X/Y/Z). Select a full ASC (e.g. *_twoFilesCombined.asc).");
        }
        return false;
    }

    auto hasValidWeights = [](const AxisHardware& hw) {
        const double sum = hw.a1 + hw.a2 + hw.a3;
        return std::abs(sum - 1.0) <= 1e-2 && hw.stimLimit > 0.0;
    };
    if (!hasValidWeights(x) || !hasValidWeights(y) || !hasValidWeights(z))
    {
        if (errorMessage)
        {
            *errorMessage = QStringLiteral("ASC hardware coefficients are invalid (a1+a2+a3 or stim limit).");
        }
        return false;
    }

    outHardware.x = x;
    outHardware.y = y;
    outHardware.z = z;
    outHardware.valid = true;
    return true;
}

PnsCalculator::Result PnsCalculator::calculate(
    const std::vector<SeqBlock*>& blocks,
    const QVector<double>& blockEdges,
    double tFactor,
    double gradientRasterUs,
    double gammaHzPerT,
    const Hardware& hardware)
{
    Result result;
    if (!hardware.valid)
    {
        result.error = QStringLiteral("PNS hardware is not initialized.");
        return result;
    }
    if (blocks.empty() || blockEdges.size() < 2)
    {
        result.error = QStringLiteral("No sequence loaded.");
        return result;
    }
    if (gradientRasterUs <= 0.0 || gammaHzPerT <= 0.0 || tFactor <= 0.0)
    {
        result.error = QStringLiteral("Missing GradientRasterTime or gamma.");
        return result;
    }

    QVector<double> blockEdgesSec(blockEdges.size());
    for (int i = 0; i < blockEdges.size(); ++i)
    {
        const double us = blockEdges[i] / tFactor;
        blockEdgesSec[i] = us * 1e-6;
    }

    double tFirst = std::numeric_limits<double>::infinity();
    double tLast = -std::numeric_limits<double>::infinity();
    for (int b = 0; b < static_cast<int>(blocks.size()); ++b)
    {
        SeqBlock* blk = blocks[b];
        if (!blk)
        {
            continue;
        }
        const double blockStart = blockEdgesSec[b];
        for (int ch = 0; ch < 3; ++ch)
        {
            if (!(blk->isTrapGradient(ch) || blk->isArbitraryGradient(ch) || blk->isExtTrapGradient(ch)))
            {
                continue;
            }
            const GradEvent& grad = blk->GetGradEvent(ch);
            const double eventStart = blockStart + static_cast<double>(grad.delay) * 1e-6;
            double eventEnd = eventStart;
            if (blk->isTrapGradient(ch))
            {
                eventEnd += (static_cast<double>(grad.rampUpTime) +
                             static_cast<double>(grad.flatTime) +
                             static_cast<double>(grad.rampDownTime)) * 1e-6;
            }
            else if (blk->isArbitraryGradient(ch))
            {
                const int n = blk->GetArbGradNumSamples(ch);
                if (n > 1)
                {
                    eventEnd += static_cast<double>(n - 1) * gradientRasterUs * 1e-6;
                }
            }
            else if (blk->isExtTrapGradient(ch))
            {
                const std::vector<long>& timesUs = blk->GetExtTrapGradTimes(ch);
                if (!timesUs.empty())
                {
                    eventEnd += static_cast<double>(timesUs.back()) * 1e-6;
                }
            }
            tFirst = std::min(tFirst, eventStart);
            tLast = std::max(tLast, eventEnd);
        }
    }

    if (!std::isfinite(tFirst) || !std::isfinite(tLast) || tLast <= tFirst)
    {
        result.error = QStringLiteral("No gradient waveform available for PNS.");
        return result;
    }

    const double dtSec = gradientRasterUs * 1e-6;
    if (dtSec <= 0.0)
    {
        result.error = QStringLiteral("Invalid gradient raster time.");
        return result;
    }

    const double eps = std::numeric_limits<double>::epsilon();
    double ntMin = std::floor(tFirst / dtSec + eps);
    double ntMax = std::ceil(tLast / dtSec - eps);
    ntMin += 0.5;
    ntMax -= 0.5;
    if (ntMin < 0.5)
    {
        ntMin = 0.5;
    }
    if (ntMax < ntMin)
    {
        result.error = QStringLiteral("Unable to build regular PNS raster.");
        return result;
    }

    const int nSamples = static_cast<int>(std::floor(ntMax - ntMin + 1.0));
    if (nSamples < 2)
    {
        result.error = QStringLiteral("Too few samples for PNS computation.");
        return result;
    }

    QVector<double> tAxis(nSamples);
    for (int i = 0; i < nSamples; ++i)
    {
        tAxis[i] = (ntMin + static_cast<double>(i)) * dtSec;
    }

    QVector<double> gxTpm(nSamples, 0.0);
    QVector<double> gyTpm(nSamples, 0.0);
    QVector<double> gzTpm(nSamples, 0.0);

    int blockIdx = 0;
    for (int i = 0; i < nSamples; ++i)
    {
        const double tSec = tAxis[i];
        while (blockIdx + 1 < blockEdgesSec.size() && tSec >= blockEdgesSec[blockIdx + 1])
        {
            ++blockIdx;
        }
        if (blockIdx < 0 || blockIdx >= static_cast<int>(blocks.size()))
        {
            continue;
        }

        SeqBlock* blk = blocks[blockIdx];
        const double blockStartSec = blockEdgesSec[blockIdx];
        double gx = gradientValueFromBlock(blk, 0, tSec, blockStartSec, gradientRasterUs);
        double gy = gradientValueFromBlock(blk, 1, tSec, blockStartSec, gradientRasterUs);
        double gz = gradientValueFromBlock(blk, 2, tSec, blockStartSec, gradientRasterUs);
        applyRotationIfNeeded(blk, gx, gy, gz);

        gxTpm[i] = gx / gammaHzPerT;
        gyTpm[i] = gy / gammaHzPerT;
        gzTpm[i] = gz / gammaHzPerT;
    }

    QVector<double> dgdtX(nSamples - 1);
    QVector<double> dgdtY(nSamples - 1);
    QVector<double> dgdtZ(nSamples - 1);
    for (int i = 0; i < nSamples - 1; ++i)
    {
        dgdtX[i] = (gxTpm[i + 1] - gxTpm[i]) / dtSec;
        dgdtY[i] = (gyTpm[i + 1] - gyTpm[i]) / dtSec;
        dgdtZ[i] = (gzTpm[i + 1] - gzTpm[i]) / dtSec;
    }

    const QVector<double> stimX = safePnsModel(dgdtX, dtSec, hardware.x);
    const QVector<double> stimY = safePnsModel(dgdtY, dtSec, hardware.y);
    const QVector<double> stimZ = safePnsModel(dgdtZ, dtSec, hardware.z);

    result.timeSec.resize(stimX.size());
    result.pnsX.resize(stimX.size());
    result.pnsY.resize(stimX.size());
    result.pnsZ.resize(stimX.size());
    result.pnsNorm.resize(stimX.size());
    bool ok = true;
    for (int i = 0; i < stimX.size(); ++i)
    {
        // SAFE returns percent; convert to normalized value (threshold = 1.0).
        const double xNorm = 0.01 * stimX[i];
        const double yNorm = 0.01 * stimY[i];
        const double zNorm = 0.01 * stimZ[i];
        const double norm = std::sqrt(xNorm * xNorm + yNorm * yNorm + zNorm * zNorm);
        // Keep PNS sample time aligned to the raster center used to build dg/dt,
        // matching MATLAB calcPNS convention for plotting/comparison.
        result.timeSec[i] = tAxis[i];
        result.pnsX[i] = xNorm;
        result.pnsY[i] = yNorm;
        result.pnsZ[i] = zNorm;
        result.pnsNorm[i] = norm;
        if (norm >= 1.0)
        {
            ok = false;
        }
    }
    result.ok = ok;
    result.valid = true;
    return result;
}
