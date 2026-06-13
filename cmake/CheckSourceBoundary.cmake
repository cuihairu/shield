set(core_paths
  "${PROJECT_SOURCE_DIR}/include/shield/core"
  "${PROJECT_SOURCE_DIR}/src/core"
)

set(public_header_paths
  "${PROJECT_SOURCE_DIR}/include/shield/core"
  "${PROJECT_SOURCE_DIR}/include/shield/shield.hpp"
)

set(public_caf_allowed_files
  "${PROJECT_SOURCE_DIR}/include/shield/core/caf_adapter.hpp"
  "${PROJECT_SOURCE_DIR}/include/shield/caf_initializer.hpp"
)

set(forbidden_core_includes
  "shield/lua/"
  "shield/net/"
  "shield/data/"
  "shield/database/"
  "shield/config/"
  "shield/log/"
  "shield/gateway/"
  "shield/discovery/"
  "shield/events/"
  "shield/extensions/"
  "shield/http/"
  "shield/script/"
  "shield/service/"
)

set(violations "")

function(is_allowed_public_caf_header source_file result_var)
  set(is_allowed FALSE)
  file(TO_CMAKE_PATH "${source_file}" normalized_source)
  foreach(allowed IN LISTS public_caf_allowed_files)
    file(TO_CMAKE_PATH "${allowed}" normalized_allowed)
    if(normalized_source STREQUAL normalized_allowed)
      set(is_allowed TRUE)
    endif()
  endforeach()
  set(${result_var} ${is_allowed} PARENT_SCOPE)
endfunction()

foreach(path IN LISTS core_paths)
  if(EXISTS "${path}")
    file(GLOB_RECURSE source_files
      "${path}/*.hpp"
      "${path}/*.h"
      "${path}/*.cpp"
      "${path}/*.cc"
    )

    foreach(source_file IN LISTS source_files)
      file(READ "${source_file}" content)
      foreach(forbidden IN LISTS forbidden_core_includes)
        if(content MATCHES "#[ \t]*include[ \t]*[<\"]${forbidden}")
          string(APPEND violations
            "${source_file}: forbidden shield_core include '${forbidden}'\n")
        endif()
      endforeach()
    endforeach()
  endif()
endforeach()

foreach(path IN LISTS public_header_paths)
  if(EXISTS "${path}")
    file(GLOB_RECURSE public_headers
      "${path}/*.hpp"
      "${path}/*.h"
    )

    foreach(public_header IN LISTS public_headers)
      is_allowed_public_caf_header("${public_header}" allow_caf)
      if(allow_caf)
        continue()
      endif()

      file(READ "${public_header}" content)
      if(content MATCHES "#[ \t]*include[ \t]*[<\"]caf/")
        string(APPEND violations
          "${public_header}: public header must not include CAF headers\n")
      endif()
      if(content MATCHES "(^|[^A-Za-z0-9_:])caf::")
        string(APPEND violations
          "${public_header}: public header must not expose caf:: types\n")
      endif()
    endforeach()
  endif()
endforeach()

if(violations)
  message(FATAL_ERROR "Source boundary check failed:\n${violations}")
endif()

message(STATUS "Source boundary check passed")
