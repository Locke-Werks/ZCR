# Embed an application manifest as a FILE, never as a link-line value.
#
# Copied from Forge's cmake/LwiManifest.cmake, comment included, because the
# comment is the valuable part. /MANIFESTUAC takes a value containing spaces and
# single-quoted attributes, which must be double-quoted for the linker, and CMake
# re-quotes target_link_options on the way through. What reached the linker in
# DeadLetter was a manifest whose attribute NAMES had been swallowed, so the
# binary carried invalid XML and Windows refused to start it with "side-by-side
# configuration is incorrect".
#
# :NO is deliberate. It stops the linker generating its own manifest, which would
# then be merged with, and could contradict, the one being embedded. This matters
# here: app.manifest is what makes the process PerMonitorV2 before any of our code
# runs, and monitor rects from DXGI are physical pixels.

function(zcr_embed_manifest target manifest)
    if(NOT MSVC)
        return()
    endif()

    target_link_options(${target} PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTUAC:NO"
        "/MANIFESTINPUT:${manifest}"
    )

    # A manifest is not a source file, so nothing else tells CMake it is an
    # input. Without this, editing it does not trigger a relink.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${manifest}")
endfunction()
