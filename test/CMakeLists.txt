cmake_minimum_required(VERSION 3.20)

set(TEST_NAME TimeSliderSyncTest)

# Enable Qt Auto process
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Set AutoUic search paths so it can find UI files in src directory
set(CMAKE_AUTOUIC_SEARCH_PATHS ${PROJECT_SOURCE_DIR}/src)

find_package(Qt6 COMPONENTS Test Core Gui Widgets PrintSupport Svg REQUIRED)

# External libraries from the main project
set(EXTERNAL_PULSEQ_DIR ${PROJECT_SOURCE_DIR}/src/external/pulseq)
set(EXTERNAL_QCP_DIR ${PROJECT_SOURCE_DIR}/src/external/qcustomplot)

set(TEST_UI_FILES
    ${PROJECT_SOURCE_DIR}/src/mainwindow.ui
)

set(TEST_SOURCES
    ${PROJECT_SOURCE_DIR}/test/TimeSliderSyncTest.cpp
    ${PROJECT_SOURCE_DIR}/src/InteractionHandler.cpp
    ${PROJECT_SOURCE_DIR}/src/LogManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mainwindow.cpp
    ${PROJECT_SOURCE_DIR}/src/TrajectoryColormap.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqLoader.cpp
    ${PROJECT_SOURCE_DIR}/src/NumericLineEdit.h
    ${PROJECT_SOURCE_DIR}/src/SeriesBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/KSpaceTrajectory.cpp
    ${PROJECT_SOURCE_DIR}/src/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/SettingsDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/TRManager.cpp
    ${PROJECT_SOURCE_DIR}/src/WaveformDrawer.cpp
    ${PROJECT_SOURCE_DIR}/src/ExtensionPlotter.cpp
    ${PROJECT_SOURCE_DIR}/src/ExtensionLegendDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/LogTableDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/doublerangeslider.cpp
    ${PROJECT_SOURCE_DIR}/src/ZoomManager.cpp
    ${EXTERNAL_PULSEQ_DIR}/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/md5.cpp
    ${EXTERNAL_QCP_DIR}/qcustomplot.cpp
)

add_executable(${TEST_NAME} ${TEST_SOURCES} ${TEST_UI_FILES})

target_include_directories(${TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
    ${EXTERNAL_QCP_DIR}
)

target_link_libraries(${TEST_NAME} PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})


# PerfZoomTest: headless timing of programmatic zoom-in
set(PERF_TEST_NAME PerfZoomTest)
add_executable(${PERF_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/PerfZoomTest.cpp
    ${PROJECT_SOURCE_DIR}/src/InteractionHandler.cpp
    ${PROJECT_SOURCE_DIR}/src/LogManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mainwindow.cpp
    ${PROJECT_SOURCE_DIR}/src/TrajectoryColormap.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqLoader.cpp
    ${PROJECT_SOURCE_DIR}/src/NumericLineEdit.h
    ${PROJECT_SOURCE_DIR}/src/SeriesBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/KSpaceTrajectory.cpp
    ${PROJECT_SOURCE_DIR}/src/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/SettingsDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/TRManager.cpp
    ${PROJECT_SOURCE_DIR}/src/WaveformDrawer.cpp
    ${PROJECT_SOURCE_DIR}/src/ExtensionPlotter.cpp
    ${PROJECT_SOURCE_DIR}/src/ExtensionLegendDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/LogTableDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/doublerangeslider.cpp
    ${PROJECT_SOURCE_DIR}/src/ZoomManager.cpp
    ${EXTERNAL_PULSEQ_DIR}/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/md5.cpp
    ${EXTERNAL_QCP_DIR}/qcustomplot.cpp
    ${TEST_UI_FILES}
)

target_include_directories(${PERF_TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
    ${EXTERNAL_QCP_DIR}
)

target_link_libraries(${PERF_TEST_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg
)
