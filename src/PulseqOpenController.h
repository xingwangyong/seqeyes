#ifndef PULSEQOPENCONTROLLER_H
#define PULSEQOPENCONTROLLER_H

#include "OpenResult.h"

#include <QString>

class IPulseqLoadUi;
class PulseqLoader;

// Unified entry point for opening Pulseq sequence files.
//
// File -> Open, Recent, drag-drop, CLI, and Reopen all funnel through this
// controller so candidate, loaded, and reopen path rules live in one place.
class PulseqOpenController
{
public:
    PulseqOpenController(PulseqLoader& loader, IPulseqLoadUi& ui);

    OpenResult openPath(QString candidatePath);
    OpenResult reopen();

private:
    PulseqLoader& m_loader;
    IPulseqLoadUi& m_ui;
};

#endif
