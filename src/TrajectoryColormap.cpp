#include "TrajectoryColormap.h"

#include <algorithm>

namespace
{
    struct ColorStop
    {
        double pos;
        double r;
        double g;
        double b;
    };

    template <std::size_t N>
    QColor sampleStops(const ColorStop (&stops)[N], double x)
    {
        if (N == 0)
            return QColor(0, 0, 0);
        x = std::clamp(x, 0.0, 1.0);
        if (x <= stops[0].pos)
            return QColor::fromRgbF(stops[0].r, stops[0].g, stops[0].b);
        if (x >= stops[N - 1].pos)
            return QColor::fromRgbF(stops[N - 1].r, stops[N - 1].g, stops[N - 1].b);

        for (std::size_t i = 0; i + 1 < N; ++i)
        {
            const ColorStop& a = stops[i];
            const ColorStop& b = stops[i + 1];
            if (x >= a.pos && x <= b.pos)
            {
                double t = (x - a.pos) / (b.pos - a.pos);
                double r = a.r + (b.r - a.r) * t;
                double g = a.g + (b.g - a.g) * t;
                double bch = a.b + (b.b - a.b) * t;
                return QColor::fromRgbF(r, g, bch);
            }
        }
        return QColor::fromRgbF(stops[N - 1].r, stops[N - 1].g, stops[N - 1].b);
    }

    // Analytic Jet colormap (matches classic MATLAB/Matplotlib jet)
    QColor jetColor(double x)
    {
        x = std::clamp(x, 0.0, 1.0);
        double r = 0.0, g = 0.0, b = 0.0;
        double four_x = 4.0 * x;
        r = std::min(1.0, std::max(0.0, std::min(four_x - 1.5, -four_x + 4.5)));
        g = std::min(1.0, std::max(0.0, std::min(four_x - 0.5, -four_x + 3.5)));
        b = std::min(1.0, std::max(0.0, std::min(four_x + 0.5, -four_x + 2.5)));
        return QColor::fromRgbF(r, g, b);
    }

    // Plasma: approximated with a small set of stops taken from the canonical Plasma colormap
    // (start: dark purple, mid: magenta/orange, end: yellow)
    constexpr ColorStop kPlasmaStops[] = {
        {0.0,  0.050383, 0.029803, 0.527975}, // ~ #0d0887
        {0.5,  0.741176, 0.216471, 0.524706}, // ~ #bd3786 (approx mid)
        {1.0,  0.940000, 0.975000, 0.131000}  // ~ #f0f921
    };

    // Cividis: approximated as a blue→yellow monotonic gradient using representative endpoints
    constexpr ColorStop kCividisStops[] = {
        {0.0,  0.000000, 0.135112, 0.304751}, // from _cividis_data[0]
        {0.5,  0.545000, 0.566000, 0.328000}, // mid-tone olive (approximate)
        {1.0,  0.995000, 0.904000, 0.145000}  // bright yellow (approximate)
    };
}

QColor sampleTrajectoryColormap(Settings::TrajectoryColormap which, double x)
{
    switch (which)
    {
    case Settings::TrajectoryColormap::Jet:
        return jetColor(x);
    case Settings::TrajectoryColormap::Plasma:
        return sampleStops(kPlasmaStops, x);
    case Settings::TrajectoryColormap::Cividis:
        return sampleStops(kCividisStops, x);
    default:
        return jetColor(x);
    }
}


