foreach(kf CoreAddons DBusAddons GlobalAccel I18n Kirigami StatusNotifierItem)
    find_package(KF6${kf} 6.8 REQUIRED)
endforeach()
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(rostrum
    main.cpp
    CrashCapture.cpp CrashCapture.h
    Logging.cpp Logging.h
)

qt_add_qml_module(rostrum
    URI Rostrum
    VERSION 1.0
    SOURCES
        AppController.cpp AppController.h
        Apps.cpp Apps.h
        CrashReports.cpp CrashReports.h
        Desktop.cpp Desktop.h
        Devices.cpp Devices.h
        Hotkeys.cpp Hotkeys.h
        MeterBallistics.h
        Mixer.cpp Mixer.h
        Obs.cpp Obs.h
        Preferences.cpp Preferences.h
        RowsModel.cpp RowsModel.h
        Scenes.cpp Scenes.h
        Tray.cpp Tray.h
        Updater.cpp Updater.h
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
        qml/StripToggle.qml
        qml/StreamDot.qml
        qml/Chevron.qml
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
        qml/PrivacyChoices.qml
        qml/CrashReportDialog.qml
        qml/SetupUpdateDialog.qml
)

# The tray draws these by pixmap so the muted variant works without an installed icon theme.
qt_add_resources(rostrum "icons"
    PREFIX "/icons"
    BASE ${PROJECT_SOURCE_DIR}/data/icons
    FILES
        ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}.svg
        ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}-tray.svg
        ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}-tray-muted.svg
)

# Shared system Qt: QML imports resolve at runtime, no static plugin import scan needed.
set_target_properties(rostrum PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_include_directories(rostrum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(rostrum PRIVATE
    ROSTRUM_DOC_DIR="${CMAKE_INSTALL_FULL_DOCDIR}"
    ROSTRUM_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    ROSTRUM_UPDATE_URL="${ROSTRUM_UPDATE_URL}"
    ROSTRUM_SENTRY_DSN="${ROSTRUM_SENTRY_DSN}"
)
target_link_libraries(rostrum PRIVATE
    rostrum_engine
    rostrum_obs
    Qt6::Network
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

# Always this pinned release, built in-process with no transport of its own: a system copy built
# with crashpad would upload crashes itself, past the user's choice. For offline builds, point
# FETCHCONTENT_SOURCE_DIR_SENTRY at an unpacked copy of the same release.
if(ROSTRUM_WITH_SENTRY)
    include(FetchContent)
    set(SENTRY_BACKEND "inproc" CACHE STRING "" FORCE)
    set(SENTRY_TRANSPORT "none" CACHE STRING "" FORCE)
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sentry
        URL https://github.com/getsentry/sentry-native/releases/download/0.17.1/sentry-native.zip
        URL_HASH SHA256=e510b714ac0fb5c24b08011e07c5b13fa01c9bd0f40708356e4af022aa20c5a1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sentry)
    target_link_libraries(rostrum PRIVATE sentry::sentry)
    target_compile_definitions(rostrum PRIVATE ROSTRUM_WITH_SENTRY)
endif()

install(TARGETS rostrum RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/data/${ROSTRUM_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/data/${ROSTRUM_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${ROSTRUM_APP_ID}.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(size 16x16 22x22)
    install(FILES ${PROJECT_SOURCE_DIR}/data/icons/${size}/${ROSTRUM_APP_ID}.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}/apps)
endforeach()
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
