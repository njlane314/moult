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
file(WRITE "${WORK_DIR}/legacy_c.c" "void c_file(void) { void* p = NULL; }\n")
file(WRITE "${WORK_DIR}/legacy_c_api.h" "#ifdef __cplusplus\nextern \"C\" {\n#endif\n#define LEGACY_C_NULL NULL\n#ifdef __cplusplus\n}\n#endif\n")
file(WRITE "${WORK_DIR}/legacy_c_private.h" "#include <stdint.h>\nstatic void* legacy_c_null(void) { return NULL; }\n")
file(MAKE_DIRECTORY "${WORK_DIR}/c_lib")
file(WRITE "${WORK_DIR}/c_lib/only_c.c" "void only_c(void) {}\n")
file(WRITE "${WORK_DIR}/c_lib/private_impl.h" "static void* private_impl_null(void) { return NULL; }\n")

set(INPUT "${WORK_DIR}/legacy_modernisation.cpp")
set(OUTPUT_DIR "${WORK_DIR}/out")
set(LANGUAGE_OUTPUT_DIR "${WORK_DIR}/language-out")

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter textual --out "${LANGUAGE_OUTPUT_DIR}" "${WORK_DIR}"
    RESULT_VARIABLE language_plan_result
    OUTPUT_VARIABLE language_plan_stdout
    ERROR_VARIABLE language_plan_stderr
)
if (NOT language_plan_result EQUAL 0)
    message(FATAL_ERROR "moult directory language plan failed\nstdout:\n${language_plan_stdout}\nstderr:\n${language_plan_stderr}")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" report "${LANGUAGE_OUTPUT_DIR}/plan.json"
    RESULT_VARIABLE language_report_result
    OUTPUT_VARIABLE language_report_stdout
    ERROR_VARIABLE language_report_stderr
)
if (NOT language_report_result EQUAL 0)
    message(FATAL_ERROR "moult directory language report failed\nstdout:\n${language_report_stdout}\nstderr:\n${language_report_stderr}")
endif()
if (NOT language_report_stdout MATCHES "Accepted edits: 3")
    message(FATAL_ERROR "expected directory scan to ignore C source for C++ edits\nstdout:\n${language_report_stdout}")
endif()
if (language_report_stdout MATCHES "legacy_c.c")
    message(FATAL_ERROR "C source should not appear in C++ modernisation report")
endif()
if (language_report_stdout MATCHES "legacy_c_api.h")
    message(FATAL_ERROR "C ABI header should not appear in C++ modernisation report")
endif()
if (language_report_stdout MATCHES "legacy_c_private.h")
    message(FATAL_ERROR "plain C header should not appear in C++ modernisation report")
endif()
if (language_report_stdout MATCHES "private_impl.h")
    message(FATAL_ERROR "header in C-only directory should not appear in C++ modernisation report")
endif()

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
    COMMAND "${MOULT_EXE}" report "${OUTPUT_DIR}/plan.json"
    RESULT_VARIABLE report_result
    OUTPUT_VARIABLE report_stdout
    ERROR_VARIABLE report_stderr
)
if (NOT report_result EQUAL 0)
    message(FATAL_ERROR "moult report failed\nstdout:\n${report_stdout}\nstderr:\n${report_stderr}")
endif()
if (NOT report_stdout MATCHES "Moult Report")
    message(FATAL_ERROR "expected report heading")
endif()
if (NOT report_stdout MATCHES "Accepted edits: 3")
    message(FATAL_ERROR "expected accepted edit count in report")
endif()
if (NOT report_stdout MATCHES "Manual-review findings: 4")
    message(FATAL_ERROR "expected manual review count in report")
endif()
if (NOT report_stdout MATCHES "modernise.use-nullptr")
    message(FATAL_ERROR "expected use-nullptr edit in report")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" diff "${OUTPUT_DIR}/plan.json"
    RESULT_VARIABLE diff_result
    OUTPUT_VARIABLE diff_stdout
    ERROR_VARIABLE diff_stderr
)
if (NOT diff_result EQUAL 0)
    message(FATAL_ERROR "moult diff failed\nstdout:\n${diff_stdout}\nstderr:\n${diff_stderr}")
endif()
if (NOT diff_stdout MATCHES "diff --git")
    message(FATAL_ERROR "expected git-style diff output")
endif()
if (NOT diff_stdout MATCHES "\\+int\\* legacy_pointer\\(\\) noexcept")
    message(FATAL_ERROR "expected noexcept replacement in diff")
endif()
if (NOT diff_stdout MATCHES "# Manual Review Suggestions")
    message(FATAL_ERROR "expected manual review suggestions in diff output")
endif()
if (NOT diff_stdout MATCHES "modernise.replace-auto-ptr")
    message(FATAL_ERROR "expected auto_ptr manual review suggestion in diff output")
endif()

execute_process(
    COMMAND "${MOULT_EXE}" review "${OUTPUT_DIR}/plan.json"
    RESULT_VARIABLE review_result
    OUTPUT_VARIABLE review_stdout
    ERROR_VARIABLE review_stderr
)
if (review_result EQUAL 0)
    message(FATAL_ERROR "moult review unexpectedly succeeded without an interactive terminal\nstdout:\n${review_stdout}\nstderr:\n${review_stderr}")
endif()
if (NOT review_stderr MATCHES "requires an interactive terminal")
    message(FATAL_ERROR "expected non-interactive review error\nstdout:\n${review_stdout}\nstderr:\n${review_stderr}")
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
