if (NOT DEFINED MOULT_EXE)
    message(FATAL_ERROR "MOULT_EXE is required")
endif()
if (NOT DEFINED FIXTURE)
    message(FATAL_ERROR "FIXTURE is required")
endif()
if (NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "WORK_DIR is required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${FIXTURE}" DESTINATION "${WORK_DIR}")

set(INPUT "${WORK_DIR}/legacy_modernisation.cpp")
file(WRITE "${WORK_DIR}/compile_commands.json"
"[
  {
    \"directory\": \"${WORK_DIR}\",
    \"file\": \"${INPUT}\",
    \"command\": \"c++ -std=c++14 -Iinclude -c legacy_modernisation.cpp -o legacy.o\"
  }
]
")

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter clang --compile-commands "${WORK_DIR}/compile_commands.json" --include-facts "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "moult clang compile database run failed\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if (NOT stdout MATCHES "\"scanner\":\"libclang\"")
    message(FATAL_ERROR "expected libclang facts in stdout")
endif()
if (NOT stdout MATCHES "review-c-style-cast")
    message(FATAL_ERROR "expected C-style cast finding")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter clang --compile-commands "${WORK_DIR}" --include-facts
    RESULT_VARIABLE primary_result
    OUTPUT_VARIABLE primary_stdout
    ERROR_VARIABLE primary_stderr
)
if (NOT primary_result EQUAL 0)
    message(FATAL_ERROR "moult clang primary compile database run failed\nstdout:\n${primary_stdout}\nstderr:\n${primary_stderr}")
endif()
if (NOT primary_stdout MATCHES "\"scanner\":\"libclang\"")
    message(FATAL_ERROR "expected libclang facts in primary compile database stdout")
endif()
if (NOT primary_stdout MATCHES "review-c-style-cast")
    message(FATAL_ERROR "expected C-style cast finding in primary compile database stdout")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter textual --compile-commands "${WORK_DIR}/compile_commands.json" --include-facts
    RESULT_VARIABLE textual_result
    OUTPUT_VARIABLE textual_stdout
    ERROR_VARIABLE textual_stderr
)
if (NOT textual_result EQUAL 0)
    message(FATAL_ERROR "moult textual primary compile database run failed\nstdout:\n${textual_stdout}\nstderr:\n${textual_stderr}")
endif()
if (NOT textual_stdout MATCHES "\"scanner\":\"textual-cpp-modernisation\"")
    message(FATAL_ERROR "expected textual facts in primary compile database stdout")
endif()
