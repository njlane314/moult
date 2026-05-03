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
    \"command\": \"c++ -std=c++14 -c ${INPUT} -o ${WORK_DIR}/legacy.o\"
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
