# Embed the same complete bundle in the app and its installer test; no runtime source paths.
function(rostrum_embed_opendeck target)
    set(bundle "${PROJECT_SOURCE_DIR}/tools/opendeck/dev.getrostrum.Rostrum.sdPlugin")
    qt_add_resources(${target} opendeck_bundle
        PREFIX "/opendeck"
        BASE "${bundle}"
        FILES
            "${bundle}/manifest.json"
            "${bundle}/plugin.sh"
            "${bundle}/plugin.py"
            "${bundle}/dbus_client.py"
            "${bundle}/inspector.html"
            "${bundle}/sounds/mic-muted.wav"
            "${bundle}/sounds/mic-live.wav"
            "${bundle}/images/mic.svg"
            "${bundle}/images/mic-muted.svg"
            "${bundle}/images/panic.svg"
            "${bundle}/images/panic-active.svg"
            "${bundle}/images/scene.svg"
            "${bundle}/images/scene-active.svg"
            "${bundle}/images/bus.svg"
            "${bundle}/images/bus-muted.svg"
            "${bundle}/images/studio-voice.svg"
            "${bundle}/images/studio-voice-off.svg"
            "${bundle}/images/push-to-talk.svg"
            "${bundle}/images/push-to-talk-active.svg"
    )
endfunction()
