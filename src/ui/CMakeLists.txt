qt_add_library(cppedit_ui STATIC
    DataInspector.cpp DataInspector.h
    DocumentView.cpp DocumentView.h
    FindBar.cpp FindBar.h
    HexView.cpp HexView.h
    MainWindow.cpp MainWindow.h
    Paths.h
    RowScrollBar.cpp RowScrollBar.h
    SearchJob.cpp SearchJob.h
    SyntaxHighlighting.cpp SyntaxHighlighting.h
    TextDecoder.cpp TextDecoder.h
    TextDocument.cpp TextDocument.h
    TextView.cpp TextView.h
)
target_link_libraries(cppedit_ui
    PUBLIC cppedit_core Qt6::Widgets Qt6::Concurrent
    PRIVATE cppedit_warnings)

if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(cppedit_ui PUBLIC KF6::SyntaxHighlighting)
    target_compile_definitions(cppedit_ui PUBLIC CPPEDIT_HAVE_KSYNTAXHIGHLIGHTING)
endif()
