#include "ExtensionPlotter.h"

#include "PulseqLoader.h"
#include "Settings.h"
#include "ExtensionStyleMap.h"

#include "external/qcustomplot/qcustomplot.h"

#include <algorithm>

namespace
{
bool isTridLabel(const QString& name)
{
    return name.compare(QStringLiteral("TRID"), Qt::CaseInsensitive) == 0;
}
}

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

QStringList ExtensionPlotter::availableLabels(PulseqLoader* loader) const
{
    if (!loader)
        return {};
    return loader->getAvailableExtensionLabels();
}

void ExtensionPlotter::ensureGraphs()
{
    if (!m_plot || !m_targetRect)
        return;
}

void ExtensionPlotter::reset()
{
    m_graphByName.clear();
    m_cacheByName.clear();
    m_lastSeqPtr = nullptr;
    m_lastBlockCount = 0;
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
    if (!seqSp)
        return;

    void* seqPtr = static_cast<void*>(seqSp.get());
    const int blockCount = static_cast<int>(loader->getDecodedSeqBlocks().size());
    if (seqPtr == m_lastSeqPtr && blockCount == m_lastBlockCount)
        return;

    m_lastSeqPtr = seqPtr;
    m_lastBlockCount = blockCount;

    const QStringList labels = availableLabels(loader);
    for (const QString& name : labels)
    {
        if (m_graphByName.contains(name))
            continue;

        auto* g = m_plot->addGraph(m_targetRect->axis(QCPAxis::atBottom),
                                   m_targetRect->axis(QCPAxis::atLeft));
        if (!g)
            continue;

        int dummyValue = 0;
        bool isFlag = false;
        loader->getExtensionValueAfterBlock(0, name, dummyValue, isFlag);

        const ExtensionVisualStyle vs = extensionStyleForName(name);
        QPen pen(vs.color);
        pen.setWidthF(1.2);
        pen.setStyle(Qt::SolidLine);
        g->setPen(pen);
        if (isTridLabel(name))
        {
            g->setLineStyle(QCPGraph::lsStepLeft);
            g->setScatterStyle(QCPScatterStyle::ssNone);
            g->setBrush(Qt::NoBrush);
        }
        else
        {
            g->setLineStyle(QCPGraph::lsNone);
            const QCPScatterStyle::ScatterShape shape = toQcpScatter(vs.marker);
            g->setScatterStyle(QCPScatterStyle(shape, isFlag ? 3.0 : 6.0));
            g->setBrush(QBrush(vs.color));
        }
        g->setAdaptiveSampling(false);
        g->setAntialiased(false);
        g->setVisible(false);

        m_graphByName.insert(name, g);
        m_cacheByName.insert(name, SeriesCache{});
    }

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

    for (const QString& name : labels)
    {
        SeriesCache& sc = m_cacheByName[name];
        sc.valid = true;
        sc.used = loader->getUsedExtensions().contains(name.toUpper());
    }

    auto appendPoint = [&](const QString& name, double t, double newVal) {
        SeriesCache& sc = m_cacheByName[name];
        if (!sc.valid)
            return;
        if (!sc.t.isEmpty() && sc.t.last() == t)
        {
            sc.v.last() = newVal;
            return;
        }
        sc.t.push_back(t);
        sc.v.push_back(newVal);
    };

    const auto& blocks = loader->getDecodedSeqBlocks();
    const int nBlocks = std::min(static_cast<int>(blocks.size()), static_cast<int>(edges.size() - 1));
    bool tridHasAnyValue = false;
    int tridLastValue = 0;
    for (int i = 0; i < nBlocks; ++i)
    {
        SeqBlock* blk = blocks[i];
        if (!blk)
            continue;

        if (m_cacheByName.value(QStringLiteral("TRID")).used)
        {
            int tridValue = 0;
            bool tridIsFlag = false;
            if (loader->getExtensionValueAfterBlock(i, QStringLiteral("TRID"), tridValue, tridIsFlag) && !tridIsFlag)
            {
                appendPoint(QStringLiteral("TRID"), edges[i], static_cast<double>(tridValue));
                tridHasAnyValue = true;
                tridLastValue = tridValue;
            }
        }

        if (blk->isADC())
        {
            const ADCEvent& adc = blk->GetADCEvent();
            const double tStart = edges[i];
            const double tDelay = adc.delay * loader->getTFactor();
            const double dwellUs = adc.dwellTime / 1000.0;
            const double dt = dwellUs * loader->getTFactor();
            const double mid = (adc.numSamples > 0 ? (adc.numSamples - 1) * 0.5 * dt : 0.0);
            const double tAdc = tStart + tDelay + mid;

            for (const QString& name : labels)
            {
                if (isTridLabel(name))
                    continue;
                if (!m_cacheByName.value(name).used)
                    continue;

                int value = 0;
                bool isFlag = false;
                if (!loader->getExtensionValueAfterBlock(i, name, value, isFlag))
                    continue;

                appendPoint(name, tAdc, static_cast<double>(value));
            }
        }
    }

    if (tridHasAnyValue)
        appendPoint(QStringLiteral("TRID"), edges[nBlocks], static_cast<double>(tridLastValue));
}

void ExtensionPlotter::updateForViewport(PulseqLoader* loader, double visibleStart, double visibleEnd)
{
    if (!m_plot || !m_targetRect || !loader)
        return;

    rebuildCacheIfNeeded(loader);

    for (const QString& name : availableLabels(loader))
    {
        QCPGraph* g = m_graphByName.value(name, nullptr);
        const auto it = m_cacheByName.constFind(name);
        if (!g || it == m_cacheByName.constEnd() || !it.value().valid)
            continue;

        const bool enabled = Settings::getInstance().isExtensionLabelEnabled(name);
        const bool show = m_hostVisible && enabled && it.value().used;
        if (!show)
        {
            g->setVisible(false);
            continue;
        }

        QVector<double> tSlice, vSlice;
        if (isTridLabel(name))
        {
            const QVector<double>& tIn = it.value().t;
            const QVector<double>& vIn = it.value().v;
            if (!tIn.isEmpty() && tIn.size() == vIn.size() && visibleEnd > visibleStart)
            {
                auto itL = std::lower_bound(tIn.begin(), tIn.end(), visibleStart);
                auto itU = std::upper_bound(tIn.begin(), tIn.end(), visibleEnd);
                const int i0 = static_cast<int>(std::distance(tIn.begin(), itL));
                const int i1 = static_cast<int>(std::distance(tIn.begin(), itU));
                const int start = std::max(0, i0 - 1);
                const int end = std::min(static_cast<int>(tIn.size()), std::max(i1, i0 + 1) + 1);
                if (start < end)
                {
                    tSlice.reserve(end - start);
                    vSlice.reserve(end - start);
                    for (int i = start; i < end; ++i)
                    {
                        tSlice.push_back(tIn[i]);
                        vSlice.push_back(vIn[i]);
                    }
                }
            }
        }
        else
        {
            sliceStepSeries(it.value().t, it.value().v, visibleStart, visibleEnd, tSlice, vSlice);
        }
        g->setData(tSlice, vSlice);
        g->setVisible(!tSlice.isEmpty());
    }
}
