#ifndef PNSCALCULATOR_H
#define PNSCALCULATOR_H

#include <QVector>
#include <QString>
#include <vector>

class SeqBlock;

class PnsCalculator
{
public:
    struct AxisHardware
    {
        double tau1Ms {0.0};
        double tau2Ms {0.0};
        double tau3Ms {0.0};
        double a1 {0.0};
        double a2 {0.0};
        double a3 {0.0};
        double stimLimit {0.0};
        double stimThreshold {0.0};
        double gScale {1.0};
    };

    struct Hardware
    {
        AxisHardware x;
        AxisHardware y;
        AxisHardware z;
        bool valid {false};
    };

    struct Result
    {
        bool valid {false};
        bool ok {false};
        QString error;
        QVector<double> timeSec;
        QVector<double> pnsX;
        QVector<double> pnsY;
        QVector<double> pnsZ;
        QVector<double> pnsNorm;
    };

    static bool parseAscFile(const QString& ascPath, Hardware& outHardware, QString* errorMessage = nullptr);

    static Result calculate(
        const std::vector<SeqBlock*>& blocks,
        const QVector<double>& blockEdges,
        double tFactor,
        double gradientRasterUs,
        double gammaHzPerT,
        const Hardware& hardware);
};

#endif // PNSCALCULATOR_H
