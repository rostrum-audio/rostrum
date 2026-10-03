foreach(kf CoreAddons DBusAddons I18n Kirigami)
    find_package(KF6${kf} 6.8 REQUIRED)
endforeach()
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(rostrum
    main.cpp
    Logging.cpp Logging.h
)

qt_add_qml_module(rostrum
    URI Rostrum
    VERSION 1.0
    SOURCES
        AppController.cpp AppController.h
    QML_FILES
        qml/Main.qml
        qml/Shell.qml
        qml/HeaderBar.qml
        qml/SceneSwitcher.qml
        qml/MicButton.qml
        qml/Sidebar.qml
        qml/StatusBar.qml
        qml/Banners.qml
        qml/PipeWireMissingPage.qml
        qml/PlaceholderPage.qml
        qml/MixerPage.qml
        qml/AppsPage.qml
        qml/ScenesPage.qml
        qml/DevicesPage.qml
        qml/ObsPage.qml
        qml/SettingsPage.qml
)

# Shared system Qt: QML imports resolve at runtime, no static plugin import scan needed.
set_target_properties(rostrum PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_include_directories(rostrum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rostrum PRIVATE
    rostrum_engine
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    KF6::CoreAddons
    KF6::DBusAddons
    KF6::I18n
    KF6::I18nQml
)

install(TARGETS rostrum RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
