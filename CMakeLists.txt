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
endif()
