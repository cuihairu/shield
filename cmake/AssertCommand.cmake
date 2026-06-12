if(NOT DEFINED COMMAND_UNDER_TEST)
  message(FATAL_ERROR "COMMAND_UNDER_TEST is required")
endif()

if(NOT DEFINED EXPECTED_EXIT_CODE)
  set(EXPECTED_EXIT_CODE 0)
endif()

execute_process(
  COMMAND ${COMMAND_UNDER_TEST}
  RESULT_VARIABLE actual_exit_code
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr
)

if(NOT actual_exit_code EQUAL EXPECTED_EXIT_CODE)
  message(FATAL_ERROR
    "Expected exit code ${EXPECTED_EXIT_CODE}, got ${actual_exit_code}\n"
    "stdout:\n${actual_stdout}\n"
    "stderr:\n${actual_stderr}"
  )
endif()

if(DEFINED EXPECTED_OUTPUT_REGEX)
  string(CONCAT combined_output "${actual_stdout}" "${actual_stderr}")
  if(NOT combined_output MATCHES "${EXPECTED_OUTPUT_REGEX}")
    message(FATAL_ERROR
      "Expected output to match '${EXPECTED_OUTPUT_REGEX}'\n"
      "stdout:\n${actual_stdout}\n"
      "stderr:\n${actual_stderr}"
    )
  endif()
endif()
