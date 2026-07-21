#include "PulseqLoadTransaction.h"

#include "PulseqLoader.h"

PulseqLoadTransaction::PulseqLoadTransaction(PulseqLoader& loader)
    : m_loader(loader)
{
}

bool PulseqLoadTransaction::load(const QString& path)
{
    m_loader.beginLoad();
    if (!prepare(path))
        return rollback();
    return commit(path);
}

bool PulseqLoadTransaction::prepare(const QString& path)
{
    m_staged = LoadedSequenceState {};
    if (!m_loader.readAndCreateVersionedLoader(path, m_staged, &m_error))
        return false;
    if (!m_loader.loadParserFile(path, m_staged, &m_error))
        return false;
    if (!m_loader.validateRequiredDefinitions(m_staged, &m_error))
        return false;
    if (!m_loader.decodeBlocks(m_staged, &m_error))
        return false;
    if (!m_loader.stageLoadedDerivedState(m_staged, &m_error))
        return false;
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

bool PulseqLoadTransaction::rollback()
{
    return m_loader.failLoad(m_error);
}
