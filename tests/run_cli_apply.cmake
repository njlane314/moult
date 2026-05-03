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

set(INPUT "${WORK_DIR}/legacy_modernization.cpp")
set(OUTPUT_DIR "${WORK_DIR}/out")

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter textual --out "${OUTPUT_DIR}" "${INPUT}"
    RESULT_VARIABLE plan_result
    OUTPUT_VARIABLE plan_stdout
    ERROR_VARIABLE plan_stderr
)
if (NOT plan_result EQUAL 0)
    message(FATAL_ERROR "moult plan failed\nstdout:\n${plan_stdout}\nstderr:\n${plan_stderr}")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" apply --backup "${OUTPUT_DIR}/plan.json"
    RESULT_VARIABLE apply_result
    OUTPUT_VARIABLE apply_stdout
    ERROR_VARIABLE apply_stderr
)
if (NOT apply_result EQUAL 0)
    message(FATAL_ERROR "moult apply failed\nstdout:\n${apply_stdout}\nstderr:\n${apply_stderr}")
endif()

file(READ "${INPUT}" applied_text)
if (NOT applied_text MATCHES "noexcept")
    message(FATAL_ERROR "expected noexcept in applied file")
endif()
if (NOT applied_text MATCHES "nullptr")
    message(FATAL_ERROR "expected nullptr in applied file")
endif()
if (applied_text MATCHES "register int value")
    message(FATAL_ERROR "expected register keyword to be removed")
endif()
if (NOT EXISTS "${INPUT}.moult.bak")
    message(FATAL_ERROR "expected backup file")
endif()
