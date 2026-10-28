# Competition native regression tests

The baseline uses `build.sh`/CMake. The repository no longer has `build.py`.
The root CMake file visits `unittest/CMakeLists.txt` on Linux. Targets are enabled
explicitly with `-DSEEKDB_BUILD_COMPETITION_TESTS=ON`; the default production
configuration does not require GTest. Tests are
`EXCLUDE_FROM_ALL`: building the production server does not build them.
Each test source retains its own main function and is a separate executable.

After configuring the production release build with the normal `build.sh`
workflow and installing its bundled dependencies:

```sh
./build.sh release -DSEEKDB_BUILD_COMPETITION_TESTS=ON
cmake --build build_release --target competition_unit_tests --parallel 2
ctest --test-dir build_release -L competition --output-on-failure
```

Individual targets, e.g. `test_file_sql_parser`, can also be built and selected
with `ctest --test-dir build_release -R '^test_file_sql_parser$' --output-on-failure`.

The seven GTest binaries use the matching production `oceanbase` shared-library
and Rust `file_sql` targets plus bundled `libgtest.a`. They do not use
`gtest_main`, since every source supplies main. CMake builds `liboceanbase.so`
from the production object graph unless `OB_SO_CACHE` explicitly supplies it.
Do not point that option at a stale or unrelated server build.

`test_ai_column_worker_state` is a standalone state-machine assertion test.
All eight targets use `-UNDEBUG` so release configuration does not remove their
assertions. Test registration does not change official mysqltest suites or
publish a module pretest binary in the farm installation component.
