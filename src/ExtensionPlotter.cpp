#include "ExtensionPlotter.h"

#include "PulseqLoader.h"
#include "Settings.h"
#include "ExtensionStyleMap.h"

#include "external/qcustomplot/qcustomplot.h"
#include "external/pulseq/v151/ExternalSequence.h"

#include <algorithm>
#include <limits>

using LabelsEnum = Labels;
using FlagsEnum = Flags;

static QCPScatterStyle::ScatterShape toQcpScatter(MarkerKind k)
{
    switch (k)
    {
        case MarkerKind::Circle:   return QCPScatterStyle::ssCircle;
        case MarkerKind::Plus:     return QCPScatterStyle::ssPlus;
        case MarkerKind::Asterisk: return QCPScatterStyle::ssStar;
        case MarkerKind::Point:    return QCPScatterStyle::ssDisc;
        case MarkerKind::Cross:    return QCPScatterStyle::ssCross;
        case MarkerKind::Square:   return QCPScatterStyle::ssSquare;
        case MarkerKind::Diamond:  return QCPScatterStyle::ssDiamond;
        case MarkerKind::TriUp:    return QCPScatterStyle::ssTriangle;
        case MarkerKind::TriDown:  return QCPScatterStyle::ssTriangleInverted;
        case MarkerKind::CrossSquare: return QCPScatterStyle::ssCrossSquare;
        case MarkerKind::PlusSquare:  return QCPScatterStyle::ssPlusSquare;
        case MarkerKind::CrossCircle: return QCPScatterStyle::ssCrossCircle;
        case MarkerKind::PlusCircle:  return QCPScatterStyle::ssPlusCircle;
        case MarkerKind::Peace:       return QCPScatterStyle::ssPeace;
        default:                   return QCPScatterStyle::ssDisc;
    }
}

ExtensionPlotter::ExtensionPlotter(QCustomPlot* plot, QCPAxisRect* targetRect)
{
    setTarget(plot, targetRect);
}

ExtensionPlotter::~ExtensionPlotter() = default;

void ExtensionPlotter::setTarget(QCustomPlot* plot, QCPAxisRect* targetRect)
{
    m_plot = plot;
    m_targetRect = targetRect;
    // Invalidate and rebuild lazily
    m_lastSeqPtr = nullptr;
    m_lastBlockCount = 0;
}

void ExtensionPlotter::setHostVisible(bool visible)
{
    m_hostVisible = visible;
    for (auto it = m_graphByName.begin(); it != m_graphByName.end(); ++it)
    {
        QCPGraph* g = it.value();
        if (!g) continue;
        
        if (!m_hostVisible)
        {
            g->setVisible(false);
        }
        else
        {
            // Restore visibility based on settings, usage, and whether data exists in current viewport
            const QString& name = it.key();
            bool enabled = Settings::getInstance().isExtensionLabelEnabled(name);
            bool used = m_cacheByName.value(name).used;
            bool hasData = (g->data() && !g->data()->isEmpty());
            
            g->setVisible(enabled && used && hasData);
        }
    }
}

QVector<ExtensionPlotter::Spec> ExtensionPlotter::supportedSpecs()
{
    // Map Settings label strings to pulseq v151 enums; unsupported strings are skipped.
    QVector<Spec> out;

    auto addCounter = [&](const QString& name, LabelsEnum id) {
        Spec s;
        s.name = name;
        s.isFlag = false;
        s.id = static_cast<int>(id);
        out.push_back(s);
    };
    auto addFlag = [&](const QString& name, FlagsEnum id) {
        Spec s;
        s.name = name;
        s.isFlag = true;
        s.id = static_cast<int>(id);
        out.push_back(s);
    };

    // Counters (NUM_LABELS)
    addCounter("SLC", LabelsEnum::SLC);
    addCounter("SEG", LabelsEnum::SEG);
    addCounter("ECO", LabelsEnum::ECO);
    addCounter("PHS", LabelsEnum::PHS);
    addCounter("SET", LabelsEnum::SET);
    addCounter("ACQ", LabelsEnum::ACQ);
    addCounter("LIN", LabelsEnum::LIN);
    addCounter("PAR", LabelsEnum::PAR);
    addCounter("AVG", LabelsEnum::AVG);
    addCounter("REP", LabelsEnum::REP);
    addCounter("ONCE", LabelsEnum::ONCE);

    // Flags (NUM_FLAGS)
    addFlag("NAV", FlagsEnum::NAV);
    addFlag("REV", FlagsEnum::REV);
    addFlag("SMS", FlagsEnum::SMS);
    addFlag("REF", FlagsEnum::REF);
    addFlag("IMA", FlagsEnum::IMA);
    addFlag("OFF", FlagsEnum::OFF);
    addFlag("NOISE", FlagsEnum::NOISE);
    addFlag("PMC", FlagsEnum::PMC);
    addFlag("NOPOS", FlagsEnum::NOPOS);
    addFlag("NOROT", FlagsEnum::NOROT);
    addFlag("NOSCL", FlagsEnum::NOSCL);

    return out;
}

void ExtensionPlotter::ensureGraphs()
{
    if (!m_plot || !m_targetRect)
        return;

    const auto specs = supportedSpecs();
    for (const Spec& s : specs)
    {
        if (m_graphByName.contains(s.name))
            continue;

        auto* g = m_plot->addGraph(m_targetRect->axis(QCPAxis::atBottom),
                                   m_targetRect->axis(QCPAxis::atLeft));
        if (!g)
            continue;

        const ExtensionVisualStyle vs = extensionStyleForName(s.name);
        QPen pen(vs.color);
        pen.setWidthF(1.2);
        pen.setStyle(Qt::SolidLine);
        g->setPen(pen);
        // Match SeqPlot.m semantics: plot label values only at ADC events (points), not as a continuous step line across delay blocks.
        g->setLineStyle(QCPGraph::lsNone);
        const QCPScatterStyle::ScatterShape shape = toQcpScatter(vs.marker);
        const double size = (s.isFlag ? 3.0 : 6.0);
        g->setScatterStyle(QCPScatterStyle(shape, size));
        g->setBrush(QBrush(vs.color));
        g->setAdaptiveSampling(false);
        g->setAntialiased(false);
        g->setVisible(false);

        m_graphByName.insert(s.name, g);
        m_cacheByName.insert(s.name, SeriesCache{});
    }
}

void ExtensionPlotter::reset()
{
    m_graphByName.clear();
    m_cacheByName.clear();
    m_lastSeqPtr = nullptr;
    m_lastBlockCount = 0;
    
    ensureGraphs();
}

void ExtensionPlotter::sliceStepSeries(const QVector<double>& tIn,
                                       const QVector<double>& vIn,
                                       double x0, double x1,
                                       QVector<double>& tOut,
                                       QVector<double>& vOut)
{
    tOut.clear();
    vOut.clear();
    if (tIn.isEmpty() || vIn.isEmpty() || tIn.size() != vIn.size())
        return;
    if (!(x1 > x0))
        return;

    // Slice points in [x0, x1]
    auto itL = std::lower_bound(tIn.begin(), tIn.end(), x0);
    auto itU = std::upper_bound(tIn.begin(), tIn.end(), x1);
    int i0 = static_cast<int>(std::distance(tIn.begin(), itL));
    int i1 = static_cast<int>(std::distance(tIn.begin(), itU));
    if (i0 >= i1)
        return;
    tOut.reserve(i1 - i0);
    vOut.reserve(i1 - i0);
    for (int i = i0; i < i1; ++i)
    {
        tOut.push_back(tIn[i]);
        vOut.push_back(vIn[i]);
    }
}

void ExtensionPlotter::rebuildCacheIfNeeded(PulseqLoader* loader)
{
    if (!loader)
        return;
    auto seqSp = loader->getSequence();
    ExternalSequence* seq = seqSp.get();
    if (!seq)
        return;

    void* seqPtr = static_cast<void*>(seq);
    const int blockCount = static_cast<int>(loader->getDecodedSeqBlocks().size());
    if (seqPtr == m_lastSeqPtr && blockCount == m_lastBlockCount)
        return;

    m_lastSeqPtr = seqPtr;
    m_lastBlockCount = blockCount;

    // Clear caches
    for (auto it = m_cacheByName.begin(); it != m_cacheByName.end(); ++it)
    {
        it.value().t.clear();
        it.value().v.clear();
        it.value().valid = false;
        it.value().used = false;
    }

    const QVector<double>& edges = loader->getBlockEdges();
    if (edges.size() < 2)
        return;

    // Track current label state (known IDs only; ignore unknown IDs >= NUM_LABELS/NUM_FLAGS).
    QVector<int> counterVal(NUM_LABELS, 0);
    QVector<bool> flagVal(NUM_FLAGS, false);

    auto specs = supportedSpecs();
    // Initialize series with a starting point at time 0
    for (const Spec& s : specs)
    {
        SeriesCache& sc = m_cacheByName[s.name];
        sc.valid = true;
        sc.used = false;
    }

    auto appendPoint = [&](const Spec& s, double t, double newVal) {
        SeriesCache& sc = m_cacheByName[s.name];
        if (!sc.valid)
            return;
        // Avoid duplicate timestamps
        if (!sc.t.isEmpty() && sc.t.last() == t)
        {
            sc.v.last() = newVal;
            return;
        }
        sc.t.push_back(t);
        sc.v.push_back(newVal);
    };

    // Track whether each label ever appeared; only plot after it appeared at least once (SeqPlot.m's label_defined semantics).
    QVector<bool> usedCounters(NUM_LABELS, false);
    QVector<bool> usedFlags(NUM_FLAGS, false);

    // Walk blocks, apply label events, and record values ONLY at ADC events (SeqPlot.m behavior).
    const auto& blocks = loader->getDecodedSeqBlocks();
    const int nBlocks = std::min(static_cast<int>(blocks.size()), static_cast<int>(edges.size() - 1));
    for (int i = 0; i < nBlocks; ++i)
    {
        SeqBlock* blk = blocks[i];
        if (!blk)
            continue;

        // Apply labelset then labelinc (SeqPlot.m behavior)
        if (blk->isLabel())
        {
            const auto& sets = blk->GetLabelSetEvents();
            for (const auto& e : sets)
            {
                const int lblId = e.numVal.first;
                const int val = e.numVal.second;
                const int flagId = e.flagVal.first;
                const bool fval = e.flagVal.second;

                if (lblId >= 0 && lblId < NUM_LABELS && lblId != LABEL_UNKNOWN)
                {
                    counterVal[lblId] = val;
                    usedCounters[lblId] = true;
                }
                if (flagId >= 0 && flagId < NUM_FLAGS && flagId != FLAG_UNKNOWN)
                {
                    flagVal[flagId] = fval;
                    usedFlags[flagId] = true;
                }
            }
            const auto& incs = blk->GetLabelIncEvents();
            for (const auto& e : incs)
            {
                const int lblId = e.numVal.first;
                const int val = e.numVal.second;
                if (lblId >= 0 && lblId < NUM_LABELS && lblId != LABEL_UNKNOWN)
                {
                    counterVal[lblId] += val;
                    usedCounters[lblId] = true;
                }
            }
        }

        if (blk->isADC())
        {
            // ADC center time: blockStart + adc.delay + (numSamples-1)/2*dwell
            const ADCEvent& adc = blk->GetADCEvent();
            const double tStart = edges[i];
            const double tDelay = adc.delay * loader->getTFactor(); // us -> internal
            const double dwellUs = adc.dwellTime / 1000.0;          // ns -> us
            const double dt = dwellUs * loader->getTFactor();
            const double mid = (adc.numSamples > 0 ? (adc.numSamples - 1) * 0.5 * dt : 0.0);
            const double tAdc = tStart + tDelay + mid;

            for (const Spec& s : specs)
            {
                // Only plot labels/flags that have appeared at least once.
                if (s.isFlag)
                {
                    if (s.id >= 0 && s.id < usedFlags.size() && !usedFlags[s.id])
                        continue;
                    m_cacheByName[s.name].used = true;
                }
                else
                {
                    if (s.id >= 0 && s.id < usedCounters.size() && !usedCounters[s.id])
                        continue;
                    m_cacheByName[s.name].used = true;
                }

                double v = 0.0;
                if (s.isFlag)
                {
                    if (s.id >= 0 && s.id < NUM_FLAGS)
                        v = flagVal[s.id] ? 1.0 : 0.0;
                }
                else
                {
                    if (s.id >= 0 && s.id < NUM_LABELS)
                        v = static_cast<double>(counterVal[s.id]);
                }
                appendPoint(s, tAdc, v);
            }
        }
    }
}

void ExtensionPlotter::updateForViewport(PulseqLoader* loader, double visibleStart, double visibleEnd)
{
    if (!m_plot || !m_targetRect || !loader)
        return;

    ensureGraphs();
    rebuildCacheIfNeeded(loader);

    const auto specs = supportedSpecs();

    for (const Spec& s : specs)
    {
        QCPGraph* g = m_graphByName.value(s.name, nullptr);
        const auto it = m_cacheByName.constFind(s.name);
        if (!g || it == m_cacheByName.constEnd() || !it.value().valid)
            continue;

        const bool enabled = Settings::getInstance().isExtensionLabelEnabled(s.name);
        const bool show = m_hostVisible && enabled && it.value().used;
        if (!show)
        {
            g->setVisible(false);
            continue;
        }

        QVector<double> tSlice, vSlice;
        sliceStepSeries(it.value().t, it.value().v, visibleStart, visibleEnd, tSlice, vSlice);
        g->setData(tSlice, vSlice);
        g->setVisible(!tSlice.isEmpty());
    }
}

