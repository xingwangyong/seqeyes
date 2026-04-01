#include "SeriesBuilder.h"
#include <QtGlobal>
#include <cmath>
#include <limits>

namespace SeriesBuilder {

void buildRFSeries(
    const std::vector<SeqBlock*>& blocks,
    const QVector<double>& edges,
    double tFactor,
    QVector<double>& rfTimeAmp,
    QVector<double>& rfAmp,
    QVector<double>& rfTimePh,
    QVector<double>& rfPh)
{
    rfTimeAmp.clear(); rfAmp.clear();
    rfTimePh.clear();  rfPh.clear();

    const int numBlocks = static_cast<int>(blocks.size());
    if (numBlocks == 0 || edges.isEmpty()) return;

    bool hasAnyPointAmp = false;
    double lastTimeAmp = 0.0;
    double lastValAmp  = 0.0;
    
    bool hasAnyPointPh = false;
    double lastTimePh = 0.0;
    double lastValPh  = 0.0;

    // Tolerances for endpoint equality
    const double epsT = 1e-12;   // time equality tolerance (internal units)
    const double epsV = 1e-12;   // value equality tolerance

    for (int i = 0; i < numBlocks; ++i) {
        SeqBlock* blk = blocks[i];
        if (!blk || !blk->isRF()) continue;

        RFEvent& rf = blk->GetRFEvent();
        int RFLength = blk->GetRFLength();
        if (RFLength <= 0) continue;

        float dwell = blk->GetRFDwellTime();
        float* rfList = blk->GetRFAmplitudePtr();
        float* phaseList = blk->GetRFPhasePtr();
        const double tStart = edges[i] + rf.delay * tFactor;

        // Decide whether to insert a break based on exact endpoint equality rule
        // Only if (lastTime,lastVal) == (firstTime,firstVal) we avoid a break; otherwise insert NaN at firstTime.
        if (RFLength > 0) {
            const double firstTime = tStart;
            const double firstAmp = static_cast<double>(rfList[0]) * static_cast<double>(rf.amplitude);
            const double firstPh = static_cast<double>(phaseList[0]);

            if (hasAnyPointAmp) {
                const bool sameAmp = (std::abs(lastTimeAmp - firstTime) <= epsT) &&
                                     (std::abs(lastValAmp  - firstAmp)  <= epsV);
                if (!sameAmp) {
                    // Insert NaN at discontinuities.
                    rfTimeAmp.append(lastTimeAmp);
                    rfAmp.append(std::numeric_limits<double>::quiet_NaN());
                }
            }
            
            if (hasAnyPointPh) {
                const bool samePh = (std::abs(lastTimePh - firstTime) <= epsT) &&
                                    (std::abs(lastValPh  - firstPh)  <= epsV);
                if (!samePh) {
                    // Insert NaN at discontinuities.
                    rfTimePh.append(lastTimePh);
                    rfPh.append(std::numeric_limits<double>::quiet_NaN());
                }
            }
        }

        // Append samples for this block; preserve true continuity across blocks
        rfTimeAmp.reserve(rfTimeAmp.size() + RFLength);
        rfAmp.reserve(rfAmp.size() + RFLength);
        rfTimePh.reserve(rfTimePh.size() + RFLength);
        rfPh.reserve(rfPh.size() + RFLength);

        for (int j = 0; j < RFLength; ++j) {
            const double t = tStart + j * dwell * tFactor;
            // Use magnitude-only RF envelope for amplitude series; do NOT fabricate zeros between blocks
            const double amp = static_cast<double>(rfList[j]) * static_cast<double>(rf.amplitude);
            // Use phase data for phase series
            const double phase = static_cast<double>(phaseList[j]);

            rfTimeAmp.append(t);
            rfAmp.append(amp);
            rfTimePh.append(t);
            rfPh.append(phase);

            lastTimeAmp = t; lastValAmp = amp;
            lastTimePh = t; lastValPh = phase;
            hasAnyPointAmp = true;
            hasAnyPointPh = true;
        }
    }
}

void buildGradientSeries(
    const std::vector<SeqBlock*>& blocks,
    const QVector<double>& edges,
    double tFactor,
    int channel, // 0=GX, 1=GY, 2=GZ
    QVector<double>& gradTime,
    QVector<double>& gradValues,
    double gradientRasterUs)
{
    gradTime.clear(); gradValues.clear();
    
    const int numBlocks = static_cast<int>(blocks.size());
    if (numBlocks == 0 || edges.isEmpty()) return;
    
    bool hasAnyPoint = false;
    double lastTime = 0.0;
    double lastVal = 0.0;
    
    // Tolerances for endpoint equality
    const double epsT = 1e-12;
    const double epsV = 1e-12;
    
    for (int i = 0; i < numBlocks; ++i) {
        SeqBlock* blk = const_cast<SeqBlock*>(blocks[i]);
        if (!blk) continue;
        
        // Check if this block has gradient data for the specified channel
        bool hasGradient = false;
        if (blk->isTrapGradient(channel) || blk->isArbitraryGradient(channel) || blk->isExtTrapGradient(channel)) {
            hasGradient = true;
        }
        
        if (!hasGradient) continue;
        
        const GradEvent& grad = blk->GetGradEvent(channel);
        const double tStart = edges[i] + grad.delay * tFactor;
        
        // Process different gradient types
        QVector<double> blockTime, blockValues;
        
        if (blk->isTrapGradient(channel)) {
            // Trapezoid gradient: build time/amplitude arrays
            double rampUpTime = grad.rampUpTime * tFactor;
            double flatTime = grad.flatTime * tFactor;
            double rampDownTime = grad.rampDownTime * tFactor;
            
            // Build trapezoid points
            QVector<double> times = {0, rampUpTime, rampUpTime + flatTime, rampUpTime + flatTime + rampDownTime};
            QVector<double> amps = {0, grad.amplitude, grad.amplitude, 0};
            
            for (int j = 0; j < times.size(); ++j) {
                blockTime.append(tStart + times[j]);
                blockValues.append(amps[j]);
            }
        }
        else if (blk->isArbitraryGradient(channel)) {
            // Arbitrary gradient: align endpoint semantics with MATLAB-style
            // first/last handling and oversampling timing.
            int numSamples = blk->GetArbGradNumSamples(channel);
            const float* shapePtr = blk->GetArbGradShapePtr(channel);
            
            if (numSamples > 0 && shapePtr) {
                double gradRaster_us = (gradientRasterUs > 0.0 ? gradientRasterUs : 10.0);
                const double dt = gradRaster_us * tFactor;
                const double ampScale = static_cast<double>(grad.amplitude);

                auto deriveEdgeSample = [&](bool first) -> double {
                    if (numSamples <= 0) return 0.0;
                    if (numSamples == 1) return static_cast<double>(shapePtr[0]);
                    if (first) {
                        return 0.5 * (3.0 * static_cast<double>(shapePtr[0]) -
                                      static_cast<double>(shapePtr[1]));
                    }
                    return 0.5 * (3.0 * static_cast<double>(shapePtr[numSamples - 1]) -
                                  static_cast<double>(shapePtr[numSamples - 2]));
                };

                const bool hasFirst = (grad.first != FLOAT_UNDEFINED);
                const bool hasLast = (grad.last != FLOAT_UNDEFINED);
                double firstVal = hasFirst ? static_cast<double>(grad.first) : deriveEdgeSample(true);
                double lastVal = hasLast ? static_cast<double>(grad.last) : deriveEdgeSample(false);

                // Compatibility: some sources may store first/last already scaled by amplitude.
                if (hasFirst && std::abs(firstVal) > 1.0 + 1e-6 && std::abs(ampScale) > 0.0)
                    firstVal /= ampScale;
                if (hasLast && std::abs(lastVal) > 1.0 + 1e-6 && std::abs(ampScale) > 0.0)
                    lastVal /= ampScale;

                // Robust v1.5.x oversampling detection:
                // prefer parser helper, but also trust raw grad.timeShape==-1
                // in case metadata wiring changes in loader paths.
                const bool oversampled = blk->isArbGradWithOversampling(channel) ||
                                         (grad.timeShape == -1);

                // Start endpoint
                blockTime.append(tStart);
                blockValues.append(firstVal * ampScale);

                if (oversampled) {
                    // Oversampled arbitrary waveform: sample points on half-raster offsets.
                    for (int j = 0; j < numSamples; ++j) {
                        double t = tStart + (static_cast<double>(j) + 1.0) * 0.5 * dt;
                        double v = static_cast<double>(shapePtr[j]) * ampScale;
                        blockTime.append(t);
                        blockValues.append(v);
                    }
                    double tEnd = tStart + (static_cast<double>(numSamples) + 1.0) * 0.5 * dt;
                    blockTime.append(tEnd);
                    blockValues.append(lastVal * ampScale);
                } else {
                    // Standard arbitrary waveform: sample points at center of each raster interval.
                    for (int j = 0; j < numSamples; ++j) {
                        double t = tStart + (static_cast<double>(j) + 0.5) * dt;
                        double v = static_cast<double>(shapePtr[j]) * ampScale;
                        blockTime.append(t);
                        blockValues.append(v);
                    }
                    double tEnd = tStart + static_cast<double>(numSamples) * dt;
                    blockTime.append(tEnd);
                    blockValues.append(lastVal * ampScale);
                }
            }
        }
        else if (blk->isExtTrapGradient(channel)) {
            // Extended trapezoid gradient: use time/amplitude arrays
            const std::vector<long>& times = blk->GetExtTrapGradTimes(channel);
            const std::vector<float>& shape = blk->GetExtTrapGradShape(channel);
            
            if (!times.empty() && !shape.empty() && times.size() == shape.size()) {
                for (size_t j = 0; j < times.size(); ++j) {
                    double t = tStart + times[j] * tFactor;
                    double amp = static_cast<double>(shape[j]) * static_cast<double>(grad.amplitude);
                    blockTime.append(t);
                    blockValues.append(amp);
                }
            }
        }
        
        // Deduplicate identical timestamps within this block (keep last occurrence)
        if (!blockTime.isEmpty()) {
            int n = blockTime.size();
            int w = 0;
            double prevT = std::numeric_limits<double>::quiet_NaN();
            for (int j = 0; j < n; ++j) {
                double tj = blockTime[j];
                double vj = blockValues[j];
                if (w > 0 && std::abs(tj - prevT) <= epsT) {
                    // Overwrite previous value at the same timestamp
                    blockTime[w - 1] = tj;
                    blockValues[w - 1] = vj;
                    continue;
                }
                blockTime[w] = tj;
                blockValues[w] = vj;
                prevT = tj;
                ++w;
            }
            blockTime.resize(w);
            blockValues.resize(w);
        }

        if (blockTime.isEmpty()) continue;

        // Avoid boundary duplicate timestamp with previous block (drop first if equal-in-time)
        if (hasAnyPoint && std::abs(blockTime.first() - lastTime) <= epsT) {
            if (blockTime.size() > 1) {
                blockTime.remove(0);
                blockValues.remove(0);
            } else {
                // Single-point block coinciding with boundary; nothing meaningful to add
                continue;
            }
        }
        
        // Check for continuity with previous block
        if (hasAnyPoint && !blockTime.isEmpty()) {
            const double firstTime = blockTime.first();
            const double firstVal = blockValues.first();
            
            const bool samePoint = (std::abs(lastTime - firstTime) <= epsT) &&
                                  (std::abs(lastVal - firstVal) <= epsV);
            if (!samePoint) {
                // Insert NaN to break the line
                gradTime.append(lastTime);
                gradValues.append(std::numeric_limits<double>::quiet_NaN());
            }
        }
        
        // Append this block's data
        gradTime.reserve(gradTime.size() + blockTime.size());
        gradValues.reserve(gradValues.size() + blockValues.size());
        
        for (int j = 0; j < blockTime.size(); ++j) {
            gradTime.append(blockTime[j]);
            gradValues.append(blockValues[j]);
            
            lastTime = blockTime[j];
            lastVal = blockValues[j];
            hasAnyPoint = true;
        }
    }
}

void buildADCSeries(
    const std::vector<SeqBlock*>& blocks,
    const QVector<double>& edges,
    double tFactor,
    QVector<double>& adcTime,
    QVector<double>& adcValues)
{
    adcTime.clear(); adcValues.clear();
    
    const int numBlocks = static_cast<int>(blocks.size());
    if (numBlocks == 0 || edges.isEmpty()) return;
    
    for (int i = 0; i < numBlocks; ++i) {
        SeqBlock* blk = const_cast<SeqBlock*>(blocks[i]);
        if (!blk || !blk->isADC()) continue;
        
        const ADCEvent& adc = blk->GetADCEvent();
        if (adc.numSamples == 0) continue;
        
        const double tStart = edges[i] + adc.delay * tFactor;
        const double tEnd = tStart + (adc.numSamples * adc.dwellTime / 1000.0) * tFactor;
        
        // ADC events are represented as rectangular pulses
        // Create simple rectangle: bottom -> top -> top -> bottom
        // This creates a clean rectangle without connecting lines
        
        // Start with NaN to ensure separation from previous ADC event
        if (!adcTime.isEmpty()) {
            adcTime.append(tStart);
            adcValues.append(std::numeric_limits<double>::quiet_NaN());
        }
        
        // Create rectangle: bottom -> top -> top -> bottom
        adcTime.append(tStart); adcValues.append(0.0);  // Bottom left
        adcTime.append(tStart); adcValues.append(1.0);  // Top left  
        adcTime.append(tEnd);   adcValues.append(1.0);  // Top right
        adcTime.append(tEnd);   adcValues.append(0.0);  // Bottom right
        
        // Add NaN to close the rectangle
        adcTime.append(tEnd);
        adcValues.append(std::numeric_limits<double>::quiet_NaN());
    }
}

} // namespace SeriesBuilder


