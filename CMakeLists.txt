cmake_minimum_required(VERSION 3.20)

project(seqeyes)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add compatibility for v151 ExternalSequence which uses deprecated std::bind1st
# This allows the external library to compile with C++17
add_compile_definitions(_SILENCE_CXX17_BIND1ST_DEPRECATION_WARNING)

# Enable Qt message context (file/line/function) so LogManager can show origins
add_compile_definitions(QT_MESSAGELOGCONTEXT)

# Open Qt Auto process
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(ENV{PATH} "${CMAKE_PREFIX_PATH}/bin;$ENV{PATH}")

set(PROJECT_ROOT ${CMAKE_SOURCE_DIR})
set(EXTERNAL_DIR ${CMAKE_SOURCE_DIR}/src/external)
set(PULSEQ_DIR ${CMAKE_SOURCE_DIR}/src/external/pulseq)
set(QCUSTOM_PLOT_DIR ${CMAKE_SOURCE_DIR}/src/external/qcustomplot)

# --- Git version info header generated at build time (not configure time) ---
include_directories(${CMAKE_BINARY_DIR}/generated)
add_custom_target(gen_version ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${CMAKE_BINARY_DIR}/generated/version_autogen.h
          -DSRC_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenVersion.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version_autogen.h
  COMMENT "Generating version_autogen.h from Git (commit date + hash)"
)


SET(PULSEQ_LIST
${PULSEQ_DIR}/ExternalSequence.h
${PULSEQ_DIR}/ExternalSequence.cpp
# Only include v151 version since it's backward compatible
${PULSEQ_DIR}/v151/ExternalSequence.h
${PULSEQ_DIR}/v151/ExternalSequence.cpp
${PULSEQ_DIR}/v151/md5.h
${PULSEQ_DIR}/v151/md5.cpp
)

SET(QCUSTOM_PLOT_LIST
${QCUSTOM_PLOT_DIR}/qcustomplot.h
${QCUSTOM_PLOT_DIR}/qcustomplot.cpp
)

SET(EXTERNAL_LIST
    ${PULSEQ_LIST}
    ${QCUSTOM_PLOT_LIST}
)
source_group("External" FILES ${EXTERNAL_LIST})

# find QT6
find_package(Qt6 COMPONENTS 
    Core
    Gui
    Widgets
    PrintSupport  # For QCustomPlot
    Svg           # For QSvgGenerator (visual regression test export)
    REQUIRED
)

set(UI_LIST
    ${PROJECT_ROOT}/src/mainwindow.ui
)
source_group("Form" FILES ${UI_LIST})

set(RESOURCE_LIST
    ${PROJECT_ROOT}/src/resources.qrc
)
source_group("Resource" FILES ${RESOURCE_LIST})

set(SRC_LIST
    ${PROJECT_ROOT}/src/main.cpp
    ${PROJECT_ROOT}/src/InteractionHandler.cpp
    ${PROJECT_ROOT}/src/LogManager.cpp
    ${PROJECT_ROOT}/src/mainwindow.cpp
    ${PROJECT_ROOT}/src/PulseqLoader.cpp
    ${PROJECT_ROOT}/src/SeriesBuilder.cpp
    ${PROJECT_ROOT}/src/KSpaceTrajectory.cpp
    ${PROJECT_ROOT}/src/Settings.cpp
    ${PROJECT_ROOT}/src/SettingsDialog.cpp
    ${PROJECT_ROOT}/src/TRManager.cpp
    ${PROJECT_ROOT}/src/WaveformDrawer.cpp
    ${PROJECT_ROOT}/src/ExtensionPlotter.cpp
    ${PROJECT_ROOT}/src/ExtensionLegendDialog.cpp
    ${PROJECT_ROOT}/src/LogTableDialog.cpp
    ${PROJECT_ROOT}/src/doublerangeslider.cpp
    ${PROJECT_ROOT}/src/ZoomManager.cpp
    ${PROJECT_ROOT}/src/AutomationRunner.cpp
    ${PROJECT_ROOT}/src/TrajectoryColormap.cpp
)

set(HEADER_LIST
    ${PROJECT_ROOT}/src/InteractionHandler.h
    ${PROJECT_ROOT}/src/LogManager.h
    ${PROJECT_ROOT}/src/mainwindow.h
    ${PROJECT_ROOT}/src/seqeyes_version.h
    ${PROJECT_ROOT}/src/PulseqLoader.h
    ${PROJECT_ROOT}/src/NumericLineEdit.h
    ${PROJECT_ROOT}/src/SeriesBuilder.h
    ${PROJECT_ROOT}/src/KSpaceTrajectory.h
    ${PROJECT_ROOT}/src/PulseqLabelAnalyzer.h
    ${PROJECT_ROOT}/src/Settings.h
    ${PROJECT_ROOT}/src/SettingsDialog.h
    ${PROJECT_ROOT}/src/TRManager.h
    ${PROJECT_ROOT}/src/WaveformDrawer.h
    ${PROJECT_ROOT}/src/ExtensionPlotter.h
    ${PROJECT_ROOT}/src/ExtensionLegendDialog.h
    ${PROJECT_ROOT}/src/ExtensionStyleMap.h
    ${PROJECT_ROOT}/src/LogTableDialog.h
    ${PROJECT_ROOT}/src/doublerangeslider.h
    ${PROJECT_ROOT}/src/ZoomManager.h
    ${PROJECT_ROOT}/src/AutomationRunner.h
    ${PROJECT_ROOT}/src/TrajectoryColormap.h
)

include_directories(${PULSEQ_DIR} ${QCUSTOM_PLOT_DIR})

# On Windows, build as a GUI application (no separate console window)
add_executable(${PROJECT_NAME} WIN32 ${SRC_LIST} ${HEADER_LIST} ${UI_LIST} ${RESOURCE_LIST} ${EXTERNAL_LIST})

# C++ unit tests (disabled when building the Python wheel via scikit-build-core)
option(SEQEYES_BUILD_TESTS "Build C++ unit tests" ON)
if(SEQEYES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Link Qt6
target_link_libraries(${PROJECT_NAME} PRIVATE 
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Svg)

# Ensure version header is generated before building the app
add_dependencies(${PROJECT_NAME} gen_version)

# Suppress C++ Core Guidelines warnings for Qt and external libraries
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE 
        /wd26495  # Variable is uninitialized (mostly from Qt headers)
        /wd26451  # Arithmetic overflow
        /wd26439  # Function should not be called
        /wd26440  # Function can be declared noexcept
        /wd26438  # Avoid goto
        /wd26446  # Prefer to use gsl::at() instead of unchecked subscript operator
        /wd26447  # The function is declared 'noexcept' but calls function which may throw
        /wd26448  # Consider using gsl::span or gsl::string_span instead of passing raw pointers
        /wd26449  # Do not pass arguments that are not compatible with the format string
        /wd26450  # Use gsl::span instead of C-style arrays
        /wd26498  # Function is constexpr, mark variable constexpr if compile-time evaluation is desired
        /wd26812  # The enum type is unscoped. Prefer 'enum class' over 'enum'
        /wd26819  # Unannotated fallthrough between switch labels
        /wd4244   # Conversion from larger type to smaller type, possible loss of data
    )
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

# ──────────────────────────────────────────────────────────────────────────────
# Installation rules – used by scikit-build-core to populate the Python wheel.
# The seqeyes executable is placed in seqeyes/bin/ inside the wheel so that
# viewer.py can find it without requiring a separate install.
# ──────────────────────────────────────────────────────────────────────────────

# On macOS, set the install RPATH so delocate can resolve Qt frameworks that
# are bundled alongside the binary under seqeyes/bin/QtFrameworks/.
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "@executable_path/QtFrameworks"
    )
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION seqeyes/bin)

# Bundle Qt runtime dependencies so the wheel is self-contained.
# On Windows we use windeployqt; on other platforms the wheel-repair tools
# (auditwheel on Linux, delocate on macOS) are invoked in CI.
if(WIN32)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}" REQUIRED)
    install(CODE "
        message(STATUS \"Running windeployqt to bundle Qt6 runtime...\")
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --dir \"\${CMAKE_INSTALL_PREFIX}/seqeyes/bin\"
                \"\${CMAKE_INSTALL_PREFIX}/seqeyes/bin/seqeyes.exe\"
            RESULT_VARIABLE _wdqt_result
        )
        if(NOT _wdqt_result EQUAL 0)
            message(WARNING \"windeployqt exited with code \${_wdqt_result}\")
        endif()
    ")
endif()
