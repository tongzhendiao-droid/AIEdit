find_package(Qt6 REQUIRED COMPONENTS Test)

# cppedit_add_test(<name> <libraries...>) builds <name>.cpp as a Qt Test executable.
function(cppedit_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test cppedit_warnings ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

cppedit_add_test(tst_filetype cppedit_core)
cppedit_add_test(tst_filesource cppedit_core)
cppedit_add_test(tst_offset cppedit_core)
cppedit_add_test(tst_hexview cppedit_ui)
# The translation test loads the .qm files built for the application.
target_compile_definitions(tst_hexview PRIVATE CPPEDIT_QM_DIR="${CMAKE_BINARY_DIR}/src/i18n")
add_dependencies(tst_hexview cppedit)
