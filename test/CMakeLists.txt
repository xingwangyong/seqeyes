cmake_minimum_required(VERSION 3.20)

set(TEST_NAME TimeSliderSyncTest)

# Enable Qt Auto process
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Set AutoUic search paths so it can find UI files in src directory
set(CMAKE_AUTOUIC_SEARCH_PATHS ${PROJECT_SOURCE_DIR}/src)

find_package(Qt6 COMPONENTS Test Core Gui Widgets PrintSupport REQUIRED)

# External libraries from the main project
set(EXTERNAL_PULSEQ_DIR ${PROJECT_SOURCE_DIR}/src/external/pulseq)
set(EXTERNAL_QCP_DIR ${PROJECT_SOURCE_DIR}/src/external/qcustomplot)

set(TEST_UI_FILES
    ${PROJECT_SOURCE_DIR}/src/mainwindow.ui
)

set(SEQEYES_TEST_APP_SOURCES
    ${PROJECT_SOURCE_DIR}/src/InteractionHandler.cpp
    ${PROJECT_SOURCE_DIR}/src/LogManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mainwindow.cpp
    ${PROJECT_SOURCE_DIR}/src/TrajectoryColormap.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqLoadTransaction.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqLoadUiAdapter.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqLoader.cpp
    ${PROJECT_SOURCE_DIR}/src/PulseqOpenController.cpp
    ${PROJECT_SOURCE_DIR}/src/NumericLineEdit.h
    ${PROJECT_SOURCE_DIR}/src/SeriesBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/KSpaceTrajectory.cpp
    ${PROJECT_SOURCE_DIR}/src/PnsCalculator.cpp
    ${PROJECT_SOURCE_DIR}/src/M1Calculator.cpp
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

add_executable(${TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/TimeSliderSyncTest.cpp
    ${SEQEYES_TEST_APP_SOURCES}
)

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
)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})


# ReopenEquivalenceTest: history-independence of reopen (no stale state leaks
# from a previously opened file onto the next one). Reuses the same app source
# set as TimeSliderSyncTest, swapping the test entry point.
set(REOPEN_TEST_NAME ReopenEquivalenceTest)
add_executable(${REOPEN_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/ReopenEquivalenceTest.cpp
    ${SEQEYES_TEST_APP_SOURCES}
)

target_include_directories(${REOPEN_TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
    ${EXTERNAL_QCP_DIR}
)

# Bake the absolute seq_files path so the test finds its inputs regardless of
# build layout or current working directory.
target_compile_definitions(${REOPEN_TEST_NAME} PRIVATE
    SEQ_FILES_DIR="${PROJECT_SOURCE_DIR}/test/seq_files"
)

target_link_libraries(${REOPEN_TEST_NAME} PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
)

add_test(NAME ${REOPEN_TEST_NAME} COMMAND ${REOPEN_TEST_NAME})


# PulseqLoaderStateTest: focused path/open-state lifecycle checks.
set(PULSEQ_LOADER_STATE_TEST_NAME PulseqLoaderStateTest)
add_executable(${PULSEQ_LOADER_STATE_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/PulseqLoaderStateTest.cpp
    ${SEQEYES_TEST_APP_SOURCES}
)

target_include_directories(${PULSEQ_LOADER_STATE_TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
    ${EXTERNAL_QCP_DIR}
)

target_compile_definitions(${PULSEQ_LOADER_STATE_TEST_NAME} PRIVATE
    SEQ_FILES_DIR="${PROJECT_SOURCE_DIR}/test/seq_files"
)

target_link_libraries(${PULSEQ_LOADER_STATE_TEST_NAME} PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
)

add_test(NAME ${PULSEQ_LOADER_STATE_TEST_NAME} COMMAND ${PULSEQ_LOADER_STATE_TEST_NAME})


# RfMultichannelDetectionTest: RoosPtxHack detection and RF_SHIMS conflict checks
set(RF_MULTICHANNEL_TEST_NAME RfMultichannelDetectionTest)
add_executable(${RF_MULTICHANNEL_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/RfMultichannelDetectionTest.cpp
    ${SEQEYES_TEST_APP_SOURCES}
)

target_include_directories(${RF_MULTICHANNEL_TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
    ${EXTERNAL_QCP_DIR}
)

target_compile_definitions(${RF_MULTICHANNEL_TEST_NAME} PRIVATE
    SEQ_FILES_DIR="${PROJECT_SOURCE_DIR}/test/seq_files"
)

target_link_libraries(${RF_MULTICHANNEL_TEST_NAME} PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
)

add_test(NAME ${RF_MULTICHANNEL_TEST_NAME} COMMAND ${RF_MULTICHANNEL_TEST_NAME})


# PerfZoomTest: headless timing of programmatic zoom-in
set(PERF_TEST_NAME PerfZoomTest)
add_executable(${PERF_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/PerfZoomTest.cpp
    ${SEQEYES_TEST_APP_SOURCES}
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
)

# PnsDumpTest: export SeqEyes PNS arrays (time/x/y/z/norm) to CSV for MATLAB comparison
set(PNS_DUMP_TEST_NAME PnsDumpTest)
add_executable(${PNS_DUMP_TEST_NAME}
    ${PROJECT_SOURCE_DIR}/test/PnsDumpTest.cpp
    ${PROJECT_SOURCE_DIR}/src/PnsCalculator.cpp
    ${EXTERNAL_PULSEQ_DIR}/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/ExternalSequence.cpp
    ${EXTERNAL_PULSEQ_DIR}/v151/md5.cpp
)

target_include_directories(${PNS_DUMP_TEST_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${EXTERNAL_PULSEQ_DIR}
)

target_link_libraries(${PNS_DUMP_TEST_NAME} PRIVATE
    Qt6::Core
)
