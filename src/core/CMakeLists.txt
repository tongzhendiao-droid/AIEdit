# Pure C++ core: no Qt dependency, so it can be unit-tested in isolation and reused by a CLI later.
add_library(aiedit_core STATIC
    BinaryLayout.cpp BinaryLayout.h
    ByteDiff.cpp ByteDiff.h
    Csv.cpp Csv.h
    Diff.cpp Diff.h
    Disassembler.cpp Disassembler.h
    EditBuffer.cpp EditBuffer.h
    Epl.cpp Epl.h
    EplLibraries.cpp EplLibraries.h
    FileSource.cpp FileSource.h
    FileSummary.cpp FileSummary.h
    FileType.cpp FileType.h
    Inflate.cpp Inflate.h
    LineIndex.cpp LineIndex.h
    MappingGuard.cpp MappingGuard.h
    Offset.cpp Offset.h
    Pdf.cpp Pdf.h
    Search.cpp Search.h
    Sse.cpp Sse.h
    TextCodec.cpp TextCodec.h
    Zip.cpp Zip.h
)
target_include_directories(aiedit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(aiedit_core PUBLIC cxx_std_20)
target_link_libraries(aiedit_core PRIVATE aiedit_warnings)
set_target_properties(aiedit_core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(CAPSTONE_FOUND)
    target_link_libraries(aiedit_core PRIVATE ${AIEDIT_CAPSTONE})
    target_compile_definitions(aiedit_core PRIVATE AIEDIT_HAVE_CAPSTONE)
endif()

if(uchardet_FOUND)
    target_link_libraries(aiedit_core PRIVATE uchardet::libuchardet)
    target_compile_definitions(aiedit_core PUBLIC AIEDIT_HAVE_UCHARDET)
endif()
