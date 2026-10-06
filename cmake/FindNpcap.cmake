# FindNpcap.cmake — locates the Npcap SDK (headers + wpcap import library).
#
# The Npcap *runtime* is installed by the Npcap installer into System32; the
# *SDK* (pcap.h, wpcap.lib) is a separate zip. bootstrap.ps1 drops it in
# third_party/npcap-sdk, which is the first place we look.
#
# Provides: Npcap::Npcap, NPCAP_FOUND, NPCAP_INCLUDE_DIR, NPCAP_LIBRARY

set(_npcap_hints
    "${CMAKE_SOURCE_DIR}/third_party/npcap-sdk"
    "$ENV{NPCAP_SDK_DIR}"
    "C:/npcap-sdk"
    "C:/Program Files/Npcap/SDK"
)

find_path(NPCAP_INCLUDE_DIR
    NAMES pcap.h
    HINTS ${_npcap_hints}
    PATH_SUFFIXES Include include
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_npcap_libdirs Lib/x64 lib/x64)
else()
    set(_npcap_libdirs Lib lib)
endif()

find_library(NPCAP_LIBRARY
    NAMES wpcap
    HINTS ${_npcap_hints}
    PATH_SUFFIXES ${_npcap_libdirs}
)

find_library(NPCAP_PACKET_LIBRARY
    NAMES Packet
    HINTS ${_npcap_hints}
    PATH_SUFFIXES ${_npcap_libdirs}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Npcap
    REQUIRED_VARS NPCAP_INCLUDE_DIR NPCAP_LIBRARY
    FAIL_MESSAGE
        "Npcap SDK not found. Run bootstrap.ps1, or set NPCAP_SDK_DIR to the \
unpacked SDK (the folder containing Include/ and Lib/)."
)

if(NPCAP_FOUND AND NOT TARGET Npcap::Npcap)
    add_library(Npcap::Npcap UNKNOWN IMPORTED)
    set_target_properties(Npcap::Npcap PROPERTIES
        IMPORTED_LOCATION "${NPCAP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NPCAP_INCLUDE_DIR}"
    )
    if(NPCAP_PACKET_LIBRARY)
        set_target_properties(Npcap::Npcap PROPERTIES
            INTERFACE_LINK_LIBRARIES "${NPCAP_PACKET_LIBRARY}")
    endif()
endif()

mark_as_advanced(NPCAP_INCLUDE_DIR NPCAP_LIBRARY NPCAP_PACKET_LIBRARY)
