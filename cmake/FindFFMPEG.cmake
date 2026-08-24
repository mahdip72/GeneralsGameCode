# Locate the FFmpeg development SDK used by the native video backend.
#
# An SDK root can be supplied with FFMPEG_ROOT. The expected layout is the
# conventional shared-development bundle containing include/, lib/, and bin/.

include(FindPackageHandleStandardArgs)

set(_FFMPEG_COMPONENTS
    avcodec
    avformat
    avutil
    swresample
    swscale
)

set(_FFMPEG_ROOT_HINTS)
if(FFMPEG_ROOT)
    list(APPEND _FFMPEG_ROOT_HINTS "${FFMPEG_ROOT}")
endif()
if(RTS_FFMPEG_ROOT)
    list(APPEND _FFMPEG_ROOT_HINTS "${RTS_FFMPEG_ROOT}")
endif()
if(DEFINED ENV{FFMPEG_ROOT})
    list(APPEND _FFMPEG_ROOT_HINTS "$ENV{FFMPEG_ROOT}")
endif()
if(DEFINED ENV{RTS_FFMPEG_ROOT})
    list(APPEND _FFMPEG_ROOT_HINTS "$ENV{RTS_FFMPEG_ROOT}")
endif()
if(_FFMPEG_ROOT_HINTS)
    set(_FFMPEG_SEARCH_POLICY NO_DEFAULT_PATH)
endif()

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS ${_FFMPEG_ROOT_HINTS}
    PATH_SUFFIXES include
    ${_FFMPEG_SEARCH_POLICY}
)
find_program(FFMPEG_EXECUTABLE
    NAMES ffmpeg
    HINTS ${_FFMPEG_ROOT_HINTS}
    PATH_SUFFIXES bin
    ${_FFMPEG_SEARCH_POLICY}
)

set(_FFMPEG_REQUIRED_VARIABLES FFMPEG_INCLUDE_DIR)
foreach(_FFMPEG_COMPONENT IN LISTS _FFMPEG_COMPONENTS)
    string(TOUPPER "${_FFMPEG_COMPONENT}" _FFMPEG_COMPONENT_UPPER)
    find_library(FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY
        NAMES "${_FFMPEG_COMPONENT}"
        HINTS ${_FFMPEG_ROOT_HINTS}
        PATH_SUFFIXES lib
        ${_FFMPEG_SEARCH_POLICY}
    )
    list(APPEND _FFMPEG_REQUIRED_VARIABLES
        FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY)
endforeach()

find_package_handle_standard_args(FFMPEG
    REQUIRED_VARS ${_FFMPEG_REQUIRED_VARIABLES}
)

if(FFMPEG_FOUND)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES)
    set(FFMPEG_LIBRARY_DIRS)

    if(WIN32 AND NOT FFMPEG_RUNTIME_DIR)
        get_filename_component(_FFMPEG_FIRST_LIBRARY_DIR "${FFMPEG_AVCODEC_LIBRARY}" DIRECTORY)
        get_filename_component(_FFMPEG_SDK_ROOT "${_FFMPEG_FIRST_LIBRARY_DIR}" DIRECTORY)
        if(IS_DIRECTORY "${_FFMPEG_SDK_ROOT}/bin")
            set(FFMPEG_RUNTIME_DIR "${_FFMPEG_SDK_ROOT}/bin" CACHE PATH
                "Directory containing the FFmpeg shared runtime libraries")
        endif()
    endif()

    foreach(_FFMPEG_COMPONENT IN LISTS _FFMPEG_COMPONENTS)
        string(TOUPPER "${_FFMPEG_COMPONENT}" _FFMPEG_COMPONENT_UPPER)
        set(_FFMPEG_LIBRARY "${FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY}")
        list(APPEND FFMPEG_LIBRARIES "${_FFMPEG_LIBRARY}")

        get_filename_component(_FFMPEG_LIBRARY_DIR "${_FFMPEG_LIBRARY}" DIRECTORY)
        list(APPEND FFMPEG_LIBRARY_DIRS "${_FFMPEG_LIBRARY_DIR}")

        if(NOT TARGET FFMPEG::${_FFMPEG_COMPONENT})
            if(WIN32)
                add_library(FFMPEG::${_FFMPEG_COMPONENT} SHARED IMPORTED)
                set_target_properties(FFMPEG::${_FFMPEG_COMPONENT} PROPERTIES
                    IMPORTED_IMPLIB "${_FFMPEG_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
                )

                if(FFMPEG_RUNTIME_DIR)
                    file(GLOB _FFMPEG_RUNTIME_LIBRARY LIST_DIRECTORIES FALSE
                        "${FFMPEG_RUNTIME_DIR}/${_FFMPEG_COMPONENT}-*.dll")
                    list(LENGTH _FFMPEG_RUNTIME_LIBRARY _FFMPEG_RUNTIME_LIBRARY_COUNT)
                    if(_FFMPEG_RUNTIME_LIBRARY_COUNT EQUAL 1)
                        list(GET _FFMPEG_RUNTIME_LIBRARY 0 _FFMPEG_RUNTIME_LIBRARY_PATH)
                        set_property(TARGET FFMPEG::${_FFMPEG_COMPONENT} PROPERTY
                            IMPORTED_LOCATION "${_FFMPEG_RUNTIME_LIBRARY_PATH}")
                    endif()
                endif()
            else()
                add_library(FFMPEG::${_FFMPEG_COMPONENT} UNKNOWN IMPORTED)
                set_target_properties(FFMPEG::${_FFMPEG_COMPONENT} PROPERTIES
                    IMPORTED_LOCATION "${_FFMPEG_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
                )
            endif()
        endif()
    endforeach()

    list(REMOVE_DUPLICATES FFMPEG_LIBRARY_DIRS)
endif()

mark_as_advanced(
    FFMPEG_INCLUDE_DIR
    FFMPEG_AVCODEC_LIBRARY
    FFMPEG_AVFORMAT_LIBRARY
    FFMPEG_AVUTIL_LIBRARY
    FFMPEG_SWRESAMPLE_LIBRARY
    FFMPEG_SWSCALE_LIBRARY
    FFMPEG_RUNTIME_DIR
    FFMPEG_EXECUTABLE
)

unset(_FFMPEG_COMPONENT)
unset(_FFMPEG_COMPONENT_UPPER)
unset(_FFMPEG_COMPONENTS)
unset(_FFMPEG_LIBRARY)
unset(_FFMPEG_LIBRARY_DIR)
unset(_FFMPEG_FIRST_LIBRARY_DIR)
unset(_FFMPEG_REQUIRED_VARIABLES)
unset(_FFMPEG_ROOT_HINTS)
unset(_FFMPEG_RUNTIME_LIBRARY)
unset(_FFMPEG_RUNTIME_LIBRARY_COUNT)
unset(_FFMPEG_RUNTIME_LIBRARY_PATH)
unset(_FFMPEG_SEARCH_POLICY)
unset(_FFMPEG_SDK_ROOT)
