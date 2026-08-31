add_executable(core_chunkio_value_read_tests
    ChunkIOValueReadTest.cpp
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib/chunkio.cpp"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WWLib/ramfile.cpp"
)

target_include_directories(core_chunkio_value_read_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas"
    "${CMAKE_SOURCE_DIR}/Dependencies/Utility"
    "${CMAKE_SOURCE_DIR}/Core/Libraries/Include"
)

target_precompile_headers(core_chunkio_value_read_tests PRIVATE
    [["Utility/CppMacros.h"]]
)

set_target_properties(core_chunkio_value_read_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_test(NAME core_chunkio_value_read_tests
    COMMAND core_chunkio_value_read_tests
)
set_tests_properties(core_chunkio_value_read_tests PROPERTIES TIMEOUT 30)
