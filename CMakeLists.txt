cmake_minimum_required(VERSION 3.24)

project(FoobarUniversalMultiroom LANGUAGES CXX)

add_library(MultiroomCore
    components/foo_out_multiroom_bridge/core/output_registry.cpp
    components/foo_out_multiroom_bridge/core/sync_clock.cpp
    transports/airplay/airplay_transport.cpp
)

target_compile_features(MultiroomCore PUBLIC cxx_std_20)
target_include_directories(MultiroomCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(MultiroomTransportContractProbe
    src/transport_contract_probe.cpp
)

target_compile_features(MultiroomTransportContractProbe PRIVATE cxx_std_20)
target_link_libraries(MultiroomTransportContractProbe PRIVATE MultiroomCore)

if(MSVC)
    target_compile_options(MultiroomCore PRIVATE /W4 /permissive-)
    target_compile_options(MultiroomTransportContractProbe PRIVATE /W4 /permissive-)
endif()
