# protocol.json - JSON provider for shield.protocol.codec.v1 with optional
# payload schema validation.

add_library(shield_protocol_json_obj OBJECT
    shield_protocol_json.cpp
    ${PROJECT_SOURCE_DIR}/src/plugin/schema_validator.cpp
)
set_target_properties(shield_protocol_json_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_protocol_json_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/plugin
        ${PROJECT_SOURCE_DIR}/plugins/_shared
)

target_link_libraries(shield_protocol_json_obj
    PRIVATE
        nlohmann_json::nlohmann_json
)

add_library(shield_protocol_json MODULE
    $<TARGET_OBJECTS:shield_protocol_json_obj>
)

target_link_libraries(shield_protocol_json
    PRIVATE
        nlohmann_json::nlohmann_json
)

set_target_properties(shield_protocol_json PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_protocol_json"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.json/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.json")

message(STATUS "  + protocol.json: JSON codec provider (v1 ABI)")
