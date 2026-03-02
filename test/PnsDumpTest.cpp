#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDir>

#include <iostream>
#include <algorithm>
#include <cmath>

#include "PnsCalculator.h"
#include "ExternalSequence.h"

namespace
{
QString argValue(const QStringList& args, const QString& key)
{
    const int idx = args.indexOf(key);
    if (idx >= 0 && idx + 1 < args.size())
    {
        return args[idx + 1];
    }
    return QString();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();

    const QString seqPath = argValue(args, "--seq");
    const QString ascPath = argValue(args, "--asc");
    const QString outCsv = argValue(args, "--out");
    int stride = argValue(args, "--stride").toInt();
    if (stride <= 0)
    {
        stride = 1;
    }

    if (seqPath.isEmpty() || ascPath.isEmpty() || outCsv.isEmpty())
    {
        std::cerr << "Usage: PnsDumpTest --seq <file.seq> --asc <hardware.asc> --out <result.csv> [--stride N]" << std::endl;
        return 2;
    }
    if (!QFileInfo::exists(seqPath))
    {
        std::cerr << "Sequence file not found: " << seqPath.toStdString() << std::endl;
        return 3;
    }
    if (!QFileInfo::exists(ascPath))
    {
        std::cerr << "ASC file not found: " << ascPath.toStdString() << std::endl;
        return 4;
    }

    ExternalSequence seq;
    if (!seq.load(seqPath.toStdString()))
    {
        std::cerr << "Failed to load sequence file: " << seqPath.toStdString() << std::endl;
        return 5;
    }

    PnsCalculator::Hardware hw;
    QString err;
    if (!PnsCalculator::parseAscFile(ascPath, hw, &err))
    {
        std::cerr << "ASC parse failed: " << err.toStdString() << std::endl;
        return 6;
    }

    const int nBlocks = seq.GetNumberOfBlocks();
    if (nBlocks <= 0)
    {
        std::cerr << "No blocks in sequence." << std::endl;
        return 7;
    }
    std::vector<SeqBlock*> blocks;
    blocks.reserve(nBlocks);
    QVector<double> blockEdges(nBlocks + 1, 0.0);
    const double tFactor = 1.0; // internal unit in this tool: microseconds
    for (int i = 0; i < nBlocks; ++i)
    {
        SeqBlock* blk = seq.GetBlock(i);
        if (!blk)
        {
            std::cerr << "Failed to decode block pointer at index " << i << std::endl;
            return 8;
        }
        if (!seq.decodeBlock(blk))
        {
            std::cerr << "Failed to decode block at index " << i << std::endl;
            return 9;
        }
        blocks.push_back(blk);
        blockEdges[i + 1] = blockEdges[i] + blk->GetDuration() * tFactor;
    }

    const std::vector<double> gradRasterDef = seq.GetDefinition("GradientRasterTime");
    if (gradRasterDef.empty() || !std::isfinite(gradRasterDef[0]) || gradRasterDef[0] <= 0.0)
    {
        std::cerr << "Missing/invalid GradientRasterTime definition." << std::endl;
        return 10;
    }
    const double gradientRasterUs = gradRasterDef[0] * 1e6;
    // Match application default proton gamma in Hz/T.
    const double gammaHzPerT = 42.576e6;

    const PnsCalculator::Result result = PnsCalculator::calculate(
        blocks, blockEdges, tFactor, gradientRasterUs, gammaHzPerT, hw);
    if (!result.valid)
    {
        std::cerr << "PNS calculate failed: " << result.error.toStdString() << std::endl;
        return 11;
    }

    const QVector<double>& t = result.timeSec;
    const QVector<double>& x = result.pnsX;
    const QVector<double>& y = result.pnsY;
    const QVector<double>& z = result.pnsZ;
    const QVector<double>& n = result.pnsNorm;
    const int sz = std::min({t.size(), x.size(), y.size(), z.size(), n.size()});
    if (sz <= 0)
    {
        std::cerr << "No PNS samples available." << std::endl;
        return 12;
    }

    QFile f(outCsv);
    const QFileInfo outInfo(f);
    const QDir outDir = outInfo.dir();
    if (!outDir.exists())
    {
        outDir.mkpath(".");
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        std::cerr << "Failed to write CSV: " << outCsv.toStdString() << std::endl;
        return 13;
    }

    QTextStream ts(&f);
    ts << "time_sec,pns_x_norm,pns_y_norm,pns_z_norm,pns_norm\n";
    for (int i = 0; i < sz; i += stride)
    {
        ts << QString::number(t[i], 'g', 17) << ","
           << QString::number(x[i], 'g', 17) << ","
           << QString::number(y[i], 'g', 17) << ","
           << QString::number(z[i], 'g', 17) << ","
           << QString::number(n[i], 'g', 17) << "\n";
    }
    if (sz > 0 && ((sz - 1) % stride) != 0)
    {
        const int i = sz - 1;
        ts << QString::number(t[i], 'g', 17) << ","
           << QString::number(x[i], 'g', 17) << ","
           << QString::number(y[i], 'g', 17) << ","
           << QString::number(z[i], 'g', 17) << ","
           << QString::number(n[i], 'g', 17) << "\n";
    }
    f.close();

    double nMax = 0.0;
    for (int i = 0; i < sz; ++i)
    {
        nMax = std::max(nMax, n[i]);
    }
    std::cout << "PNS_DUMP_OK samples=" << sz << " max_norm=" << nMax << std::endl;
    return 0;
}
