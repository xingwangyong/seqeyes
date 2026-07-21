#ifndef PULSEQLOADTRANSACTION_H
#define PULSEQLOADTRANSACTION_H

#include "PulseqLoader.h"

#include <QPair>
#include <QString>

class PulseqLoadTransaction
{
public:
    explicit PulseqLoadTransaction(PulseqLoader& loader);

    bool load(const QString& path);

private:
    bool prepare(const QString& path);
    bool commit(const QString& path);
    bool rollback();

    PulseqLoader& m_loader;
    PulseqLoader::LoadError m_error;
    QPair<double, double> m_initialRange;
};

#endif
