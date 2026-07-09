# protocol.msgpack - MessagePack provider for shield.protocol.codec.v1.

add_library(shield_protocol_msgpack_obj OBJECT shield_protocol_msgpack.cpp)
set_target_properties(shield_protocol_msgpack_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_protocol_msgpack_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(shield_protocol_msgpack_obj
    PRIVATE
        nlohmann_json::nlohmann_json
)

add_library(shield_protocol_msgpack MODULE
    $<TARGET_OBJECTS:shield_protocol_msgpack_obj>
)

target_link_libraries(shield_protocol_msgpack
    PRIVATE
        nlohmann_json::nlohmann_json
)

set_target_properties(shield_protocol_msgpack PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_protocol_msgpack"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.msgpack/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.msgpack")

message(STATUS "  + protocol.msgpack: MessagePack codec provider (v1 ABI)")
