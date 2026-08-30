add_executable(core_renderer_contract_tests
    RendererContractTest.cpp
)

target_link_libraries(core_renderer_contract_tests PRIVATE
    core_renderer
    d3d8lib
)

target_include_directories(core_renderer_contract_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Core/GameEngine/Include
    ${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include
    ${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas
    ${CMAKE_SOURCE_DIR}/Dependencies/Utility
)

if(IS_VS6_BUILD)
    set_target_properties(core_renderer_contract_tests PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
else()
    set_target_properties(core_renderer_contract_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()

add_test(NAME core_renderer_contract_tests
    COMMAND core_renderer_contract_tests
)
set_tests_properties(core_renderer_contract_tests PROPERTIES TIMEOUT 30)
