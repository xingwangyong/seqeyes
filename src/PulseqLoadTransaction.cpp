#include "PulseqLoadTransaction.h"

#include "PulseqLoader.h"
#include "LogManager.h"
#include <QElapsedTimer>

namespace {
struct ScopedLoadTimer {
    QElapsedTimer timer;
    bool isSuccess = false;
    ScopedLoadTimer() { timer.start(); }
    ~ScopedLoadTimer() {
        if (isSuccess)
            LOG_INFO_CAT("Performance", QString("Load completed in %1 ms").arg(timer.elapsed()));
        else
            LOG_INFO_CAT("Performance", QString("Load failed in %1 ms").arg(timer.elapsed()));
    }
};
}

PulseqLoadTransaction::PulseqLoadTransaction(PulseqLoader& loader)
    : m_loader(loader)
{
}

LoadResult PulseqLoadTransaction::load(const QString& path)
{
    ScopedLoadTimer timer;
    m_loader.beginLoad();
    if (!prepare(path))
        return rollback();
    if (!commit(path))
        return {false, LoadedSequenceState {}, m_error};
    timer.isSuccess = true;
    return {true, LoadedSequenceState {}, LoadError {}};
}

bool PulseqLoadTransaction::prepare(const QString& path)
{
    m_staged = LoadedSequenceState {};
    
    QElapsedTimer stageTimer;
    stageTimer.start();
    
    if (!m_loader.readAndCreateVersionedLoader(path, m_staged, &m_error))
        return false;
    LOG_DEBUG_CAT("Performance", QString("readAndCreateVersionedLoader took %1 ms").arg(stageTimer.restart()));

    if (!m_loader.loadParserFile(path, m_staged, &m_error))
        return false;
    LOG_DEBUG_CAT("Performance", QString("loadParserFile took %1 ms").arg(stageTimer.restart()));

    if (!m_loader.validateRequiredDefinitions(m_staged, &m_error))
        return false;
    LOG_DEBUG_CAT("Performance", QString("validateRequiredDefinitions took %1 ms").arg(stageTimer.restart()));

    if (!m_loader.decodeBlocks(m_staged, &m_error))
        return false;
    LOG_DEBUG_CAT("Performance", QString("decodeBlocks took %1 ms").arg(stageTimer.restart()));

    if (!m_loader.stageLoadedDerivedState(m_staged, &m_error))
        return false;
    LOG_DEBUG_CAT("Performance", QString("stageLoadedDerivedState took %1 ms").arg(stageTimer.restart()));

    return true;
}

bool PulseqLoadTransaction::commit(const QString& path)
{
    m_loader.commitStagedSequence(m_staged);
    m_loader.commitStagedDerivedState();

    m_initialRange = m_loader.configureInitialViewport();
    m_loader.finishSuccessfulLoad(path, m_initialRange);
    return true;
}

LoadResult PulseqLoadTransaction::rollback()
{
    m_loader.failLoad(m_error);
    return {false, LoadedSequenceState {}, m_error};
}
