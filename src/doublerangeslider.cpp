#include "doublerangeslider.h"
#include <QApplication>
#include <QStyleOptionSlider>
#include <QDebug>
#include <cmath>

DoubleRangeSlider::DoubleRangeSlider(QWidget *parent)
    : QWidget(parent)
    , m_minimum(0)
    , m_maximum(100)
    , m_startValue(0)
    , m_endValue(100)
    , m_singleStep(1)
    , m_startSliderPressed(false)
    , m_endSliderPressed(false)
    , m_pressedSlider(-1)
    , m_sliderWidth(12)
    , m_sliderHeight(20)
    , m_trackColor(QColor(220, 220, 220))
    , m_rangeColor(QColor(70, 130, 180))
    , m_sliderColor(QColor(50, 50, 50))
    , m_sliderPressedColor(QColor(50, 50, 50))
{
    setMinimumHeight(30);
    setMinimumWidth(200);
    setMouseTracking(true);
}

void DoubleRangeSlider::setRange(int min, int max)
{
    m_minimum = min;
    m_maximum = max;
    
    // Ensure values are within range
    if (m_startValue < m_minimum) m_startValue = m_minimum;
    if (m_startValue > m_maximum) m_startValue = m_maximum;
    if (m_endValue < m_minimum) m_endValue = m_minimum;
    if (m_endValue > m_maximum) m_endValue = m_maximum;
    
    update();
}

void DoubleRangeSlider::setMinimum(int min)
{
    setRange(min, m_maximum);
}

void DoubleRangeSlider::setMaximum(int max)
{
    setRange(m_minimum, max);
}

void DoubleRangeSlider::setStartValue(int value)
{
    if (value < m_minimum) value = m_minimum;
    if (value > m_maximum) value = m_maximum;
    if (value > m_endValue) value = m_endValue;
    
    if (m_startValue != value) {
        m_startValue = value;
        update();
        emit startValueChanged(m_startValue);
        emit valuesChanged(m_startValue, m_endValue);
    }
}

void DoubleRangeSlider::setEndValue(int value)
{
    if (value < m_minimum) value = m_minimum;
    if (value > m_maximum) value = m_maximum;
    if (value < m_startValue) value = m_startValue;
    
    if (m_endValue != value) {
        m_endValue = value;
        update();
        emit endValueChanged(m_endValue);
        emit valuesChanged(m_startValue, m_endValue);
    }
}

void DoubleRangeSlider::setValues(int start, int end)
{
    bool changed = false;
    
    if (start < m_minimum) start = m_minimum;
    if (start > m_maximum) start = m_maximum;
    if (end < m_minimum) end = m_minimum;
    if (end > m_maximum) end = m_maximum;
    if (start > end) start = end;
    
    if (m_startValue != start) {
        m_startValue = start;
        changed = true;
    }
    if (m_endValue != end) {
        m_endValue = end;
        changed = true;
    }
    
    if (changed) {
        update();
        emit startValueChanged(m_startValue);
        emit endValueChanged(m_endValue);
        emit valuesChanged(m_startValue, m_endValue);
    }
}

void DoubleRangeSlider::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    int trackY = height() / 2;
    int trackHeight = 4;
    int trackLeft = m_sliderWidth / 2;
    int trackRight = width() - m_sliderWidth / 2;
    int trackWidth = trackRight - trackLeft;
    
    // Draw track
    QRect trackRect(trackLeft, trackY - trackHeight / 2, trackWidth, trackHeight);
    painter.fillRect(trackRect, m_trackColor);
    
    // Draw range
    int startPos = positionFromValue(m_startValue);
    int endPos = positionFromValue(m_endValue);
    QRect rangeRect(startPos, trackY - trackHeight / 2, endPos - startPos, trackHeight);
    painter.fillRect(rangeRect, m_rangeColor);
    
    // Draw start slider - using rounded rectangle
    QRect startRect = startSliderRect();
    QColor startColor = m_startSliderPressed ? QColor(30, 30, 30) : m_sliderColor;
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(startColor);
    painter.drawRoundedRect(startRect, 3, 3);
    
    // Draw end slider - using rounded rectangle
    QRect endRect = endSliderRect();
    QColor endColor = m_endSliderPressed ? QColor(30, 30, 30) : m_sliderColor;
    painter.setPen(QPen(Qt::white, 1));
    painter.setBrush(endColor);
    painter.drawRoundedRect(endRect, 3, 3);
    
    // If two sliders are too close, draw a connecting line between them
    if (std::abs(startPos - endPos) < m_sliderWidth) {
        painter.setPen(QPen(Qt::darkGray, 2));
        int midY = trackY;
        painter.drawLine(startPos, midY, endPos, midY);
    }
}

void DoubleRangeSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    
    int pos = event->pos().x();
    
    if (isStartSliderAt(pos)) {
        m_startSliderPressed = true;
        m_pressedSlider = 0;
        setCursor(Qt::SizeHorCursor);
    } else if (isEndSliderAt(pos)) {
        m_endSliderPressed = true;
        m_pressedSlider = 1;
        setCursor(Qt::SizeHorCursor);
    }
}

void DoubleRangeSlider::mouseMoveEvent(QMouseEvent *event)
{
    int pos = event->pos().x();
    
    if (m_startSliderPressed && m_pressedSlider == 0) {
        int newValue = valueFromPosition(pos);
        setStartValue(newValue);
    } else if (m_endSliderPressed && m_pressedSlider == 1) {
        int newValue = valueFromPosition(pos);
        setEndValue(newValue);
    } else {
        // Update mouse cursor
        if (isStartSliderAt(pos) || isEndSliderAt(pos)) {
            setCursor(Qt::SizeHorCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }
    }
}

void DoubleRangeSlider::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    
    m_startSliderPressed = false;
    m_endSliderPressed = false;
    m_pressedSlider = -1;
    setCursor(Qt::ArrowCursor);
}

void DoubleRangeSlider::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y() > 0 ? m_singleStep : -m_singleStep;
    
    // Determine which slider the mouse position is closer to
    int pos = event->position().x();
    int startPos = positionFromValue(m_startValue);
    int endPos = positionFromValue(m_endValue);
    
    if (std::abs(pos - startPos) < std::abs(pos - endPos)) {
        // Closer to start slider
        setStartValue(m_startValue + delta);
    } else {
        // Closer to end slider
        setEndValue(m_endValue + delta);
    }
}

int DoubleRangeSlider::valueFromPosition(int pos) const
{
    int trackLeft = m_sliderWidth / 2;
    int trackRight = width() - m_sliderWidth / 2;
    int trackWidth = trackRight - trackLeft;
    
    if (trackWidth <= 0) return m_minimum;
    
    double ratio = (double)(pos - trackLeft) / trackWidth;
    int value = m_minimum + (int)(ratio * (m_maximum - m_minimum));
    
    // Align to step size
    if (m_singleStep > 1) {
        value = ((value - m_minimum) / m_singleStep) * m_singleStep + m_minimum;
    }
    
    if (value < m_minimum) value = m_minimum;
    if (value > m_maximum) value = m_maximum;
    
    return value;
}

int DoubleRangeSlider::positionFromValue(int value) const
{
    int trackLeft = m_sliderWidth / 2;
    int trackRight = width() - m_sliderWidth / 2;
    int trackWidth = trackRight - trackLeft;
    
    if (trackWidth <= 0 || m_maximum == m_minimum) return trackLeft;
    
    double ratio = (double)(value - m_minimum) / (m_maximum - m_minimum);
    return trackLeft + (int)(ratio * trackWidth);
}

QRect DoubleRangeSlider::startSliderRect() const
{
    int pos = positionFromValue(m_startValue);
    // Make start slider slightly offset upward to avoid overlapping with end slider
    int yOffset = (height() - m_sliderHeight) / 2 - 2;
    return QRect(pos - m_sliderWidth / 2, yOffset, m_sliderWidth, m_sliderHeight);
}

QRect DoubleRangeSlider::endSliderRect() const
{
    int pos = positionFromValue(m_endValue);
    // Make end slider slightly offset downward to avoid overlapping with start slider
    int yOffset = (height() - m_sliderHeight) / 2 + 2;
    return QRect(pos - m_sliderWidth / 2, yOffset, m_sliderWidth, m_sliderHeight);
}

bool DoubleRangeSlider::isStartSliderAt(int pos) const
{
    QRect rect = startSliderRect();
    return rect.contains(pos, rect.center().y());
}

bool DoubleRangeSlider::isEndSliderAt(int pos) const
{
    QRect rect = endSliderRect();
    return rect.contains(pos, rect.center().y());
}

//#include "doublerangeslider.moc"
