/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER_OMT
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ai_service/ai_column_worker_state.h"
#include "observer/ob_inner_sql_connection.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/session/ob_sql_session_info.h"
#include "observer/vector_index/ob_vector_index_util.h"
#include "lib/time/ob_time_utility.h"
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <unistd.h>

namespace oceanbase { namespace observer {
using namespace common;
using namespace share::schema;
namespace {
using State = AiColumnTaskState;
const char *TASKS = "oceanbase.__all_ai_column_task";
const int64_t LEASE_US = 120000000;
struct Task {
  uint64_t table = 0, column = 0;
  int64_t version = 0, state = 0, retries = 0;
  std::string request, predicate, owner, payload;
  bool null_result = false;
};
std::string str(const ObString &s) { return std::string(s.ptr() == nullptr ? "" : s.ptr(), s.length()); }
std::string number(int64_t n) { return std::to_string(n); }
std::string literal(const std::string &s)
{
  static const char hex[] = "0123456789abcdef";
  std::string out("CONVERT(X'");
  for (unsigned char c : s) { out += hex[c >> 4]; out += hex[c & 15]; }
  out += "' USING utf8mb4)";
  return out;
}
std::string identifier(const ObString &s)
{
  std::string out("`");
  for (int64_t i = 0; i < s.length(); ++i) {
    if (s.ptr()[i] == '`') { out += '`'; }
    out += s.ptr()[i];
  }
  return out + '`';
}
std::string group_key(const Task &t)
{
  return "table_id=" + number(t.table) + " AND request_id=" + literal(t.request)
      + " AND rowkey_predicate=" + literal(t.predicate);
}
std::string task_key(const Task &t) { return group_key(t) + " AND column_id=" + number(t.column); }
// The flag is restored before returning a connection to the pool, including error paths.
class InternalSessionGuard {
public:
  explicit InternalSessionGuard(ObSingleConnectionProxy &proxy)
    : session_(static_cast<ObInnerSQLConnection *>(proxy.get_connection())->get_session()),
      previous_(session_.is_ai_column_internal()), mode_(session_.get_sql_mode())
  {
    session_.set_ai_column_internal(true);
    session_.set_sql_mode(mode_ | SMO_STRICT_ALL_TABLES);
  }
  ~InternalSessionGuard() { restore(); }
  void restore() { if (active_) { session_.set_ai_column_internal(previous_); session_.set_sql_mode(mode_); active_ = false; } }
private:
  sql::ObSQLSessionInfo &session_;
  bool previous_;
  ObSQLMode mode_;
  bool active_ = true;
};
int get_string(sqlclient::ObMySQLResult &res, const char *field, std::string &out, bool *null_value = nullptr)
{
  ObString s;
  int ret = res.get_varchar(field, s);
  if (ret == OB_ERR_NULL_VALUE && null_value != nullptr) { *null_value = true; ret = OB_SUCCESS; }
  else if (ret == OB_SUCCESS) { out = str(s); if (null_value != nullptr) { *null_value = false; } }
  return ret;
}
int read_task(sqlclient::ObMySQLResult &r, Task &t)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(r.get_uint("table_id", t.table)) || OB_FAIL(r.get_uint("column_id", t.column))
      || OB_FAIL(r.get_int("definition_version", t.version)) || OB_FAIL(r.get_int("state", t.state))
      || OB_FAIL(r.get_int("retry_count", t.retries)) || OB_FAIL(get_string(r, "request_id", t.request))
      || OB_FAIL(get_string(r, "rowkey_predicate", t.predicate)) || OB_FAIL(get_string(r, "owner_token", t.owner))
      || OB_FAIL(get_string(r, "result_payload", t.payload, &t.null_result))) {}
  return ret;
}
int write(ObISQLClient &client, const std::string &sql, int64_t &affected)
{ return client.write(sql.c_str(), affected); }
struct Definition {
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  AiColumnInfo info;
  std::string qualified, result_name, state_name, request_name, spec_name;
  int resolve(const Task &t)
  {
    int ret = OB_SUCCESS;
    const ObDatabaseSchema *database = nullptr;
    const ObColumnSchemaV2 *state_col = nullptr, *request_col = nullptr, *spec_col = nullptr;
    if (OB_ISNULL(GCTX.schema_service_)) { ret = OB_NOT_INIT; }
    else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {}
    else if (OB_FAIL(guard.get_table_schema(t.table, table))) {}
    else if (table == nullptr || (column = table->get_column_schema(t.column)) == nullptr || !column->is_ai_column()) { ret = OB_ENTRY_NOT_EXIST; }
    else if (OB_FAIL(get_ai_column_info(*column, info))) {}
    else if (info.definition_version_ != t.version) { ret = OB_ENTRY_NOT_EXIST; }
    else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {}
    else if (database == nullptr
        || (state_col = table->get_column_schema(info.state_column_id_)) == nullptr
        || (request_col = table->get_column_schema(info.request_column_id_)) == nullptr
        || (spec_col = table->get_column_schema(info.result_spec_column_id_)) == nullptr) { ret = OB_ENTRY_NOT_EXIST; }
    else {
      qualified = identifier(database->get_database_name_str()) + "." + identifier(table->get_table_name_str());
      result_name = identifier(column->get_column_name_str());
      state_name = identifier(state_col->get_column_name_str());
      request_name = identifier(request_col->get_column_name_str());
      spec_name = identifier(spec_col->get_column_name_str());
    }
    return ret;
  }
  std::string current(const Task &t) const
  {
    return "(" + t.predicate + ") AND " + request_name + "=" + literal(t.request)
        + " AND " + spec_name + "=" + literal(number(t.version))
        + " AND EXISTS (SELECT 1 FROM oceanbase.__all_ai_column a WHERE a.table_id="
        + number(t.table) + " AND a.column_id=" + number(t.column)
        + " AND a.definition_version=" + number(t.version) + ")";
  }
};
// No permissive numeric conversion: SQL CAST would silently turn bad model text into zero.
int validate_number(const std::string &value, ObObjType type)
{
  int ret = OB_SUCCESS;
  const bool integral = ob_is_integer_type(type);
  const bool numeric = integral || ob_is_numeric_type(type);
  if (numeric) {
    const char *begin = value.c_str();
    char *end = nullptr;
    errno = 0;
    if (integral) {
      bool unsigned_type = ob_is_uint_tc(type);
      size_t p = (!value.empty() && (value[0] == '-' || value[0] == '+')) ? 1 : 0;
      if (p == value.size() || (unsigned_type && value[0] == '-')) { return OB_INVALID_ARGUMENT; }
      for (size_t i = p; i < value.size(); ++i) { if (value[i] < '0' || value[i] > '9') { return OB_INVALID_ARGUMENT; } }
      const uint64_t u = unsigned_type ? strtoull(begin, &end, 10) : 0;
      const int64_t v = unsigned_type ? 0 : strtoll(begin, &end, 10);
      int bits = 64;
      switch (type) {
        case ObTinyIntType: case ObUTinyIntType: bits = 8; break;
        case ObSmallIntType: case ObUSmallIntType: bits = 16; break;
        case ObMediumIntType: case ObUMediumIntType: bits = 24; break;
        case ObInt32Type: case ObUInt32Type: bits = 32; break;
        default: break;
      }
      if (bits < 64 && (unsigned_type ? u >= (1ULL << bits) : (v < -(1LL << (bits - 1)) || v >= (1LL << (bits - 1))))) { ret = OB_DATA_OUT_OF_RANGE; }
    } else {
      const long double v = strtold(begin, &end);
      if (!std::isfinite(v) || (ob_is_float_type(type) && std::fabs(v) > std::numeric_limits<float>::max())
          || (ob_is_double_type(type) && std::fabs(v) > std::numeric_limits<double>::max())) { ret = OB_DATA_OUT_OF_RANGE; }
    }
    if (errno == ERANGE || end == begin || end == nullptr || *end != '\0') { ret = OB_DATA_OUT_OF_RANGE; }
  }
  return ret;
}
int validate_vector(const std::string &value, const ObColumnSchemaV2 &column)
{
  int64_t dimension = 0;
  int ret = share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), dimension);
  if (ret != OB_SUCCESS) { return ret; }
  const char *p = value.c_str();
  auto spaces = [&]() { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; } };
  spaces();
  if (*p++ != '[') { return OB_INVALID_ARGUMENT; }
  int64_t count = 0;
  spaces();
  while (*p != ']' && *p != '\0') {
    // Disallow NaN/Inf and non-JSON floating representations accepted by strtod.
    if (*p != '-' && (*p < '0' || *p > '9')) { return OB_INVALID_ARGUMENT; }
    const char *begin = p;
    char *end = nullptr;
    errno = 0;
    const double value = strtod(begin, &end);
    if (end == begin || errno == ERANGE || !std::isfinite(value)
        || std::fabs(value) > std::numeric_limits<float>::max()) { return OB_DATA_OUT_OF_RANGE; }
    p = end;
    ++count;
    spaces();
    if (*p == ',') { ++p; spaces(); if (*p == ']') { return OB_INVALID_ARGUMENT; } }
    else if (*p != ']') { return OB_INVALID_ARGUMENT; }
  }
  if (*p++ != ']') { return OB_INVALID_ARGUMENT; }
  spaces();
  return *p == '\0' && count == dimension ? OB_SUCCESS : OB_INVALID_ARGUMENT;
}
int evaluate(Task &t, Definition &d)
{
  int ret = OB_SUCCESS;
  ObSingleConnectionProxy proxy;
  if (OB_FAIL(proxy.connect(0, GCTX.sql_proxy_))) {}
  else {
    InternalSessionGuard internal(proxy);
    // An autocommit SELECT uses a consistent snapshot and holds no business row write lock.
    // CAST AS CHAR also serializes native vectors without interpreting their binary layout.
    const std::string query = "SELECT CAST((" + str(d.info.normalized_expr_) + ") AS CHAR) AS payload FROM "
        + d.qualified + " WHERE " + d.current(t)
        + " AND EXISTS (SELECT 1 FROM " + TASKS + " WHERE " + task_key(t)
        + " AND state=1 AND owner_token=" + literal(t.owner)
        + " AND lease_until>" + number(ObTimeUtility::current_time()) + ")";
    {
      ObISQLClient::ReadResult res;
      if (OB_FAIL(proxy.read(res, query.c_str()))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(res.get_result()->next())) { if (ret == OB_ITER_END) { ret = OB_ENTRY_NOT_EXIST; } }
      else { ret = get_string(*res.get_result(), "payload", t.payload, &t.null_result); }
    }
    if (OB_SUCC(ret) && !t.null_result) {
      ret = validate_number(t.payload, d.column->get_data_type());
      if (OB_SUCC(ret) && d.column->get_data_type() == ObCollectionSQLType) {
        ret = validate_vector(t.payload, *d.column);
      }
      if (OB_SUCC(ret) && ob_is_string_type(d.column->get_data_type())
          && d.column->get_data_length() > 0
          && ObCharset::strlen_char(d.column->get_collation_type(), t.payload.data(), t.payload.size()) > d.column->get_data_length()) {
        ret = OB_DATA_TOO_LONG;
      }
      if (OB_SUCC(ret) && (d.column->get_data_type() == ObJsonType || d.column->get_data_type() == ObCollectionSQLType)) {
        ObISQLClient::ReadResult res;
        int64_t valid = 0;
        const std::string query = "SELECT JSON_VALID(" + literal(t.payload) + ") AS valid";
        if (OB_FAIL(proxy.read(res, query.c_str()))) {}
        else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(res.get_result()->next()) || OB_FAIL(res.get_result()->get_int("valid", valid))) {}
        else if (valid != 1) { ret = OB_INVALID_ARGUMENT; }
      }
      if (OB_SUCC(ret) && (ob_is_real_type(d.column->get_data_type())
          || ob_is_number_tc(d.column->get_data_type()) || ob_is_decimal_int_tc(d.column->get_data_type()))) {
        std::string target = "DOUBLE";
        if (!ob_is_real_type(d.column->get_data_type())) {
          target = "DECIMAL(" + number(d.column->get_accuracy().get_precision()) + ","
              + number(d.column->get_accuracy().get_scale()) + ")";
        }
        ObISQLClient::ReadResult res;
        const std::string query = "SELECT CAST(" + literal(t.payload) + " AS " + target + ") AS value";
        auto &session = static_cast<ObInnerSQLConnection *>(proxy.get_connection())->get_session();
        session.get_warnings_buffer().reset();
        if (OB_FAIL(proxy.read(res, query.c_str()))) {}
        else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else if (OB_FAIL(res.get_result()->next())) {}
        else if (session.get_warnings_buffer().get_total_warning_count() != 0) { ret = OB_DATA_OUT_OF_RANGE; }
      }
    }
  }
  return ret;
}
int claim(Task &t, bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  const int64_t now = ObTimeUtility::current_time();
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {}
  else {
    InternalSessionGuard internal(trans);
    const std::string eligible = "((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_until<=" + number(now) + "))";
    {
      ObISQLClient::ReadResult res;
      const std::string query = std::string("SELECT * FROM ") + TASKS + " WHERE " + eligible + " ORDER BY eligible_ts LIMIT 1 FOR UPDATE";
      if (OB_FAIL(trans.read(res, query.c_str()))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(res.get_result()->next())) { if (ret == OB_ITER_END) { ret = OB_SUCCESS; } }
      else if (OB_FAIL(read_task(*res.get_result(), t))) {}
      else { found = true; }
    }
    if (OB_SUCC(ret) && found) {
      t.owner = number(now) + ":" + number(getpid());
      if (t.state == State::RUNNING) { ++t.retries; }
      int64_t affected = 0;
      ret = write(trans, std::string("UPDATE ") + TASKS + " SET state=1,owner_token=" + literal(t.owner)
          + ",lease_until=" + number(now + LEASE_US) + ",retry_count=" + number(t.retries)
          + " WHERE " + task_key(t) + " AND " + eligible, affected);
      if (OB_SUCC(ret) && affected != 1) { found = false; }
    }
    // Restore the capability before end() returns the connection to the pool.
    internal.restore();
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
int finish(const Task &t, int error, int64_t max_retries)
{
  int ret = OB_SUCCESS;
  int64_t affected = 0;
  const int64_t now = ObTimeUtility::current_time();
  const int state = error == OB_SUCCESS ? State::SUCCEEDED : State::failure_state(t.retries, max_retries);
  const int64_t retries = t.retries + (state == State::RETRY_WAIT ? 1 : 0);
  const std::string value = error == OB_SUCCESS && !t.null_result ? literal(t.payload) : "NULL";
  // Conditional completion fences workers whose lease expired while the HTTP request was running.
  ret = write(*GCTX.sql_proxy_, std::string("UPDATE ") + TASKS + " SET state=" + number(state)
      + ",retry_count=" + number(retries) + ",eligible_ts=TIMESTAMPADD(MICROSECOND," + number(State::retry_delay_us(retries)) + ",NOW(6))"
      + ",result_payload=" + value + ",error_code=" + number(error)
      + " WHERE " + task_key(t) + " AND state=1 AND owner_token=" + literal(t.owner)
      + " AND lease_until>" + number(now), affected);
  return ret;
}
int publish(const Task &group)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {}
  else {
    InternalSessionGuard internal(trans);
    std::vector<Task> tasks;
    bool ready = true, stale = false;
    {
      ObISQLClient::ReadResult res;
      const std::string query = std::string("SELECT * FROM ") + TASKS + " WHERE " + group_key(group) + " ORDER BY column_id";
      if (OB_FAIL(trans.read(res, query.c_str()))) {}
      else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else {
        while (OB_SUCC(ret) && OB_SUCC(ret = res.get_result()->next())) {
          Task task;
          if (OB_FAIL(read_task(*res.get_result(), task))) {}
          else { ready &= State::terminal(task.state); tasks.push_back(task); }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      }
    }
    std::string update, condition;
    for (size_t i = 0; OB_SUCC(ret) && ready && !stale && i < tasks.size(); ++i) {
      Definition d;
      if (OB_FAIL(d.resolve(tasks[i]))) {
        if (ret == OB_ENTRY_NOT_EXIST) { stale = true; ret = OB_SUCCESS; }
      } else {
        if (i == 0) { update = "UPDATE " + d.qualified + " SET "; }
        else { update += ","; condition += " AND "; }
        const bool success = tasks[i].state == State::SUCCEEDED;
        update += d.result_name + "=" + (success && !tasks[i].null_result ? literal(tasks[i].payload) : "NULL")
            + "," + d.state_name + "=" + (success ? "1" : "2");
        condition += "(" + d.current(tasks[i]) + ")";
      }
    }
    int64_t affected = 0;
    if (OB_SUCC(ret) && ready && !tasks.empty()) {
      if (!stale) {
        ret = write(trans, update + " WHERE " + condition, affected);
      }
      if (OB_SUCC(ret)) {
        // Delete and native row/index UPDATE commit together. Zero matched rows means superseded/deleted.
        ret = write(trans, std::string("DELETE FROM ") + TASKS + " WHERE " + group_key(group), affected);
      }
    }
    internal.restore();
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
int recover_publication()
{
  int ret = OB_SUCCESS;
  Task task;
  bool found = false;
  {
    ObISQLClient::ReadResult res;
    const std::string query = std::string("SELECT * FROM ") + TASKS
        + " t WHERE state IN (2,4) AND NOT EXISTS (SELECT 1 FROM " + TASKS
        + " n WHERE n.table_id=t.table_id AND n.request_id=t.request_id AND n.rowkey_predicate=t.rowkey_predicate"
          " AND n.state NOT IN (2,4)) LIMIT 1";
    if (OB_FAIL(GCTX.sql_proxy_->read(res, query.c_str()))) {}
    else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(res.get_result()->next())) { if (ret == OB_ITER_END) { ret = OB_SUCCESS; } }
    else if (OB_FAIL(read_task(*res.get_result(), task))) {}
    else { found = true; }
  }
  if (OB_SUCC(ret) && found) { ret = publish(task); }
  return ret;
}
} // namespace
int AiColumnWorker::process_one()
{
  int ret = OB_SUCCESS;
  Task task;
  bool found = false;
  if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else if (OB_FAIL(recover_publication())) {}
  else if (OB_FAIL(claim(task, found))) {}
  else if (found) {
    Definition definition;
    int error = definition.resolve(task);
    if (error == OB_SUCCESS) {
      error = task.retries > definition.info.max_retries_ ? OB_TIMEOUT : evaluate(task, definition);
    }
    const int64_t maximum = error == OB_ENTRY_NOT_EXIST ? 0 : definition.info.max_retries_;
    if (OB_FAIL(finish(task, error, maximum))) {}
    else { ret = publish(task); }
  }
  return ret;
}
void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumn");
  while (!has_set_stop()) {
    const int ret = process_one();
    if (ret != OB_SUCCESS && ret != OB_TABLE_NOT_EXIST && ret != OB_NOT_INIT) {
      LOG_WARN("AI column worker iteration failed", K(ret));
    }
    usleep(100000);
  }
}
} }
