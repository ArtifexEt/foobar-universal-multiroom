cmake_minimum_required(VERSION 3.24)

project(FoobarUniversalMultiroom LANGUAGES CXX)

add_executable(MultiroomTransportContractProbe
    src/transport_contract_probe.cpp
)

target_compile_features(MultiroomTransportContractProbe PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(MultiroomTransportContractProbe PRIVATE /W4 /permissive-)
endif()
