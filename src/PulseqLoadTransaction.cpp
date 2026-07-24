#include "PulseqLoadTransaction.h"

#include "PulseqLoader.h"
#include "LogManager.h"
#include "mainwindow.h"
#include <QElapsedTimer>
#include <QUuid>

namespace {
struct ScopedLoadTimer {
    QElapsedTimer timer;
    bool isSuccess = false;
    ScopedLoadTimer() { timer.start(); }
    ~ScopedLoadTimer() {
        // Output handled by PerfLoadSummary
    }
};
}

PulseqLoadTransaction::PulseqLoadTransaction(PulseqLoader& loader)
    : m_loader(loader)
{
}

LoadResult PulseqLoadTransaction::load(const QString& path)
{
    qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    m_loadTraceId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    ScopedLoadTimer timer;
    m_loader.beginLoad();
    if (!prepare(path))
        return rollback();
        
    if (m_loader.getMainWindow()) {
        m_loader.getMainWindow()->beginInitialLoadPerf(m_loadTraceId, m_parseMs, m_decodeMs, startMs);
    }
    
    if (!commit(path))
        return {false, LoadedSequenceState {}, m_error, m_loadTraceId};
        
    timer.isSuccess = true;
    m_totalMs = timer.timer.elapsed();
    
    return {true, LoadedSequenceState {}, LoadError {}, m_loadTraceId};
}

bool PulseqLoadTransaction::prepare(const QString& path)
{
    m_staged = LoadedSequenceState {};
    
    QElapsedTimer stageTimer;
    stageTimer.start();
    
    bool ok = m_loader.readAndCreateVersionedLoader(path, m_staged, &m_error);
    m_parseMs += stageTimer.restart();
    if (!ok)
        return false;

    ok = m_loader.loadParserFile(path, m_staged, &m_error);
    m_parseMs += stageTimer.restart();
    if (!ok)
        return false;

    ok = m_loader.validateRequiredDefinitions(m_staged, &m_error);
    m_parseMs += stageTimer.restart();
    if (!ok)
        return false;

    ok = m_loader.decodeBlocks(m_staged, &m_error);
    m_decodeMs += stageTimer.restart();
    if (!ok)
        return false;

    ok = m_loader.stageLoadedDerivedState(m_staged, &m_error);
    if (Settings::getInstance().getPerformanceDebugEnabled()) {
        LOG_DEBUG_CAT("Performance", QString("stageLoadedDerivedState took %1 ms").arg(stageTimer.restart()));
    }
    if (!ok)
        return false;

    return true;
}

bool PulseqLoadTransaction::commit(const QString& path)
{
    m_loader.commitStagedSequence(m_staged);
    m_loader.commitStagedDerivedState();

    m_initialRange = m_loader.configureInitialViewport();
    m_loader.finishSuccessfulLoad(path, m_initialRange, m_loadTraceId);
    return true;
}

LoadResult PulseqLoadTransaction::rollback()
{
    m_loader.failLoad(m_error);
    return {false, LoadedSequenceState {}, m_error, m_loadTraceId};
}
