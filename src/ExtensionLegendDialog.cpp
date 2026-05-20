#include "ExtensionLegendDialog.h"

#include "PulseqLoader.h"
#include "Settings.h"
#include "ExtensionStyleMap.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSet>

static QPixmap drawMarkerIcon(const ExtensionVisualStyle& s, int px = 14)
{
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(s.color);
    pen.setWidthF(2.0);
    p.setPen(pen);

    const QRectF r(2, 2, px - 4, px - 4);
    const QPointF c = r.center();

    auto drawTriangle = [&](const QVector<QPointF>& pts) {
        QPainterPath path;
        path.moveTo(pts[0]);
        path.lineTo(pts[1]);
        path.lineTo(pts[2]);
        path.closeSubpath();
        p.drawPath(path);
    };

    switch (s.marker)
    {
        case MarkerKind::Line:
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            break;
        case MarkerKind::Circle:
            p.drawEllipse(r);
            break;
        case MarkerKind::Point:
            p.setBrush(s.color);
            p.drawEllipse(QRectF(c.x() - 2, c.y() - 2, 4, 4));
            break;
        case MarkerKind::Plus:
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            break;
        case MarkerKind::Cross:
            p.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.bottom()));
            p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
            break;
        case MarkerKind::Asterisk:
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            p.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.bottom()));
            p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
            break;
        case MarkerKind::Square:
            p.drawRect(r);
            break;
        case MarkerKind::Diamond:
            drawTriangle({QPointF(c.x(), r.top()), QPointF(r.right(), c.y()), QPointF(c.x(), r.bottom())});
            drawTriangle({QPointF(c.x(), r.top()), QPointF(r.left(), c.y()), QPointF(c.x(), r.bottom())});
            break;
        case MarkerKind::TriUp:
            drawTriangle({QPointF(c.x(), r.top()), QPointF(r.right(), r.bottom()), QPointF(r.left(), r.bottom())});
            break;
        case MarkerKind::TriDown:
            drawTriangle({QPointF(r.left(), r.top()), QPointF(r.right(), r.top()), QPointF(c.x(), r.bottom())});
            break;
        case MarkerKind::CrossSquare:
            p.drawRect(r);
            p.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.bottom()));
            p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
            break;
        case MarkerKind::PlusSquare:
            p.drawRect(r);
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            break;
        case MarkerKind::CrossCircle:
            p.drawEllipse(r);
            p.drawLine(QPointF(r.left(), r.top()), QPointF(r.right(), r.bottom()));
            p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
            break;
        case MarkerKind::PlusCircle:
            p.drawEllipse(r);
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
            break;
        case MarkerKind::Peace:
            p.drawEllipse(r);
            p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
            p.drawLine(QPointF(c.x(), c.y()), QPointF(r.left(), r.bottom()));
            p.drawLine(QPointF(c.x(), c.y()), QPointF(r.right(), r.bottom()));
            break;
    }
    p.end();
    return pm;
}

ExtensionLegendDialog::ExtensionLegendDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Extension legend");
    setWindowModality(Qt::NonModal);
    resize(520, 420);
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(QStringList() << "Enabled" << "Used" << "Name" << "Marker" << "Color");
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_table);
}

void ExtensionLegendDialog::refresh(PulseqLoader* loader)
{
    const QStringList labels = Settings::getSupportedExtensionLabels();
    QSet<QString> used;
    if (loader)
        used = loader->getUsedExtensions();

    m_table->setRowCount(labels.size());
    for (int r = 0; r < labels.size(); ++r)
    {
        const QString name = labels[r];
        const bool enabled = Settings::getInstance().isExtensionLabelEnabled(name);
        const bool isUsed = used.contains(name.toUpper());
        const ExtensionVisualStyle vs = extensionStyleForName(name);

        auto* itEnabled = new QTableWidgetItem(enabled ? "1" : "0");
        auto* itUsed = new QTableWidgetItem(isUsed ? "1" : "0");
        auto* itName = new QTableWidgetItem(name);
        auto* itMarker = new QTableWidgetItem(vs.markerText);
        auto* itColor = new QTableWidgetItem();
        itColor->setIcon(QIcon(drawMarkerIcon(vs)));

        // Visually de-emphasize disabled/unused entries.
        if (!enabled)
        {
            for (auto* it : {itEnabled, itUsed, itName, itMarker, itColor})
                it->setForeground(QBrush(QColor(140, 140, 140)));
        }
        if (enabled && !isUsed)
        {
            itUsed->setForeground(QBrush(QColor(160, 160, 160)));
        }

        m_table->setItem(r, 0, itEnabled);
        m_table->setItem(r, 1, itUsed);
        m_table->setItem(r, 2, itName);
        m_table->setItem(r, 3, itMarker);
        m_table->setItem(r, 4, itColor);
    }
}
