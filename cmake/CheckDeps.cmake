# [SHIELD] Dependency checking for shield_core
# This script verifies that shield_core does not depend on forbidden libraries

function(check_shield_core_deps target)
    message(STATUS "Checking shield_core dependencies...")

    get_target_property(link_libs ${target} LINK_LIBRARIES)

    # Forbidden libraries for shield_core
    set(forbidden
        "Lua::Lua"
        "sol2"
        "Boost::asio"
        "Boost::beast"
        "hiredis"
        "redis++"
        "yaml-cpp"
        "Boost::log"
        "Prometheus"
    )

    foreach(lib ${link_libs})
        foreach(bad ${forbidden})
            string(FIND "${lib}" "${bad}" bad_pos)
            if(NOT bad_pos EQUAL -1)
                message(FATAL_ERROR
                    "shield_core MUST NOT link ${bad}. "
                    "This violates the clean architecture principle.")
            endif()
        endforeach()
    endforeach()

    message(STATUS "shield_core dependency check: PASSED")
endfunction()

# Call the check
if(TARGET shield_core)
    check_shield_core_deps(shield_core)
endif()
