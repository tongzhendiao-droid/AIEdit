qt_add_library(aiedit_ui STATIC
    AiEdit.cpp AiEdit.h
    AiEditBar.cpp AiEditBar.h
    AiPanel.cpp AiPanel.h
    AiSettings.cpp AiSettings.h
    AiSettingsDialog.cpp AiSettingsDialog.h
    AiTools.cpp AiTools.h
    BackupStore.cpp BackupStore.h
    ChatClient.cpp ChatClient.h
    Codec.cpp Codec.h
    Collation.h
    Compare.cpp Compare.h
    CompareTab.cpp CompareTab.h
    CompareView.cpp CompareView.h
    Contents.cpp Contents.h
    ContentsPanel.cpp ContentsPanel.h
    DataInspector.cpp DataInspector.h
    Desktop.cpp Desktop.h
    Extract.cpp Extract.h
    DocumentView.cpp DocumentView.h
    FieldMark.h
    FileSearch.cpp FileSearch.h
    FileTypeSniffer.cpp FileTypeSniffer.h
    FilterPanel.cpp FilterPanel.h
    FilterResultsView.cpp FilterResultsView.h
    FilterRuleWriter.cpp FilterRuleWriter.h
    FindBar.cpp FindBar.h
    FindInFilesPanel.cpp FindInFilesPanel.h
    FolderModels.cpp FolderModels.h
    FolderPanel.cpp FolderPanel.h
    FolderSummary.cpp FolderSummary.h
    FuzzyMatch.cpp FuzzyMatch.h
    GitIgnore.cpp GitIgnore.h
    HexCompareView.cpp HexCompareView.h
    HexView.cpp HexView.h
    LineOperations.cpp LineOperations.h
    LogFilter.cpp LogFilter.h
    MainWindow.cpp MainWindow.h
    Markdown.cpp Markdown.h
    MarkdownView.cpp MarkdownView.h
    Paths.h
    QuickOpen.cpp QuickOpen.h
    RowScrollBar.cpp RowScrollBar.h
    Replace.cpp Replace.h
    SearchJob.cpp SearchJob.h
    SingleInstance.cpp SingleInstance.h
    SyntaxHighlighting.cpp SyntaxHighlighting.h
    TableView.cpp TableView.h
    TabWidget.cpp TabWidget.h
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
    # AIEdit's own colour schemes, where KSyntaxHighlighting looks for an application's themes.
    qt_add_resources(aiedit_ui "themes"
        PREFIX "/org.kde.syntax-highlighting/themes-addons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/themes"
        FILES themes/aiedit-paper.theme themes/aiedit-eye-care.theme themes/aiedit-night.theme)
    # Languages of AIEdit's own: 易语言, as its sources are shown (M10).
    qt_add_resources(aiedit_ui "syntax"
        PREFIX "/org.kde.syntax-highlighting/syntax-addons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/syntax"
        FILES syntax/epl.xml)
endif()
