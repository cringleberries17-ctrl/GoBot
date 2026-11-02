# FindNpcap.cmake — the Npcap SDK (headers + wpcap import library), from this
# tree and nowhere else.
#
# The Npcap *runtime* is installed by the Npcap installer into System32; the
# *SDK* (pcap.h, wpcap.lib) is a separate zip. Version 1.13 of it is kept in
# third_party/npcap-sdk, unpacked, so that a build needs no network. What it
# links goes into three programs that run as administrator, so nowhere else
# is looked in -- not C:\npcap-sdk, which any user can make, nor wherever
# NPCAP_SDK_DIR says -- and every header and library in it is held to the
# hash third_party/npcap-sdk.sha256 records: one missing or changed stops the
# configure.
#
# Provides: Npcap::Npcap, NPCAP_FOUND, NPCAP_INCLUDE_DIR, NPCAP_LIBRARY

set(_npcap_dir  "${CMAKE_SOURCE_DIR}/third_party/npcap-sdk")
set(_npcap_sums "${CMAKE_SOURCE_DIR}/third_party/npcap-sdk.sha256")

if(NOT EXISTS "${_npcap_sums}")
    message(FATAL_ERROR "third_party/npcap-sdk.sha256 is missing: the Npcap SDK cannot be checked.")
endif()
file(STRINGS "${_npcap_sums}" _npcap_lines)
set(_npcap_checked 0)
foreach(_line IN LISTS _npcap_lines)
    if(_line MATCHES "^[ \t]*(#|$)")
        continue()
    endif()
    if(NOT _line MATCHES "^([0-9a-f]+)  (.+)$")
        message(FATAL_ERROR "third_party/npcap-sdk.sha256: not a hash and a file: '${_line}'")
    endif()
    set(_want "${CMAKE_MATCH_1}")
    set(_rel  "${CMAKE_MATCH_2}")
    if(NOT EXISTS "${_npcap_dir}/${_rel}")
        message(FATAL_ERROR "third_party/npcap-sdk has no ${_rel}. Restore third_party/npcap-sdk from the repository.")
    endif()
    file(SHA256 "${_npcap_dir}/${_rel}" _have)
    if(NOT _have STREQUAL _want)
        message(FATAL_ERROR "third_party/npcap-sdk/${_rel} is not the file that was committed "
                            "(SHA256 ${_have}, recorded ${_want}). Restore third_party/npcap-sdk "
                            "from the repository.")
    endif()
    # Changed after this configure, it runs the configure -- and this check --
    # again before the next build.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_npcap_dir}/${_rel}")
    math(EXPR _npcap_checked "${_npcap_checked} + 1")
endforeach()
if(_npcap_checked EQUAL 0)
    message(FATAL_ERROR "third_party/npcap-sdk.sha256 lists no files.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_npcap_sums}")

# Fixed paths, no search. A tree configured before this kept what
# find_library found then, wherever that was.
unset(NPCAP_INCLUDE_DIR CACHE)
unset(NPCAP_LIBRARY CACHE)
unset(NPCAP_PACKET_LIBRARY CACHE)
set(NPCAP_INCLUDE_DIR "${_npcap_dir}/Include")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_npcap_libdir "${_npcap_dir}/Lib/x64")
else()
    set(_npcap_libdir "${_npcap_dir}/Lib")
endif()
set(NPCAP_LIBRARY        "${_npcap_libdir}/wpcap.lib")
set(NPCAP_PACKET_LIBRARY "${_npcap_libdir}/Packet.lib")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Npcap REQUIRED_VARS NPCAP_INCLUDE_DIR NPCAP_LIBRARY)

if(NPCAP_FOUND AND NOT TARGET Npcap::Npcap)
    add_library(Npcap::Npcap UNKNOWN IMPORTED)
    set_target_properties(Npcap::Npcap PROPERTIES
        IMPORTED_LOCATION "${NPCAP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${NPCAP_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NPCAP_PACKET_LIBRARY}")
endif()
