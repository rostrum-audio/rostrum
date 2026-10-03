foreach(kf CoreAddons DBusAddons GlobalAccel I18n Kirigami StatusNotifierItem)
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
        Desktop.cpp Desktop.h
        Devices.cpp Devices.h
        Hotkeys.cpp Hotkeys.h
        MeterBallistics.h
        Mixer.cpp Mixer.h
        Preferences.cpp Preferences.h
        RowsModel.cpp RowsModel.h
        Scenes.cpp Scenes.h
        Tray.cpp Tray.h
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

# The tray draws these by pixmap so the muted variant works without an installed icon theme.
qt_add_resources(rostrum "icons"
    PREFIX "/icons"
    BASE ${PROJECT_SOURCE_DIR}/data/icons
    FILES
        ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}.svg
        ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}-muted.svg
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
    Qt6::DBus
    KF6::CoreAddons
    KF6::DBusAddons
    KF6::GlobalAccel
    KF6::StatusNotifierItem
    KF6::I18n
    KF6::I18nQml
)

install(TARGETS rostrum RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/${ROSTRUM_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
