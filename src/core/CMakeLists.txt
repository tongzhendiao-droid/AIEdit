# Pure C++ core: no Qt dependency, so it can be unit-tested in isolation and reused by a CLI later.
add_library(cppedit_core STATIC
    FileSource.cpp FileSource.h
    FileType.cpp FileType.h
    LineIndex.cpp LineIndex.h
    Offset.cpp Offset.h
    Search.cpp Search.h
    TextCodec.cpp TextCodec.h
)
target_include_directories(cppedit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(cppedit_core PUBLIC cxx_std_20)
target_link_libraries(cppedit_core PRIVATE cppedit_warnings)
set_target_properties(cppedit_core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(uchardet_FOUND)
    target_link_libraries(cppedit_core PRIVATE uchardet::libuchardet)
    target_compile_definitions(cppedit_core PUBLIC CPPEDIT_HAVE_UCHARDET)
endif()
