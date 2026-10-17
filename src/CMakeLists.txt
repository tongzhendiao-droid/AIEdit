add_subdirectory(core)
add_subdirectory(ui)

qt_add_executable(aiedit main.cpp)
target_link_libraries(aiedit PRIVATE aiedit_ui aiedit_warnings)
target_compile_definitions(aiedit PRIVATE AIEDIT_VERSION="${PROJECT_VERSION}")
set_target_properties(aiedit PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON)
if(WIN32)
    set(AIEDIT_WINDOWS_ICON "${PROJECT_SOURCE_DIR}/packaging/windows/aiedit.ico")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/aiedit.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/aiedit.rc"
                   @ONLY)
    target_sources(aiedit PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/aiedit.rc")
endif()

# Translations. lrelease/lupdate are looked up directly because some distributions ship them
# without the Qt6LinguistTools CMake package. The .qm files are embedded under :/i18n.
set(qt_tool_hints
    "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
    "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}"
    /usr/lib/qt6/bin)
find_program(AIEDIT_LRELEASE NAMES lrelease lrelease-qt6 HINTS ${qt_tool_hints})
find_program(AIEDIT_LUPDATE NAMES lupdate lupdate-qt6 HINTS ${qt_tool_hints})
file(GLOB ts_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/translations/*.ts")

if(AIEDIT_LRELEASE)
    set(qm_files)
    foreach(ts_file IN LISTS ts_files)
        get_filename_component(name "${ts_file}" NAME_WE)
        set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/i18n/${name}.qm")
        add_custom_command(OUTPUT "${qm_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/i18n"
            COMMAND "${AIEDIT_LRELEASE}" -silent "${ts_file}" -qm "${qm_file}"
            DEPENDS "${ts_file}"
            VERBATIM)
        list(APPEND qm_files "${qm_file}")
    endforeach()
    qt_add_resources(aiedit "translations"
        PREFIX "/i18n"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/i18n"
        FILES ${qm_files})
else()
    message(WARNING "lrelease not found: building without translations")
endif()

if(AIEDIT_LUPDATE)
    # Refreshes the .ts files from the sources; run after changing user-visible strings.
    add_custom_target(update_translations
        COMMAND "${AIEDIT_LUPDATE}" -no-obsolete -locations none -I "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" -ts ${ts_files}
        VERBATIM)
endif()

# The icon also goes inside the binary, for systems without an icon theme. On Windows it is the
# .ico, which Qt reads without its SVG plugin.
if(WIN32)
    qt_add_resources(aiedit "icons"
        PREFIX "/icons"
        BASE "${PROJECT_SOURCE_DIR}/packaging/windows"
        FILES "${PROJECT_SOURCE_DIR}/packaging/windows/aiedit.ico")
else()
    qt_add_resources(aiedit "icons"
        PREFIX "/icons"
        BASE "${PROJECT_SOURCE_DIR}/packaging/icons"
        FILES "${PROJECT_SOURCE_DIR}/packaging/icons/aiedit.svg")
endif()

include(GNUInstallDirs)
install(TARGETS aiedit
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    # A folder that runs anywhere: KSyntaxHighlighting, then Qt's libraries, plugins and Chinese
    # translations (windeployqt), and the compiler's runtime, so that the Visual C++
    # redistributable need not be installed. (Capstone and uchardet are linked in.)
    if(KF6SyntaxHighlighting_FOUND)
        install(IMPORTED_RUNTIME_ARTIFACTS KF6::SyntaxHighlighting RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    qt_generate_deploy_app_script(TARGET aiedit
        OUTPUT_SCRIPT deploy_script
        NO_COMPILER_RUNTIME
        DEPLOY_TOOL_OPTIONS --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --translations zh_CN)
    install(SCRIPT "${deploy_script}")
    # windeployqt copies Windows' own icuuc.dll, which Qt links. Newer Windows' only passes calls
    # on to icu.dll, which Windows 10 1809 lacks, so the copy keeps AIEdit from starting there;
    # every Windows Qt runs on has an icuuc.dll of its own.
    install(CODE "file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/icuuc.dll\")")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
endif()

if(UNIX AND NOT APPLE)
    # Desktop integration: the launcher entry (with the absolute path of the installed binary,
    # which does not depend on PATH) and the icon in the hicolor theme.
    set(AIEDIT_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/aiedit")
    configure_file("${PROJECT_SOURCE_DIR}/packaging/linux/aiedit.desktop.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/aiedit.desktop" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/aiedit.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/aiedit.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
endif()
