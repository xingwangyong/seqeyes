#pragma once

#include <QColor>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>

class QCustomPlot;
class QCPAxisRect;
class QCPGraph;
class PulseqLoader;

/**
 * @brief Draw Pulseq extension labels (SLC/REP/AVG...) as step lines.
 *
 * Keep extension plotting logic out of WaveformDrawer.cpp to prevent it from growing too large.
 */
class ExtensionPlotter
{
public:
    explicit ExtensionPlotter(QCustomPlot* plot, QCPAxisRect* targetRect);
    ~ExtensionPlotter();

    ExtensionPlotter(const ExtensionPlotter&) = delete;
    ExtensionPlotter& operator=(const ExtensionPlotter&) = delete;

    // Called after a new sequence is loaded or when plot/rect changes.
    void setTarget(QCustomPlot* plot, QCPAxisRect* targetRect);

    // Host visibility: when ADC axis is hidden, hide all extension graphs too.
    void setHostVisible(bool visible);

    // Update visible series for the current viewport.
    void updateForViewport(PulseqLoader* loader, double visibleStart, double visibleEnd,
                           const QStringList& enabledUsedLabels, int pixelWidth);

    int lastVisibleMaxValue() const { return m_lastVisibleMaxValue; }

    // Clear all graphs from plot (non-owning pointers; QCustomPlot owns graphs).
    void reset();

private:
    struct SeriesCache
    {
        QVector<double> t; // points at ADC center times
        QVector<double> v; // value at those ADC times
        bool valid {false};
        bool used {false}; // whether this label/flag ever appeared in the sequence
    };

    void ensureGraphs();
    void rebuildCacheIfNeeded(PulseqLoader* loader, const QStringList& labels);
    QStringList availableLabels(PulseqLoader* loader) const;

    static void sliceStepSeries(const QVector<double>& tIn,
                                const QVector<double>& vIn,
                                double x0, double x1,
                                int pixelWidth,
                                QVector<double>& tOut,
                                QVector<double>& vOut,
                                int* maxValueOut = nullptr);

private:
    QCustomPlot* m_plot {nullptr};
    QCPAxisRect* m_targetRect {nullptr};
    bool m_hostVisible {true};
    int m_lastVisibleMaxValue {0};

    // Cache invalidation
    void* m_lastSeqPtr {nullptr};
    int m_lastBlockCount {0};

    // Graphs (one per label/flag)
    QMap<QString, QCPGraph*> m_graphByName;

    // Full-series cache (per label name)
    QHash<QString, SeriesCache> m_cacheByName;
};
