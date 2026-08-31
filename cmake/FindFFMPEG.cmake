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

list(REMOVE_DUPLICATES _FFMPEG_ROOT_HINTS)
string(SHA256 _FFMPEG_ROOT_HINT_SIGNATURE "${_FFMPEG_ROOT_HINTS}")
if(DEFINED RTS_FFMPEG_ROOT_HINT_SIGNATURE AND
        NOT RTS_FFMPEG_ROOT_HINT_SIGNATURE STREQUAL _FFMPEG_ROOT_HINT_SIGNATURE)
    unset(FFMPEG_INCLUDE_DIR CACHE)
    unset(FFMPEG_EXECUTABLE CACHE)
    foreach(_FFMPEG_COMPONENT IN LISTS _FFMPEG_COMPONENTS)
        string(TOUPPER "${_FFMPEG_COMPONENT}" _FFMPEG_COMPONENT_UPPER)
        unset(FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY CACHE)
    endforeach()
endif()
set(RTS_FFMPEG_ROOT_HINT_SIGNATURE "${_FFMPEG_ROOT_HINT_SIGNATURE}" CACHE INTERNAL
    "FFmpeg root-hint signature used to invalidate resolved SDK paths" FORCE)

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
	set(_FFMPEG_PREVIOUS_SDK_ROOT "${FFMPEG_SDK_ROOT}")
	# The public include directory is common to release and debug libraries.
	# Deriving the SDK root from a selected import library incorrectly produces
	# <sdk>/debug for multi-configuration package layouts such as vcpkg.
	get_filename_component(_FFMPEG_SDK_ROOT "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
	set(FFMPEG_SDK_ROOT "${_FFMPEG_SDK_ROOT}" CACHE PATH
		"Resolved root of the FFmpeg development SDK" FORCE)

	# Validate the actual COFF machine records from each import library.  Root
	# directory names are not an architecture contract and are intentionally not
	# consulted here.
	if(WIN32)
		find_program(_RTS_FFMPEG_POWERSHELL NAMES powershell pwsh)
		if(NOT _RTS_FFMPEG_POWERSHELL)
			message(FATAL_ERROR "PowerShell is required to validate FFmpeg COFF import-library architecture.")
		endif()
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			set(_RTS_FFMPEG_EXPECTED_MACHINE 34404)
		elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
			set(_RTS_FFMPEG_EXPECTED_MACHINE 332)
		else()
			message(FATAL_ERROR "Unsupported pointer width for FFmpeg COFF validation: ${CMAKE_SIZEOF_VOID_P}")
		endif()
		foreach(_FFMPEG_COMPONENT IN LISTS _FFMPEG_COMPONENTS)
			string(TOUPPER "${_FFMPEG_COMPONENT}" _FFMPEG_COMPONENT_UPPER)
			execute_process(
				COMMAND "${_RTS_FFMPEG_POWERSHELL}" -NoProfile -NonInteractive
					-ExecutionPolicy Bypass -File "${CMAKE_CURRENT_LIST_DIR}/ValidateFFmpegCoff.ps1"
					-Path "${FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY}"
					-ExpectedMachine "${_RTS_FFMPEG_EXPECTED_MACHINE}"
				RESULT_VARIABLE _RTS_FFMPEG_COFF_RESULT
				OUTPUT_VARIABLE _RTS_FFMPEG_COFF_OUTPUT
				ERROR_VARIABLE _RTS_FFMPEG_COFF_ERROR)
			if(NOT _RTS_FFMPEG_COFF_RESULT EQUAL 0)
				message(FATAL_ERROR "FFmpeg ${_FFMPEG_COMPONENT} import-library COFF validation failed: ${_RTS_FFMPEG_COFF_OUTPUT}${_RTS_FFMPEG_COFF_ERROR}")
			endif()
		endforeach()
	endif()

	set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
	set(FFMPEG_LIBRARIES "")
	set(FFMPEG_LIBRARY_DIRS "")
	set(RTS_FFMPEG_RUNTIME_DLLS "")

    if(WIN32)
        set(_FFMPEG_AUTO_RUNTIME_DIR "${FFMPEG_SDK_ROOT}/bin")
        set(_FFMPEG_USE_AUTO_RUNTIME_DIR FALSE)
        get_property(_FFMPEG_RUNTIME_RESOLVED_THIS_CONFIGURE GLOBAL
            PROPERTY RTS_FFMPEG_RUNTIME_RESOLVED_THIS_CONFIGURE)
        if(_FFMPEG_RUNTIME_RESOLVED_THIS_CONFIGURE AND FFMPEG_RUNTIME_DIR)
            if(RTS_FFMPEG_AUTO_RUNTIME_DIR)
                set(_FFMPEG_USE_AUTO_RUNTIME_DIR TRUE)
            endif()
        elseif(NOT FFMPEG_RUNTIME_DIR)
            set(_FFMPEG_USE_AUTO_RUNTIME_DIR TRUE)
        elseif(RTS_FFMPEG_AUTO_RUNTIME_DIR)
            set(_FFMPEG_CURRENT_RUNTIME_DIR "${FFMPEG_RUNTIME_DIR}")
            set(_FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR "${RTS_FFMPEG_AUTO_RUNTIME_DIR}")
            cmake_path(NORMAL_PATH _FFMPEG_CURRENT_RUNTIME_DIR)
            cmake_path(NORMAL_PATH _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
            string(TOLOWER "${_FFMPEG_CURRENT_RUNTIME_DIR}" _FFMPEG_CURRENT_RUNTIME_DIR)
            string(TOLOWER "${_FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR}" _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
            if(_FFMPEG_CURRENT_RUNTIME_DIR STREQUAL _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
                set(_FFMPEG_USE_AUTO_RUNTIME_DIR TRUE)
            endif()
        elseif(_FFMPEG_PREVIOUS_SDK_ROOT)
            set(_FFMPEG_CURRENT_RUNTIME_DIR "${FFMPEG_RUNTIME_DIR}")
            set(_FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR "${_FFMPEG_PREVIOUS_SDK_ROOT}/bin")
            cmake_path(NORMAL_PATH _FFMPEG_CURRENT_RUNTIME_DIR)
            cmake_path(NORMAL_PATH _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
            string(TOLOWER "${_FFMPEG_CURRENT_RUNTIME_DIR}" _FFMPEG_CURRENT_RUNTIME_DIR)
            string(TOLOWER "${_FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR}" _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
            if(_FFMPEG_CURRENT_RUNTIME_DIR STREQUAL _FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
                set(_FFMPEG_USE_AUTO_RUNTIME_DIR TRUE)
            endif()
        endif()
        if(_FFMPEG_USE_AUTO_RUNTIME_DIR)
            set(FFMPEG_RUNTIME_DIR "${_FFMPEG_AUTO_RUNTIME_DIR}" CACHE PATH
                "Directory containing the FFmpeg shared runtime libraries" FORCE)
            set(RTS_FFMPEG_AUTO_RUNTIME_DIR "${_FFMPEG_AUTO_RUNTIME_DIR}" CACHE INTERNAL
                "Last automatically selected FFmpeg runtime directory" FORCE)
        else()
            if(IS_DIRECTORY "${FFMPEG_RUNTIME_DIR}")
                file(REAL_PATH "${FFMPEG_RUNTIME_DIR}" _FFMPEG_CUSTOM_RUNTIME_DIR)
            else()
                set(_FFMPEG_CUSTOM_RUNTIME_DIR "${FFMPEG_RUNTIME_DIR}")
            endif()
            set(FFMPEG_RUNTIME_DIR "${_FFMPEG_CUSTOM_RUNTIME_DIR}" CACHE PATH
                "Directory containing the FFmpeg shared runtime libraries" FORCE)
            unset(RTS_FFMPEG_AUTO_RUNTIME_DIR CACHE)
        endif()
        if(NOT IS_DIRECTORY "${FFMPEG_RUNTIME_DIR}")
            message(FATAL_ERROR
                "FFmpeg runtime directory does not exist: ${FFMPEG_RUNTIME_DIR}")
        endif()
        set_property(GLOBAL PROPERTY RTS_FFMPEG_RUNTIME_RESOLVED_THIS_CONFIGURE TRUE)
    endif()

    foreach(_FFMPEG_COMPONENT IN LISTS _FFMPEG_COMPONENTS)
        string(TOUPPER "${_FFMPEG_COMPONENT}" _FFMPEG_COMPONENT_UPPER)
        set(_FFMPEG_LIBRARY "${FFMPEG_${_FFMPEG_COMPONENT_UPPER}_LIBRARY}")
        list(APPEND FFMPEG_LIBRARIES "${_FFMPEG_LIBRARY}")

        get_filename_component(_FFMPEG_LIBRARY_DIR "${_FFMPEG_LIBRARY}" DIRECTORY)
        list(APPEND FFMPEG_LIBRARY_DIRS "${_FFMPEG_LIBRARY_DIR}")

        if(WIN32)
            if(NOT TARGET FFMPEG::${_FFMPEG_COMPONENT})
                add_library(FFMPEG::${_FFMPEG_COMPONENT} SHARED IMPORTED)
            endif()
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
                    list(APPEND RTS_FFMPEG_RUNTIME_DLLS "${_FFMPEG_RUNTIME_LIBRARY_PATH}")
                else()
                    message(FATAL_ERROR
                        "FFmpeg runtime directory must contain exactly one ${_FFMPEG_COMPONENT}-*.dll: ${FFMPEG_RUNTIME_DIR}")
                endif()
            endif()
        else()
            if(NOT TARGET FFMPEG::${_FFMPEG_COMPONENT})
                add_library(FFMPEG::${_FFMPEG_COMPONENT} UNKNOWN IMPORTED)
            endif()
            set_target_properties(FFMPEG::${_FFMPEG_COMPONENT} PROPERTIES
                IMPORTED_LOCATION "${_FFMPEG_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
            )
        endif()
    endforeach()

	list(REMOVE_DUPLICATES FFMPEG_LIBRARY_DIRS)
	if(WIN32 AND RTS_FFMPEG_RUNTIME_DLLS)
		set(_FFMPEG_RUNTIME_CLOSURE_FILE
			"${CMAKE_BINARY_DIR}/CMakeFiles/RTSFFmpegRuntimeClosure.txt")
		execute_process(
			COMMAND "${CMAKE_COMMAND}"
				"-DROOT_DLLS:STRING=${RTS_FFMPEG_RUNTIME_DLLS}"
				"-DRUNTIME_DIR:PATH=${FFMPEG_RUNTIME_DIR}"
				"-DOUTPUT_FILE:FILEPATH=${_FFMPEG_RUNTIME_CLOSURE_FILE}"
				-P "${CMAKE_CURRENT_LIST_DIR}/ResolveWindowsRuntimeClosure.cmake"
			RESULT_VARIABLE _FFMPEG_RUNTIME_CLOSURE_RESULT
			OUTPUT_VARIABLE _FFMPEG_RUNTIME_CLOSURE_OUTPUT
			ERROR_VARIABLE _FFMPEG_RUNTIME_CLOSURE_ERROR)
		if(NOT _FFMPEG_RUNTIME_CLOSURE_RESULT EQUAL 0)
			message(FATAL_ERROR
				"FFmpeg runtime dependency closure failed: ${_FFMPEG_RUNTIME_CLOSURE_OUTPUT}${_FFMPEG_RUNTIME_CLOSURE_ERROR}")
		endif()
		file(READ "${_FFMPEG_RUNTIME_CLOSURE_FILE}" RTS_FFMPEG_RUNTIME_DLLS)
	endif()
	list(REMOVE_DUPLICATES RTS_FFMPEG_RUNTIME_DLLS)
	set(RTS_FFMPEG_RUNTIME_DLLS "${RTS_FFMPEG_RUNTIME_DLLS}" CACHE INTERNAL
		"Exact FFmpeg runtime dependency closure selected for installation" FORCE)
endif()

mark_as_advanced(
	FFMPEG_INCLUDE_DIR
	FFMPEG_SDK_ROOT
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
unset(_FFMPEG_PREVIOUS_SDK_ROOT)
unset(_FFMPEG_REQUIRED_VARIABLES)
unset(_FFMPEG_ROOT_HINTS)
unset(_FFMPEG_ROOT_HINT)
unset(_FFMPEG_RUNTIME_LIBRARY)
unset(_FFMPEG_RUNTIME_LIBRARY_COUNT)
unset(_FFMPEG_RUNTIME_LIBRARY_PATH)
unset(_FFMPEG_RUNTIME_RESOLVED_THIS_CONFIGURE)
unset(_FFMPEG_RUNTIME_CLOSURE_ERROR)
unset(_FFMPEG_RUNTIME_CLOSURE_FILE)
unset(_FFMPEG_RUNTIME_CLOSURE_OUTPUT)
unset(_FFMPEG_RUNTIME_CLOSURE_RESULT)
unset(_FFMPEG_AUTO_RUNTIME_DIR)
unset(_FFMPEG_CURRENT_RUNTIME_DIR)
unset(_FFMPEG_CUSTOM_RUNTIME_DIR)
unset(_FFMPEG_PREVIOUS_AUTO_RUNTIME_DIR)
unset(_FFMPEG_USE_AUTO_RUNTIME_DIR)
unset(_FFMPEG_SEARCH_POLICY)
unset(_FFMPEG_SDK_ROOT)
unset(_FFMPEG_ROOT_HINT_SIGNATURE)
