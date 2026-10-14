qt_add_library(cppedit_ui STATIC
    DataInspector.cpp DataInspector.h
    DocumentView.cpp DocumentView.h
    HexView.cpp HexView.h
    MainWindow.cpp MainWindow.h
    Paths.h
)
target_link_libraries(cppedit_ui
    PUBLIC cppedit_core Qt6::Widgets
    PRIVATE cppedit_warnings)
