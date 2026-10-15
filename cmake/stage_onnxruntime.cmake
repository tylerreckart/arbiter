# Copy ONNX Runtime's SONAME library (and its symlinks) next to a binary
# so an $ORIGIN / @loader_path runtime path can load it. Invoked as:
#   cmake -DSRC=<libdir> -DDEST=<dir> -P cmake/stage_onnxruntime.cmake
if(NOT SRC OR NOT DEST)
    message(FATAL_ERROR "stage_onnxruntime: SRC and DEST are required")
endif()
file(MAKE_DIRECTORY "${DEST}")
file(GLOB _libs
    "${SRC}/libonnxruntime.so*"
    "${SRC}/libonnxruntime*.dylib")
foreach(_lib IN LISTS _libs)
    get_filename_component(_name "${_lib}" NAME)
    set(_dest "${DEST}/${_name}")
    if(IS_SYMLINK "${_lib}")
        file(READ_SYMLINK "${_lib}" _target)
        file(REMOVE "${_dest}")
        file(CREATE_LINK "${_target}" "${_dest}" SYMBOLIC)
    else()
        file(COPY_FILE "${_lib}" "${_dest}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
