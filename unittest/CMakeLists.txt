# Production/evaluation builds must not acquire a new GTest requirement merely
# because these optional regressions exist in the source checkout.
option(SEEKDB_BUILD_COMPETITION_TESTS "Build competition regression test targets" OFF)
if(NOT SEEKDB_BUILD_COMPETITION_TESTS)
  return()
endif()

# Focused competition regressions. Each source owns its main(), so keep these
# independent of any future module/unity pretest binary.
find_library(SEEKDB_TEST_GTEST_LIBRARY NAMES gtest
  PATHS "${DEP_DIR}/lib" "${DEP_DIR}/lib64" NO_DEFAULT_PATH)
if(NOT SEEKDB_TEST_GTEST_LIBRARY)
  message(FATAL_ERROR "Competition unit tests require the bundled devdeps-gtest library")
endif()
add_library(seekdb_test_gtest STATIC IMPORTED)
set_target_properties(seekdb_test_gtest PROPERTIES
  IMPORTED_LOCATION "${SEEKDB_TEST_GTEST_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${DEP_DIR}/include")

add_custom_target(competition_unit_tests)
function(seekdb_competition_gtest target source)
  add_executable(${target} EXCLUDE_FROM_ALL "${source}")
  target_link_libraries(${target} PRIVATE oceanbase seekdb_test_gtest file_sql)
  target_compile_options(${target} PRIVATE -UNDEBUG)
  add_dependencies(competition_unit_tests ${target})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES LABELS competition)
endfunction()

seekdb_competition_gtest(test_file_sql_parser sql/parser/test_file_sql_parser.cpp)
seekdb_competition_gtest(test_file_scan_utils sql/engine/basic/test_file_scan_utils.cpp)
seekdb_competition_gtest(test_ai_column_parser sql/parser/test_ai_column_parser.cpp)
seekdb_competition_gtest(ob_expr_ai_prompt_test sql/engine/expr/ob_expr_ai_prompt_test.cpp)
seekdb_competition_gtest(test_ob_openai_utils sql/engine/expr/test_ob_openai_utils.cpp)
seekdb_competition_gtest(test_ob_ai_rerank sql/engine/expr/test_ob_ai_rerank.cpp)
seekdb_competition_gtest(test_ob_ai_embed_batch sql/engine/expr/test_ob_ai_embed_batch.cpp)

# This state-machine test uses assert(), not GTest, and needs no server library.
add_executable(test_ai_column_worker_state EXCLUDE_FROM_ALL
  sql/engine/ai_column/test_ai_column_worker_state.cpp)
target_include_directories(test_ai_column_worker_state PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_features(test_ai_column_worker_state PRIVATE cxx_std_17)
target_compile_options(test_ai_column_worker_state PRIVATE -UNDEBUG)
add_dependencies(competition_unit_tests test_ai_column_worker_state)
add_test(NAME test_ai_column_worker_state COMMAND test_ai_column_worker_state)
set_tests_properties(test_ai_column_worker_state PROPERTIES LABELS competition)

seekdb_competition_gtest(test_fts_hotpath storage/fts/test_fts_hotpath.cpp)
