# Helper macro for querying Windows registry values
# Queries both 32-bit and 64-bit registry views automatically
macro(fetch_registry_value registry_key registry_value output_var description)
    # Registry discovery always writes to a registry-only cache variable.  The
    # product CMakeLists decide whether that value is an effective Win32
    # fallback; an explicit RTS_INSTALL_PREFIX_* value is never overwritten.
    if(NOT DEFINED ${output_var} OR "${${output_var}}" STREQUAL "")
        cmake_host_system_information(RESULT _variable
            QUERY WINDOWS_REGISTRY
            "${registry_key}"
            VALUE "${registry_value}"
            VIEW 32_64)
        if(_variable)
            set(${output_var} "${_variable}" CACHE PATH "${description}")
        endif()
    endif()
endmacro()

