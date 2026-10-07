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
            "${bundle}/inspector.html"
            "${bundle}/sounds/mic-muted.wav"
            "${bundle}/sounds/mic-live.wav"
            "${bundle}/images/mic.svg"
            "${bundle}/images/mic-muted.svg"
            "${bundle}/images/panic.svg"
            "${bundle}/images/scene.svg"
            "${bundle}/images/bus.svg"
    )
endfunction()
