#pragma once

#include <QColor>
#include <QString>

// Shared mapping between extension plotting and legend UI.
// Keep this header small and dependency-free (no QCustomPlot include).

enum class MarkerKind
{
    Line,
    Circle,
    Plus,
    Asterisk,
    Point,
    Cross,
    Square,
    Diamond,
    TriUp,
    TriDown,
    CrossSquare,
    PlusSquare,
    CrossCircle,
    PlusCircle,
    Peace
};

struct ExtensionVisualStyle
{
    QColor color;
    MarkerKind marker {MarkerKind::Circle};
    QString markerText; // Matlab-like hint: "o", "+", "*", ".", "x", "s", "d", "^", "v", ">", "<", "p", "h"
};

inline ExtensionVisualStyle extensionStyleForName(const QString& name)
{
    // Favor high distinguishability: unique marker for common counters.
    const QString n = name.toUpper();

    // Counters
    if (n == "SLC") return {QColor(52, 152, 219), MarkerKind::Circle, "o"};
    if (n == "SEG") return {QColor(241, 196, 15), MarkerKind::Square, "s"};
    if (n == "REP") return {QColor(231, 76, 60), MarkerKind::TriUp, "^"};
    if (n == "AVG") return {QColor(155, 89, 182), MarkerKind::TriDown, "v"};
    if (n == "ECO") return {QColor(46, 204, 113), MarkerKind::Diamond, "d"};
    if (n == "PHS") return {QColor(155, 89, 182), MarkerKind::PlusSquare, "+s"};
    if (n == "LIN") return {QColor(52, 152, 219), MarkerKind::PlusCircle, "+o"};
    if (n == "PAR") return {QColor(230, 126, 34), MarkerKind::CrossCircle, "xo"};
    if (n == "SET") return {QColor(26, 188, 156), MarkerKind::CrossSquare, "xs"};
    if (n == "ACQ") return {QColor(127, 140, 141), MarkerKind::Point, "."};
    if (n == "ONCE") return {QColor(52, 73, 94), MarkerKind::Asterisk, "*"};
    if (n == "TRID") return {QColor(142, 68, 173), MarkerKind::Line, "line"};

    // Flags (keep slightly muted and point-like)
    if (n == "NAV") return {QColor(39, 174, 96), MarkerKind::Point, "."};
    if (n == "REV") return {QColor(211, 84, 0), MarkerKind::Point, "."};
    if (n == "SMS") return {QColor(142, 68, 173), MarkerKind::Point, "."};
    if (n == "REF") return {QColor(22, 160, 133), MarkerKind::Point, "."};
    if (n == "IMA") return {QColor(41, 128, 185), MarkerKind::Point, "."};
    if (n == "OFF") return {QColor(127, 140, 141), MarkerKind::Point, "."};
    if (n == "NOISE") return {QColor(149, 165, 166), MarkerKind::Point, "."};
    if (n == "PMC") return {QColor(52, 73, 94), MarkerKind::Point, "."};
    if (n == "NOROT") return {QColor(52, 73, 94), MarkerKind::Point, "."};
    if (n == "NOPOS") return {QColor(52, 73, 94), MarkerKind::Point, "."};
    if (n == "NOSCL") return {QColor(52, 73, 94), MarkerKind::Point, "."};

    // Fallback
    return {QColor(127, 140, 141), MarkerKind::Point, "."};
}
