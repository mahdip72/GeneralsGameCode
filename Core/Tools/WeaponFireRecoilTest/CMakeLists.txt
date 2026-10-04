add_executable(core_weapon_fire_recoil_tests WeaponFireRecoilTest.cpp)
target_include_directories(core_weapon_fire_recoil_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/Core/GameEngineDevice/Include"
)
set_target_properties(core_weapon_fire_recoil_tests PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MINGW)
    target_link_options(core_weapon_fire_recoil_tests PRIVATE -mconsole)
endif()
add_test(NAME core_weapon_fire_recoil_tests COMMAND core_weapon_fire_recoil_tests)
set_tests_properties(core_weapon_fire_recoil_tests PROPERTIES TIMEOUT 30)
