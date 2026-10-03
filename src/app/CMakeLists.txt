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
        Apps.cpp Apps.h
        Devices.cpp Devices.h
        MeterBallistics.h
        Mixer.cpp Mixer.h
        Preferences.cpp Preferences.h
        RowsModel.cpp RowsModel.h
        Scenes.cpp Scenes.h
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
        qml/MixerPage.qml
        qml/MasterFader.qml
        qml/BusStrip.qml
        qml/Fader.qml
        qml/PlainSlider.qml
        qml/PeakMeter.qml
        qml/DestinationControl.qml
        qml/AppChip.qml
        qml/ColorPopup.qml
        qml/AppsPage.qml
        qml/AppRow.qml
        qml/RuleRow.qml
        qml/BusMenu.qml
        qml/ScenesPage.qml
        qml/SceneNameDialog.qml
        qml/DevicesPage.qml
        qml/DeviceList.qml
        qml/ObsPage.qml
        qml/SettingsPage.qml
        qml/Wizard.qml
        qml/MixDiagram.qml
)

# Shared system Qt: QML imports resolve at runtime, no static plugin import scan needed.
set_target_properties(rostrum PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_include_directories(rostrum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rostrum PRIVATE
    ROSTRUM_DOC_DIR="${CMAKE_INSTALL_FULL_DOCDIR}"
    ROSTRUM_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
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
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
