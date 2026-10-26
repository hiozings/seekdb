# Variables are supplied by run_mysqltest.py; use a dedicated local instance.
--disable_query_log
--source $file_sql_setup_include
SET enable_file_sql=ON;
--replace_result $file_sql_data_dir FILE_SQL_DATA
--enable_query_log
