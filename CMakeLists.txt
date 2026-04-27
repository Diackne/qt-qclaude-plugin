cmake_minimum_required(VERSION 3.16)

project(QClaude VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED QTC_INSTALL_DIR AND DEFINED ENV{QTC_INSTALL_DIR})
    set(QTC_INSTALL_DIR "$ENV{QTC_INSTALL_DIR}")
endif()
if(QTC_INSTALL_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${QTC_INSTALL_DIR}")
endif()
if(DEFINED QtCreator_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${QtCreator_DIR}")
endif()

find_package(QtCreator REQUIRED COMPONENTS Core)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network)

# IDE_VERSION is set by QtCreatorIDEBranding.cmake (loaded via find_package
# above). Qt Creator 19 introduced Core::Constants::SETTINGS_CATEGORY_AI; on
# older versions the compat layer falls back to a hardcoded id.
if(IDE_VERSION VERSION_GREATER_EQUAL "19.0.0")
    set(QCLAUDE_HAS_SETTINGS_CATEGORY_AI ON)
endif()

add_qtc_plugin(QClaude
    PLUGIN_DEPENDS
        QtCreator::Core
        QtCreator::ProjectExplorer
        QtCreator::TextEditor
        QtCreator::DiffEditor
    DEPENDS
        Qt::Widgets
        Qt::Svg
        Qt::Network
        QtCreator::ExtensionSystem
        QtCreator::Utils
    SOURCES
        src/qclaude_compat.h
        src/qclaudeconstants.h
        src/qclaudetr.h
        src/qclaudeplugin.cpp
        src/qclaudepanel.h
        src/qclaudepanel.cpp
        src/qclaudesettings.h
        src/qclaudesettings.cpp
        src/qclaudediff.h
        src/qclaudediff.cpp
        src/qclaudeautocomplete.h
        src/qclaudeautocomplete.cpp
        src/qclaudefilepicker.h
        src/qclaudefilepicker.cpp
        src/qclaudehookserver.h
        src/qclaudehookserver.cpp
        src/claudeprocess.h
        src/claudeprocess.cpp
        resources/qclaude.qrc
)

if(QCLAUDE_HAS_SETTINGS_CATEGORY_AI)
    target_compile_definitions(QClaude PRIVATE QCLAUDE_HAS_SETTINGS_CATEGORY_AI)
endif()

# Pre-flight permission gate helpers. Both are tiny QtCore-only relays
# that ship next to the plugin .so so the panel can reference them by an
# absolute path without a separate install step.
#
# qclaude_mcp_permission is the active path: an MCP stdio server wired to
# claude via `--permission-prompt-tool`, gating every tool that needs
# permission (including `mcp__*` calls).
#
# qclaude_hook_bridge is the older PreToolUse-hook relay; it remains in
# the build for compatibility with previously-generated settings files
# but the panel no longer references it.
add_executable(qclaude_hook_bridge
    helpers/qclaude_hook_bridge.cpp
)
target_link_libraries(qclaude_hook_bridge PRIVATE Qt::Core Qt::Network)
set_target_properties(qclaude_hook_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:QClaude>
)
add_dependencies(QClaude qclaude_hook_bridge)

add_executable(qclaude_mcp_permission
    helpers/qclaude_mcp_permission.cpp
)
target_link_libraries(qclaude_mcp_permission PRIVATE Qt::Core Qt::Network)
set_target_properties(qclaude_mcp_permission PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:QClaude>
)
add_dependencies(QClaude qclaude_mcp_permission)

get_target_property(QtCreatorCorePath QtCreator::Core LOCATION)
find_program(QtCreatorExecutable
    NAMES qtcreator "Qt Creator"
    PATHS "${QtCreatorCorePath}/../../../bin"
    NO_DEFAULT_PATH
)
if(QtCreatorExecutable)
    add_custom_target(RunQtCreator
        COMMAND ${QtCreatorExecutable} -pluginpath $<TARGET_FILE_DIR:QClaude>
        DEPENDS QClaude
    )
    set_target_properties(RunQtCreator PROPERTIES FOLDER "qtc_runnable")
endif()
