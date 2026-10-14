qt_add_library(aiedit_ui STATIC
    AiPanel.cpp AiPanel.h
    AiSettings.cpp AiSettings.h
    AiSettingsDialog.cpp AiSettingsDialog.h
    AiTools.cpp AiTools.h
    ChatClient.cpp ChatClient.h
    DataInspector.cpp DataInspector.h
    DocumentView.cpp DocumentView.h
    FieldMark.h
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
target_link_libraries(aiedit_ui
    PUBLIC aiedit_core Qt6::Widgets Qt6::Concurrent Qt6::Network
    PRIVATE aiedit_warnings)

if(KF6SyntaxHighlighting_FOUND)
    target_link_libraries(aiedit_ui PUBLIC KF6::SyntaxHighlighting)
    target_compile_definitions(aiedit_ui PUBLIC AIEDIT_HAVE_KSYNTAXHIGHLIGHTING)
endif()
