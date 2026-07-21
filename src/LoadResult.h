#ifndef LOADRESULT_H
#define LOADRESULT_H

#include <QString>
#include <QVector>
#include <memory>
#include <vector>

#include "ExternalSequence.h"

// Standalone bundle type for the future staged load state. The transaction can
// own decoded blocks independently of the live loader once staging is complete.
struct LoadBlockBundle
{
    std::vector<SeqBlock*> blocks;

    ~LoadBlockBundle()
    {
        for (SeqBlock* block : blocks)
            delete block;
    }

    LoadBlockBundle() = default;
    LoadBlockBundle(const LoadBlockBundle&) = delete;
    LoadBlockBundle& operator=(const LoadBlockBundle&) = delete;

    LoadBlockBundle(LoadBlockBundle&& other) noexcept
        : blocks(std::move(other.blocks))
    {
    }

    LoadBlockBundle& operator=(LoadBlockBundle&& other) noexcept
    {
        if (this != &other)
        {
            for (SeqBlock* block : blocks)
                delete block;
            blocks = std::move(other.blocks);
        }
        return *this;
    }
};

// Products of a successful sequence load before they are committed to the
// live PulseqLoader state. This is reserved for the next staging step.
struct LoadedSequenceState
{
    std::shared_ptr<ExternalSequence> sequence;

    // Non-owning view into blockBundle->blocks. Any code that stores or copies
    // decodedBlocks must also keep blockBundle alive for at least as long.
    std::vector<SeqBlock*> decodedBlocks;

    // Owns the SeqBlock* entries exposed through decodedBlocks.
    std::shared_ptr<LoadBlockBundle> blockBundle;

    QVector<double> blockEdges;
    double totalDuration_us = 0.0;
    QString versionString;
    double b0Tesla = 0.0;
    QString systemName;
    QString b0Warning;
    int versionMajor = 0;
    int versionMinor = 0;
};

struct LoadError
{
    QString title;
    QString message;
};

struct LoadResult
{
    bool ok = false;
    LoadedSequenceState state;
    LoadError error;
};

#endif
