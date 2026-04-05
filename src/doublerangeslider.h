#ifndef DOUBLERANGESLIDER_H
#define DOUBLERANGESLIDER_H

#include <QWidget>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QStyleOptionSlider>

/**
 * @brief DoubleRangeSlider - Custom dual slider control
 *
 * Features:
 * - True dual slider: Two independently draggable sliders on one slider bar
 * - Range selection: Support for selecting start and end values
 * - Mouse interaction: Support for dragging and wheel operations
 * - Visual feedback: Color changes when slider is pressed, middle shows selected range
 * - Signal emission: Emits startValueChanged, endValueChanged, valuesChanged signals
 *
 * Use cases:
 * - TR range selection: Select TRs 5-8
 * - Time range selection: Select time period 0.5-2.0 seconds
 */
class DoubleRangeSlider : public QWidget
{
    Q_OBJECT

public:
    explicit DoubleRangeSlider(QWidget *parent = nullptr);
    
    // Set range
    void setRange(int min, int max);
    void setMinimum(int min);
    void setMaximum(int max);
    
    // Get range
    int minimum() const { return m_minimum; }
    int maximum() const { return m_maximum; }
    
    // Set values
    void setStartValue(int value);
    void setEndValue(int value);
    void setValues(int start, int end);
    
    // Get values
    int startValue() const { return m_startValue; }
    int endValue() const { return m_endValue; }
    
    // Set step size
    void setSingleStep(int step) { m_singleStep = step; }
    int singleStep() const { return m_singleStep; }

signals:
    void startValueChanged(int value);
    void endValueChanged(int value);
    void valuesChanged(int start, int end);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void updateSliderPositions();
    int valueFromPosition(int pos) const;
    int positionFromValue(int value) const;
    QRect startSliderRect() const;
    QRect endSliderRect() const;
    bool isStartSliderAt(int pos) const;
    bool isEndSliderAt(int pos) const;

private:
    // ===== Basic Properties =====
    int m_minimum;        // Minimum value
    int m_maximum;        // Maximum value
    int m_startValue;     // Start value
    int m_endValue;       // End value
    int m_singleStep;     // Step size

    // ===== Slider State =====
    bool m_startSliderPressed;  // Whether start slider is pressed
    bool m_endSliderPressed;    // Whether end slider is pressed
    int m_pressedSlider;        // Currently pressed slider: 0=start, 1=end, -1=none

    // ===== Style Properties =====
    int m_sliderWidth;           // Slider width
    int m_sliderHeight;          // Slider height
    QColor m_trackColor;         // Track color (gray background)
    QColor m_rangeColor;         // Range color (blue selected area)
    QColor m_sliderColor;        // Slider color (dark gray)
    QColor m_sliderPressedColor; // Slider color when pressed
};

#endif // DOUBLERANGESLIDER_H
