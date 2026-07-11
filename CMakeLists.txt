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

if(WIN32)
    set(_seqeyes_path_sep ";")
else()
    set(_seqeyes_path_sep ":")
endif()
set(ENV{PATH} "${CMAKE_PREFIX_PATH}/bin${_seqeyes_path_sep}$ENV{PATH}")

set(PROJECT_ROOT ${CMAKE_SOURCE_DIR})
set(EXTERNAL_DIR ${CMAKE_SOURCE_DIR}/src/external)
set(PULSEQ_DIR ${CMAKE_SOURCE_DIR}/src/external/pulseq)
set(QCUSTOM_PLOT_DIR ${CMAKE_SOURCE_DIR}/src/external/qcustomplot)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
    Svg
    Widgets
    PrintSupport  # For QCustomPlot
    REQUIRED
)

set(UI_LIST
    ${PROJECT_ROOT}/src/mainwindow.ui
)

set(RESOURCE_LIST
    ${PROJECT_ROOT}/resources/resources.qrc
)
source_group("Form" FILES ${UI_LIST})

set(SRC_LIST
    ${PROJECT_ROOT}/src/main.cpp
    ${PROJECT_ROOT}/src/InteractionHandler.cpp
    ${PROJECT_ROOT}/src/LogManager.cpp
    ${PROJECT_ROOT}/src/mainwindow.cpp
    ${PROJECT_ROOT}/src/PulseqLoader.cpp
    ${PROJECT_ROOT}/src/SeriesBuilder.cpp
    ${PROJECT_ROOT}/src/KSpaceTrajectory.cpp
    ${PROJECT_ROOT}/src/M1Calculator.cpp
    ${PROJECT_ROOT}/src/PnsCalculator.cpp
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
    ${PROJECT_ROOT}/src/M1Calculator.h
    ${PROJECT_ROOT}/src/PnsCalculator.h
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

add_executable(${PROJECT_NAME} ${SRC_LIST} ${HEADER_LIST} ${UI_LIST} ${RESOURCE_LIST} ${EXTERNAL_LIST})

# Ensure UTF-8 source interpretation on MSVC so Unicode string literals (e.g. "→", "•", "–") are stable.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# macOS-only helper: convert SVG logo to ICNS for app bundle icon.
# Built on demand (not part of default ALL target):
#   cmake --build <build_dir> --target svg_to_icns
if(APPLE)
    add_executable(svg_to_icns EXCLUDE_FROM_ALL
        ${PROJECT_ROOT}/tools/svg_to_icns.cpp
    )
    target_link_libraries(svg_to_icns PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Svg
    )
endif()

# Build as a GUI app on desktop platforms so launching the packaged binary
# doesn't open an extra console window or lose bundle metadata on macOS.
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH FALSE
    )

    # macOS app icon (.icns) for Finder/Dock.
    # Prefer .icns directly, then .ico, then .svg as a last resort.
    # Note: .svg is not a native bundle icon format on macOS.
    set(SEQEYES_APP_ICON_ICNS ${PROJECT_ROOT}/resources/images/logo.icns)
    set(SEQEYES_APP_ICON_ICO  ${PROJECT_ROOT}/resources/images/logo.ico)
    set(SEQEYES_APP_ICON_SVG  ${PROJECT_ROOT}/resources/images/logo.svg)

    if(EXISTS "${SEQEYES_APP_ICON_ICNS}")
        set_source_files_properties("${SEQEYES_APP_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(${PROJECT_NAME} PRIVATE "${SEQEYES_APP_ICON_ICNS}")
        set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE_ICON_FILE "logo.icns")
    else()
        set(SEQEYES_APP_ICON_SOURCE "")
        if(EXISTS "${SEQEYES_APP_ICON_ICO}")
            set(SEQEYES_APP_ICON_SOURCE "${SEQEYES_APP_ICON_ICO}")
        elseif(EXISTS "${SEQEYES_APP_ICON_SVG}")
            set(SEQEYES_APP_ICON_SOURCE "${SEQEYES_APP_ICON_SVG}")
        endif()

        if(NOT SEQEYES_APP_ICON_SOURCE STREQUAL "")
            find_program(SIPS_EXECUTABLE sips)
            find_program(ICONUTIL_EXECUTABLE iconutil)
            if(SIPS_EXECUTABLE AND ICONUTIL_EXECUTABLE)
                set(SEQEYES_APP_ICONSET_DIR ${CMAKE_BINARY_DIR}/seqeyes.iconset)
                set(SEQEYES_APP_ICNS ${CMAKE_BINARY_DIR}/seqeyes.icns)
                file(MAKE_DIRECTORY "${SEQEYES_APP_ICONSET_DIR}")

                # Generate the required iconset filenames. We render each logical size
                # and its @2x variant even if pixel dimensions overlap by design.
                set(_seqeyes_base_sizes 16 32 128 256 512)
                set(_seqeyes_icon_ok TRUE)
                foreach(_base IN LISTS _seqeyes_base_sizes)
                    math(EXPR _retina "${_base} * 2")

                    set(_out_base "${SEQEYES_APP_ICONSET_DIR}/icon_${_base}x${_base}.png")
                    execute_process(
                        COMMAND "${SIPS_EXECUTABLE}" -s format png -z "${_base}" "${_base}" "${SEQEYES_APP_ICON_SOURCE}" --out "${_out_base}"
                        RESULT_VARIABLE _sips_result_base
                        OUTPUT_QUIET
                        ERROR_QUIET
                    )
                    if(NOT _sips_result_base EQUAL 0)
                        set(_seqeyes_icon_ok FALSE)
                    endif()

                    set(_out_retina "${SEQEYES_APP_ICONSET_DIR}/icon_${_base}x${_base}@2x.png")
                    execute_process(
                        COMMAND "${SIPS_EXECUTABLE}" -s format png -z "${_retina}" "${_retina}" "${SEQEYES_APP_ICON_SOURCE}" --out "${_out_retina}"
                        RESULT_VARIABLE _sips_result_retina
                        OUTPUT_QUIET
                        ERROR_QUIET
                    )
                    if(NOT _sips_result_retina EQUAL 0)
                        set(_seqeyes_icon_ok FALSE)
                    endif()
                endforeach()

                if(_seqeyes_icon_ok)
                    execute_process(
                        COMMAND "${ICONUTIL_EXECUTABLE}" -c icns "${SEQEYES_APP_ICONSET_DIR}" -o "${SEQEYES_APP_ICNS}"
                        RESULT_VARIABLE _iconutil_result
                        OUTPUT_QUIET
                        ERROR_QUIET
                    )
                    if(_iconutil_result EQUAL 0 AND EXISTS "${SEQEYES_APP_ICNS}")
                        set_source_files_properties("${SEQEYES_APP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
                        target_sources(${PROJECT_NAME} PRIVATE "${SEQEYES_APP_ICNS}")
                        set_target_properties(${PROJECT_NAME} PROPERTIES MACOSX_BUNDLE_ICON_FILE "seqeyes.icns")
                    else()
                        message(WARNING "Failed to create seqeyes.icns from ${SEQEYES_APP_ICON_SOURCE}; macOS app icon may be generic.")
                    endif()
                else()
                    message(WARNING "Failed to rasterize icon source ${SEQEYES_APP_ICON_SOURCE}; macOS app icon may be generic.")
                endif()
            else()
                message(WARNING "sips/iconutil not found; cannot generate macOS .icns app icon.")
            endif()
        endif()
    endif()
endif()

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
    Qt6::PrintSupport)

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

# Optional Windows executable icon (.ico) for taskbar/pinned shortcut.
# SVG is used at runtime in Qt; .ico is used by Windows shell when available.
if(WIN32)
    set(SEQEYES_APP_ICO ${PROJECT_ROOT}/resources/images/logo.ico)
    if(EXISTS "${SEQEYES_APP_ICO}")
        set(SEQEYES_APP_RC ${CMAKE_BINARY_DIR}/seqeyes_app_icon.rc)
        file(WRITE "${SEQEYES_APP_RC}" "IDI_ICON1 ICON \"${SEQEYES_APP_ICO}\"\n")
        target_sources(${PROJECT_NAME} PRIVATE "${SEQEYES_APP_RC}")
    endif()
endif()

# 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# Installation rules 鈥?used by scikit-build-core to populate the Python wheel.
# The seqeyes executable is placed in seqeyes/bin/ inside the wheel so that
# viewer.py can find it without requiring a separate install.
# 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION seqeyes/bin
    BUNDLE DESTINATION seqeyes/bin
)

# Bundle Qt runtime dependencies so the wheel is self-contained.
# On Windows and macOS, use Qt's deployment tooling during install so the
# wheel contains the runtime pieces users need without a separate Qt install.
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
if(APPLE)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake}" DIRECTORY)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${_qt_bin_dir}" REQUIRED)
    install(CODE "
        set(_seqeyes_app \"\${CMAKE_INSTALL_PREFIX}/seqeyes/bin/seqeyes.app\")
        message(STATUS \"Running macdeployqt to bundle Qt6 runtime into \${_seqeyes_app}...\")
        execute_process(
            COMMAND \"${MACDEPLOYQT_EXECUTABLE}\"
                \"\${_seqeyes_app}\"
                -always-overwrite
                -no-strip
                -verbose=1
            RESULT_VARIABLE _macdeployqt_result
        )
        if(NOT _macdeployqt_result EQUAL 0)
            message(FATAL_ERROR \"macdeployqt exited with code \${_macdeployqt_result}\")
        endif()
    ")
endif()
