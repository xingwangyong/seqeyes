#ifndef PULSEQOPENCONTROLLER_H
#define PULSEQOPENCONTROLLER_H

#include <QString>

class PulseqLoader;
class PulseqLoadUiAdapter;

class PulseqOpenController
{
public:
    PulseqOpenController(PulseqLoader& loader, PulseqLoadUiAdapter& ui);

    bool openPath(QString candidatePath);
    bool reopen();

private:
    PulseqLoader& m_loader;
    PulseqLoadUiAdapter& m_ui;
};

#endif
