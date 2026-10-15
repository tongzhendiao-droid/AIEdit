qt_add_library(aiedit_ui STATIC
    AiPanel.cpp AiPanel.h
    AiSettings.cpp AiSettings.h
    AiSettingsDialog.cpp AiSettingsDialog.h
    AiTools.cpp AiTools.h
    BackupStore.cpp BackupStore.h
    ChatClient.cpp ChatClient.h
    Contents.cpp Contents.h
    ContentsPanel.cpp ContentsPanel.h
    DataInspector.cpp DataInspector.h
    DocumentView.cpp DocumentView.h
    FieldMark.h
    FileSearch.cpp FileSearch.h
    FilterPanel.cpp FilterPanel.h
    FilterResultsView.cpp FilterResultsView.h
    FilterRuleWriter.cpp FilterRuleWriter.h
    FindBar.cpp FindBar.h
    FindInFilesPanel.cpp FindInFilesPanel.h
    HexView.cpp HexView.h
    LineOperations.cpp LineOperations.h
    LogFilter.cpp LogFilter.h
    MainWindow.cpp MainWindow.h
    Paths.h
    RowScrollBar.cpp RowScrollBar.h
    Replace.cpp Replace.h
    SearchJob.cpp SearchJob.h
    SingleInstance.cpp SingleInstance.h
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
