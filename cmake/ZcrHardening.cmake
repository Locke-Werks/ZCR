# Exploit mitigations, following Forge's cmake/LwiHardening.cmake.
#
# This binary does not parse attacker-controlled bytes the way an installer does,
# so the threat model is milder. The flags are near free and the portable case is
# real: people do run this from a Downloads folder, which is exactly the DLL
# planting scenario /DEPENDENTLOADFLAG closes.
#
# /Qspectre is conditional. It needs the "MSVC v143 - VS 2022 C++ x64/x86
# Spectre-mitigated libs" component, which is NOT installed by default in Visual
# Studio Community. Requiring it unconditionally makes a fresh clone fail to
# configure for a reason that reads like a code error, so it is detected and
# reported instead.

set(ZCR_SPECTRE_LIBS_FOUND FALSE)
if(MSVC AND DEFINED CMAKE_CXX_COMPILER)
    get_filename_component(_zcr_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64 -> .../VC/Tools/MSVC/<ver>
    get_filename_component(_zcr_msvc_root "${_zcr_msvc_bin}/../../.." ABSOLUTE)
    if(EXISTS "${_zcr_msvc_root}/lib/spectre/x64")
        set(ZCR_SPECTRE_LIBS_FOUND TRUE)
    endif()
endif()

if(ZCR_HARDENING AND NOT ZCR_SPECTRE_LIBS_FOUND)
    message(STATUS
        "zcr: Spectre-mitigated libs not found, building without /Qspectre.\n"
        "      To enable: Visual Studio Installer > Modify > Individual components >\n"
        "      \"MSVC v143 - VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)\"")
endif()

function(zcr_apply_hardening target)
    if(NOT MSVC OR NOT ZCR_HARDENING)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        /GS                       # stack buffer overrun detection
        /guard:cf                 # Control Flow Guard
        /sdl                      # additional security checks, promotes some warnings
        /Gy /Gw                   # function and data COMDATs, lets /OPT:REF do its job
        /guard:ehcont
        $<$<BOOL:${ZCR_SPECTRE_LIBS_FOUND}>:/Qspectre>
    )

    target_link_options(${target} PRIVATE
        /GUARD:CF
        /GUARD:EHCONT
        /DYNAMICBASE              # ASLR
        /HIGHENTROPYVA            # 64-bit ASLR entropy
        /NXCOMPAT                 # DEP
        /CETCOMPAT                # shadow stack
        # LOAD_LIBRARY_SEARCH_SYSTEM32 for implicit imports. Covers implicit
        # imports only, which is why SetDefaultDllDirectories is still the first
        # call in wWinMain.
        /DEPENDENTLOADFLAG:0x800
        /OPT:REF /OPT:ICF
        /INCREMENTAL:NO
    )
endfunction()
