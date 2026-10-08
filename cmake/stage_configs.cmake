# Put the default configs beside the executable, without destroying settings.
#
# This replaces `cmake -E copy_directory`, which overwrites unconditionally and
# so threw away every setting the moment anything was rebuilt. That is not a
# theoretical complaint: a channel was chosen in the GUI, the project was built
# again, configs/login.ini went back to channel_index=0, and the bot logged
# into Kênh 1 while the person who set it watched it do so.
#
# A file that is already there is left exactly as it is. The only way to get
# the shipped defaults back is to delete the file, which is the behaviour
# anyone who has edited a config expects.
#
# Call with -DSRC=<configs dir> -DDST=<where they go>.

# Not overwriting is right, but doing it silently is not. Editing a shipped
# default and rebuilding leaves the running copy untouched with no sign that
# anything was ignored -- which costs an afternoon the first time a change to
# configs/protocol.ini appears to have had no effect whatsoever. So a file that
# is kept because it differs is named, with what to do about it.

file(GLOB _config_files RELATIVE "${SRC}" "${SRC}/*")

set(_stale "")

foreach(_name IN LISTS _config_files)
    if(IS_DIRECTORY "${SRC}/${_name}")
        continue()
    endif()
    if(EXISTS "${DST}/${_name}")
        file(SHA256 "${SRC}/${_name}" _src_hash)
        file(SHA256 "${DST}/${_name}" _dst_hash)
        if(NOT _src_hash STREQUAL _dst_hash)
            list(APPEND _stale "${_name}")
        endif()
        continue()
    endif()
    file(COPY "${SRC}/${_name}" DESTINATION "${DST}")
    message(STATUS "staged default config: ${_name}")
endforeach()

if(_stale)
    string(REPLACE ";" ", " _stale_list "${_stale}")
    message(STATUS "configs kept as they are (they differ from the shipped "
                   "defaults): ${_stale_list}")
    message(STATUS "  Your edits there are safe and are what the build output "
                   "will use. If you meant to change the SHIPPED default "
                   "instead, delete the staged copy and build again.")
endif()
