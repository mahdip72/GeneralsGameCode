add_executable(core_renderer_contract_tests
    RendererContractTest.cpp
)

target_link_libraries(core_renderer_contract_tests PRIVATE
    core_renderer
)

add_test(NAME core_renderer_contract_tests
    COMMAND core_renderer_contract_tests
)
set_tests_properties(core_renderer_contract_tests PROPERTIES TIMEOUT 30)
