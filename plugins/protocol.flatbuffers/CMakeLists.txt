# protocol.flatbuffers — FlatBuffers provider for shield.protocol.codec.v1.

find_package(flatbuffers CONFIG REQUIRED)

add_library(shield_protocol_flatbuffers_obj OBJECT shield_protocol_flatbuffers.cpp)
set_target_properties(shield_protocol_flatbuffers_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_protocol_flatbuffers_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(shield_protocol_flatbuffers_obj
    PRIVATE
        flatbuffers::flatbuffers
        nlohmann_json::nlohmann_json
)

add_library(shield_protocol_flatbuffers MODULE
    $<TARGET_OBJECTS:shield_protocol_flatbuffers_obj>
)

target_link_libraries(shield_protocol_flatbuffers
    PRIVATE
        flatbuffers::flatbuffers
        nlohmann_json::nlohmann_json
)

set_target_properties(shield_protocol_flatbuffers PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_protocol_flatbuffers"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.flatbuffers/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.flatbuffers")

message(STATUS "  + protocol.flatbuffers: FlatBuffers codec provider (v1 ABI)")
