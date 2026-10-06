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

file(GLOB _config_files RELATIVE "${SRC}" "${SRC}/*")

foreach(_name IN LISTS _config_files)
    if(IS_DIRECTORY "${SRC}/${_name}")
        continue()
    endif()
    if(EXISTS "${DST}/${_name}")
        continue()
    endif()
    file(COPY "${SRC}/${_name}" DESTINATION "${DST}")
    message(STATUS "staged default config: ${_name}")
endforeach()
