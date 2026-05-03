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

file(WRITE "${WORK_DIR}/compile_commands.json"
"[
  {
    \"directory\": \"${WORK_DIR}\",
    \"file\": \"legacy_modernisation.cpp\",
    \"command\": \"c++ -std=c++14 -c legacy_modernisation.cpp -o legacy.o\"
  },
  {
    \"directory\": \"${WORK_DIR}\",
    \"file\": \"legacy_c.c\",
    \"command\": \"cc -std=c11 -c legacy_c.c -o legacy_c.o\"
  }
]
")

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter textual --compile-commands "${WORK_DIR}/compile_commands.json" --include-facts
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "moult textual compile database run failed\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if (NOT stdout MATCHES "\"scanner\":\"textual-cpp-modernisation\"")
    message(FATAL_ERROR "expected textual facts in compile database stdout")
endif()
if (NOT stdout MATCHES "\"accepted_edit_count\":3")
    message(FATAL_ERROR "expected accepted edits from compile database input")
endif()
if (stdout MATCHES "legacy_c.c")
    message(FATAL_ERROR "C source from compile database should not be scanned by C++ modernisation")
endif()

file(WRITE "${WORK_DIR}/compile_commands_cxx_c.json"
"[
  {
    \"directory\": \"${WORK_DIR}\",
    \"file\": \"legacy_c.c\",
    \"command\": \"c++ -std=c++14 -c legacy_c.c -o legacy_c.o\"
  }
]
")

execute_process(
    COMMAND "${MOULT_EXE}" plan --adapter textual --compile-commands "${WORK_DIR}/compile_commands_cxx_c.json"
    RESULT_VARIABLE cxx_c_result
    OUTPUT_VARIABLE cxx_c_stdout
    ERROR_VARIABLE cxx_c_stderr
)
if (NOT cxx_c_result EQUAL 0)
    message(FATAL_ERROR "moult textual C file as C++ run failed\nstdout:\n${cxx_c_stdout}\nstderr:\n${cxx_c_stderr}")
endif()
if (NOT cxx_c_stdout MATCHES "\"accepted_edit_count\":1")
    message(FATAL_ERROR "expected C file compiled as C++ to be eligible for C++ modernisation")
endif()
