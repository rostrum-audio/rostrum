find_package(Qt6 REQUIRED COMPONENTS Test)

function(rostrum_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE rostrum_engine Qt6::Test)
    target_compile_definitions(${name} PRIVATE ROSTRUM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

rostrum_add_test(tst_paths)
rostrum_add_test(tst_requirements)
rostrum_add_test(tst_model)
rostrum_add_test(tst_appidentity)
rostrum_add_test(tst_appclassifier)
rostrum_add_test(tst_privacy)
rostrum_add_test(tst_scenes)
rostrum_add_test(tst_controls)
rostrum_add_test(tst_engine)
rostrum_add_test(tst_history)
rostrum_add_test(tst_backup)
rostrum_add_test(tst_ruleexport)
rostrum_add_test(tst_autostart)
rostrum_add_test(tst_testtone)
rostrum_add_test(tst_micfilters)
rostrum_add_test(tst_obs)
target_link_libraries(tst_obs PRIVATE rostrum_obs)
rostrum_add_test(tst_readiness)
target_link_libraries(tst_readiness PRIVATE rostrum_obs)

if(TARGET rostrum-dsp)
    add_executable(tst_dsp tst_dsp.cpp)
    target_link_libraries(tst_dsp PRIVATE Qt6::Test ${CMAKE_DL_LIBS})
    target_include_directories(tst_dsp PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(tst_dsp PRIVATE ROSTRUM_DSP_PLUGIN="$<TARGET_FILE:rostrum-dsp>")
    if(ROSTRUM_DSP_DENOISE)
        target_compile_definitions(tst_dsp PRIVATE ROSTRUM_DSP_DENOISE)
    endif()
    add_dependencies(tst_dsp rostrum-dsp)
    add_test(NAME tst_dsp COMMAND tst_dsp)
endif()

find_package(Qt6 QUIET COMPONENTS QuickTest)
if(TARGET rostrum AND TARGET Qt6::QuickTest)
    add_executable(tst_quick tst_quick.cpp)
    target_link_libraries(tst_quick PRIVATE Qt6::QuickTest)
    add_test(NAME tst_quick COMMAND tst_quick -input ${CMAKE_CURRENT_SOURCE_DIR}/qml)
    set_tests_properties(tst_quick PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

find_program(DBUS_RUN_SESSION dbus-run-session)
find_program(GDBUS gdbus)
if(TARGET rostrum AND DBUS_RUN_SESSION AND GDBUS)
    add_test(NAME dbus_control COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dbus-control.sh $<TARGET_FILE:rostrum>)
    set_tests_properties(dbus_control PROPERTIES TIMEOUT 120)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME offscreen_diagnostics COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_offscreen_diagnostics.py)
    set_tests_properties(offscreen_diagnostics PROPERTIES TIMEOUT 45)
    add_test(NAME appimage_audit COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_appimage_audit.py)
    rostrum_add_test(tst_releasefeed)
    target_compile_definitions(tst_releasefeed PRIVATE
        ROSTRUM_PYTHON="${Python3_EXECUTABLE}" ROSTRUM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

# Desktop metadata checks run where the validators are installed (CI's app build has both).
find_program(APPSTREAMCLI_EXECUTABLE appstreamcli)
if(APPSTREAMCLI_EXECUTABLE)
    add_test(NAME metainfo_valid
        COMMAND ${APPSTREAMCLI_EXECUTABLE} validate --no-net ${PROJECT_SOURCE_DIR}/data/${ROSTRUM_APP_ID}.metainfo.xml)
endif()
find_program(DESKTOP_FILE_VALIDATE_EXECUTABLE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE_EXECUTABLE)
    add_test(NAME desktop_file_valid
        COMMAND ${DESKTOP_FILE_VALIDATE_EXECUTABLE} ${PROJECT_SOURCE_DIR}/data/${ROSTRUM_APP_ID}.desktop)
endif()

# Explicit opt-in; a missing prerequisite is a configuration error, never a skip.
if(ROSTRUM_AUDIO_INTEGRATION_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(tool pipewire wireplumber pw-cat pw-cli pw-dump pw-link dbus-daemon)
        find_program(AUDIO_${tool} NAMES ${tool} REQUIRED)
    endforeach()
    add_test(NAME audio_safety COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/audio-safety.py
        --driver $<TARGET_FILE:rostrum-graphtest>)
    add_test(NAME audio_safety_fixture COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_fixture.py
        $<TARGET_FILE:rostrum-graphtest> -v)
    add_test(NAME audio_safety_lifecycle COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_lifecycle.py
        $<TARGET_FILE:rostrum-graphtest> -v)
    add_test(NAME audio_readiness COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/integration/readiness-routing.py $<TARGET_FILE:rostrum-graphtest>)
    if(TARGET rostrum-dsp)
        set(mic_filter_test_options "")
        if(ROSTRUM_DSP_DENOISE)
            list(APPEND mic_filter_test_options --denoise)
        endif()
        add_test(NAME audio_mic_filter_scheduling COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration/mic-filter-scheduling.py
            $<TARGET_FILE:rostrum-graphtest> $<TARGET_FILE:rostrum-dsp> ${mic_filter_test_options})
        set_tests_properties(audio_mic_filter_scheduling PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE LABELS "audio-integration")
    endif()
    # Lifecycle regressions deliberately interrupt private jobs; serialize audio tests.
    set_tests_properties(audio_safety audio_safety_fixture audio_safety_lifecycle audio_readiness PROPERTIES
        TIMEOUT 180 RUN_SERIAL TRUE LABELS "audio-integration")
endif()
