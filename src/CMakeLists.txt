add_subdirectory(core)
add_subdirectory(ui)

qt_add_executable(cppedit main.cpp)
target_link_libraries(cppedit PRIVATE cppedit_ui cppedit_warnings)
target_compile_definitions(cppedit PRIVATE CPPEDIT_VERSION="${PROJECT_VERSION}")
set_target_properties(cppedit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)

# Translations. lrelease/lupdate are looked up directly because some distributions ship them
# without the Qt6LinguistTools CMake package. The .qm files are embedded under :/i18n.
set(qt_tool_hints
    "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
    "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}"
    /usr/lib/qt6/bin)
find_program(CPPEDIT_LRELEASE NAMES lrelease lrelease-qt6 HINTS ${qt_tool_hints})
find_program(CPPEDIT_LUPDATE NAMES lupdate lupdate-qt6 HINTS ${qt_tool_hints})
file(GLOB ts_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/translations/*.ts")

if(CPPEDIT_LRELEASE)
    set(qm_files)
    foreach(ts_file IN LISTS ts_files)
        get_filename_component(name "${ts_file}" NAME_WE)
        set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/i18n/${name}.qm")
        add_custom_command(OUTPUT "${qm_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/i18n"
            COMMAND "${CPPEDIT_LRELEASE}" -silent "${ts_file}" -qm "${qm_file}"
            DEPENDS "${ts_file}"
            VERBATIM)
        list(APPEND qm_files "${qm_file}")
    endforeach()
    qt_add_resources(cppedit "translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/i18n"
        FILES ${qm_files})
else()
    message(WARNING "lrelease not found: building without translations")
endif()

if(CPPEDIT_LUPDATE)
    # Refreshes the .ts files from the sources; run after changing user-visible strings.
    add_custom_target(update_translations
        COMMAND "${CPPEDIT_LUPDATE}" -no-obsolete -locations none -I "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" -ts ${ts_files}
        VERBATIM)
endif()

include(GNUInstallDirs)
install(TARGETS cppedit
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
