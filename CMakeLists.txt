cmake_minimum_required(VERSION 3.20)
project(minivectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MVDB_STATIC_RUNTIME "Link libstdc++/libgcc statically" OFF)
option(MVDB_BUILD_TESTS "Build unit tests" OFF)

add_executable(minivectordb
  src/main.cpp
  src/util/util.cpp
  src/sql/sql.cpp
  src/exec/engine.cpp
  src/protocol/protocol.cpp
  src/protocol/json.cpp
  src/storage/page.cpp
  src/storage/buffer_pool.cpp
  src/storage/heap_file.cpp
  src/storage/record.cpp
  src/storage/catalog.cpp
  src/storage/btree.cpp
  src/txn/wal.cpp
  src/vector/hnsw.cpp
  src/vector/distance.cpp
)

target_include_directories(minivectordb PRIVATE src)
target_compile_options(minivectordb PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(MVDB_STATIC_RUNTIME)
  target_link_options(minivectordb PRIVATE -static-libstdc++ -static-libgcc)
endif()

find_package(Threads REQUIRED)
target_link_libraries(minivectordb PRIVATE Threads::Threads)

if(MVDB_BUILD_TESTS)
  add_executable(buffer_pool_spill_test
    tests/storage/buffer_pool_spill_test.cpp
    src/storage/buffer_pool.cpp
    src/storage/page.cpp
  )
  target_include_directories(buffer_pool_spill_test PRIVATE src)
  add_executable(btree_test
    tests/storage/btree_test.cpp
    src/storage/btree.cpp
  src/txn/wal.cpp
  src/vector/hnsw.cpp
  src/vector/distance.cpp
    src/storage/buffer_pool.cpp
    src/storage/page.cpp
  )
  target_include_directories(btree_test PRIVATE src)
  target_link_libraries(btree_test PRIVATE Threads::Threads)

  add_executable(hnsw_test
    tests/storage/hnsw_test.cpp
    src/vector/hnsw.cpp
    src/vector/distance.cpp
  src/vector/distance.cpp
    src/sql/sql.cpp
    src/util/util.cpp
  )
  target_include_directories(hnsw_test PRIVATE src)
  target_link_libraries(hnsw_test PRIVATE Threads::Threads)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME buffer_pool_spill COMMAND buffer_pool_spill_test)
  add_test(NAME btree COMMAND btree_test)
  add_test(NAME hnsw COMMAND hnsw_test)
  foreach(suite correctness_regressions sql_protocol_edges vector_edges large_rows)
    add_test(NAME ${suite}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/conformance/test_${suite}.py
              --bin $<TARGET_FILE:minivectordb>)
  endforeach()
  add_test(NAME conformance
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/conformance/driver.py
            --bin $<TARGET_FILE:minivectordb>)
  add_test(NAME public_cases
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/official/run_official.py
            --bin $<TARGET_FILE:minivectordb>)
  add_test(NAME requirements
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/official/requirements_check.py
            $<TARGET_FILE:minivectordb>)
  add_test(NAME crash_recovery
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/crash/kill_test.py
            --bin $<TARGET_FILE:minivectordb> --trials 20)
  add_test(NAME large_dataset
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/storage/large_dataset.py
            --bin $<TARGET_FILE:minivectordb> --rows 100000 --dim 128)
  set_tests_properties(correctness_regressions sql_protocol_edges vector_edges large_rows
    conformance public_cases requirements crash_recovery large_dataset
    PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 600)
endif()
