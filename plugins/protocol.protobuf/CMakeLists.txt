# protocol.protobuf — Protobuf provider for shield.protocol.codec.v1.

find_package(Protobuf CONFIG REQUIRED)

add_library(shield_protocol_protobuf_obj OBJECT shield_protocol_protobuf.cpp)
set_target_properties(shield_protocol_protobuf_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_protocol_protobuf_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(shield_protocol_protobuf_obj
    PRIVATE
        protobuf::libprotobuf
        nlohmann_json::nlohmann_json
)

add_library(shield_protocol_protobuf MODULE
    $<TARGET_OBJECTS:shield_protocol_protobuf_obj>
)

target_link_libraries(shield_protocol_protobuf
    PRIVATE
        protobuf::libprotobuf
        nlohmann_json::nlohmann_json
)

set_target_properties(shield_protocol_protobuf PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_protocol_protobuf"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.protobuf/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.protobuf")

message(STATUS "  + protocol.protobuf: Protobuf codec provider (v1 ABI)")
