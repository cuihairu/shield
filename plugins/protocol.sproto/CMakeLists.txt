# protocol.sproto — Sproto provider for shield.protocol.codec.v1.

find_package(sproto CONFIG REQUIRED)

add_library(shield_protocol_sproto_obj OBJECT shield_protocol_sproto.cpp)
set_target_properties(shield_protocol_sproto_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_protocol_sproto_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(shield_protocol_sproto_obj
    PRIVATE
        sproto::sproto
        nlohmann_json::nlohmann_json
)

add_library(shield_protocol_sproto MODULE
    $<TARGET_OBJECTS:shield_protocol_sproto_obj>
)

target_link_libraries(shield_protocol_sproto
    PRIVATE
        sproto::sproto
        nlohmann_json::nlohmann_json
)

set_target_properties(shield_protocol_sproto PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_protocol_sproto"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.sproto/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/protocol.sproto")

message(STATUS "  + protocol.sproto: Sproto codec provider (v1 ABI)")
