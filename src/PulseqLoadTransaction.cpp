#include "PulseqLoadTransaction.h"

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
    if (!m_loader.readAndCreateVersionedLoader(path, &m_error))
        return false;
    if (!m_loader.loadParserFile(path, &m_error))
        return false;
    if (!m_loader.validateRequiredDefinitions(&m_error))
        return false;
    if (!m_loader.decodeBlocks(&m_error))
        return false;
    if (!m_loader.buildLoadedWaveformCaches(&m_error))
        return false;

    m_initialRange = m_loader.configureInitialViewport();
    m_loader.updateRepetitionTimeMetadata();
    return true;
}

bool PulseqLoadTransaction::commit(const QString& path)
{
    m_loader.finishSuccessfulLoad(path, m_initialRange);
    return true;
}

bool PulseqLoadTransaction::rollback()
{
    return m_loader.failLoad(m_error);
}

