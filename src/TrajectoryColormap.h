#ifndef TRAJECTORYCOLORMAP_H
#define TRAJECTORYCOLORMAP_H

#include <QColor>
#include "Settings.h"

// Evaluate the configured trajectory colormap at normalized position x in [0,1].
QColor sampleTrajectoryColormap(Settings::TrajectoryColormap which, double x);

#endif // TRAJECTORYCOLORMAP_H


