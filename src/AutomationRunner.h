#ifndef AUTOMATIONRUNNER_H
#define AUTOMATIONRUNNER_H

#include <QString>

class MainWindow;

// AutomationRunner executes a JSON-defined scenario against an existing MainWindow instance.
// Minimal schema:
// {
//   "actions": [
//     { "type": "open_file", "path": "C:/path/to/file.seq" },
//     { "type": "configure_pns", "asc_path": "C:/path/to/profile.asc", "show_pns": true, "show_x": true, "show_y": true, "show_z": true, "show_norm": true },
//     { "type": "reset_view" },
//     { "type": "measure_zoom_by_factor", "factor": 0.5 }
//   ]
// }
// Output: prints metrics such as "ZOOM_MS: <number>" to stdout.
class AutomationRunner
{
public:
    // Returns 0 on success, non-zero on failure.
    static int run(MainWindow& window, const QString& scenarioJsonPath);

private:
    static int runAction(MainWindow& window, const QString& type, const QVariantMap& params);
};

#endif // AUTOMATIONRUNNER_H


