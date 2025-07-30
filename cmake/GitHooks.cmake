# Git Hooks and Code Formatting Module
# This module handles automatic installation of git hooks and provides formatting targets

# Find clang-format
find_program(CLANG_FORMAT_EXE NAMES "clang-format" "clang-format-15" "clang-format-14")
if(CLANG_FORMAT_EXE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXE}")
    
    # Option to control git hooks installation
    option(INSTALL_GIT_HOOKS "Install git hooks automatically during configuration" ON)
    
    # Auto-install git hooks during configuration
    if(INSTALL_GIT_HOOKS AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        set(HOOKS_DIR "${CMAKE_SOURCE_DIR}/.git/hooks")
        set(PRE_COMMIT_HOOK "${HOOKS_DIR}/pre-commit")
        
        if(NOT EXISTS "${PRE_COMMIT_HOOK}")
            message(STATUS "Installing git pre-commit hook...")
            
            # Create the pre-commit hook content
            set(HOOK_CONTENT "#!/bin/bash
# Auto-generated pre-commit hook by CMake

files=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\\.(cpp|c|h|hpp|cc|cxx)$')

if [ ! -z \"$files\" ]; then
    echo \"Running clang-format on staged files...\"
    
    if ! command -v ${CLANG_FORMAT_EXE} &> /dev/null; then
        echo \"Error: clang-format not found at ${CLANG_FORMAT_EXE}\"
        exit 1
    fi
    
    echo \"$files\" | xargs ${CLANG_FORMAT_EXE} -i
    echo \"$files\" | xargs git add
    
    echo \"Code formatting completed.\"
fi

exit 0")
            
            # Write and make executable
            file(WRITE "${PRE_COMMIT_HOOK}" "${HOOK_CONTENT}")
            if(UNIX)
                execute_process(COMMAND chmod +x "${PRE_COMMIT_HOOK}")
            endif()
            
            message(STATUS "✓ Git pre-commit hook installed successfully!")
        else()
            message(STATUS "Git pre-commit hook already exists, skipping installation")
        endif()
    elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/.git")
        message(STATUS "Not a git repository, skipping git hooks installation")
    endif()
    
    # Add formatting targets for manual use
    file(GLOB_RECURSE ALL_SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.hpp" 
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.hpp"
        "${CMAKE_SOURCE_DIR}/tests/*.h"
        "${CMAKE_SOURCE_DIR}/examples/*.cpp"
        "${CMAKE_SOURCE_DIR}/examples/*.hpp"
        "${CMAKE_SOURCE_DIR}/examples/*.h"
    )
    
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_SOURCE_FILES}
        COMMENT "Formatting all source files with clang-format"
        VERBATIM
    )
    
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALL_SOURCE_FILES}
        COMMENT "Checking code formatting with clang-format (fails on formatting issues)"
        VERBATIM
    )
    
    message(STATUS "Available formatting targets:")
    message(STATUS "  make format       - Format all source files")
    message(STATUS "  make format-check - Check formatting without modifying files")
    
else()
    message(WARNING "clang-format not found - code formatting features unavailable")
    message(STATUS "Please install clang-format to enable automatic code formatting")
endif()