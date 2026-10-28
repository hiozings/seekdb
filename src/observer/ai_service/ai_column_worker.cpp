/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER_OMT
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ai_service/ai_column_worker_state.h"
#include "observer/ob_inner_sql_connection.h"
#include "observer/ob_inner_sql_result.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_constraint.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "sql/session/ob_sql_session_info.h"
#include "observer/vector_index/ob_vector_index_util.h"
#include "lib/time/ob_time_utility.h"
#include <string>
#include <algorithm>
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
  std::string request, predicate, rowkey_hash, owner, payload;
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
      + " AND rowkey_hash=" + literal(t.rowkey_hash)
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
  int ret = OB_SUCCESS;
  ObObj value;
  ObString text;
  ObArenaAllocator allocator("AiColumnRead");
  if (OB_FAIL(res.get_obj(field, value))) {}
  else if (value.is_null()) {
    if (null_value == nullptr) { ret = OB_ERR_NULL_VALUE; }
    else { *null_value = true; out.clear(); }
  } else {
    if (value.is_lob_storage()) {
      // All worker clients use GCTX's inner SQL proxy. Materialize out-of-row
      // LONGTEXT with this result's snapshot; get_varchar only exposes in-row data.
      auto &ctx = static_cast<ObInnerSQLResult &>(res).result_set().get_exec_context();
      ret = sql::ObTextStringHelper::read_real_string_data(ctx, &allocator, value, text);
    } else { ret = value.get_string(text); }
    if (OB_SUCC(ret)) { out = str(text); if (null_value != nullptr) { *null_value = false; } }
  }
  return ret;
}
int read_task(sqlclient::ObMySQLResult &r, Task &t)
{
  int ret = OB_SUCCESS;
  int64_t table_id = 0, column_id = 0;
  // Inner SQL getters preserve ObObj types: signed BIGINT cannot use get_uint.
  if (OB_FAIL(r.get_int("table_id", table_id)) || OB_FAIL(r.get_int("column_id", column_id))) {}
  else if (table_id <= 0 || column_id <= 0) { ret = OB_INVALID_ARGUMENT; }
  else {
    t.table = static_cast<uint64_t>(table_id);
    t.column = static_cast<uint64_t>(column_id);
    if (OB_FAIL(r.get_int("definition_version", t.version)) || OB_FAIL(r.get_int("state", t.state))
        || OB_FAIL(r.get_int("retry_count", t.retries)) || OB_FAIL(get_string(r, "request_id", t.request))
        || OB_FAIL(get_string(r, "rowkey_predicate", t.predicate)) || OB_FAIL(get_string(r, "rowkey_hash", t.rowkey_hash))
        || OB_FAIL(get_string(r, "owner_token", t.owner))
        || OB_FAIL(get_string(r, "result_payload", t.payload, &t.null_result))) {}
  }
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
  std::string current_request(const Task &t) const
  {
    return request_name + "=" + literal(t.request)
        + " AND " + spec_name + "=" + literal(number(t.version))
        + " AND EXISTS (SELECT 1 FROM oceanbase.__all_ai_column a WHERE a.table_id="
        + number(t.table) + " AND a.column_id=" + number(t.column)
        + " AND a.definition_version=" + number(t.version) + ")";
  }
  std::string current(const Task &t) const
  { return "(" + t.predicate + ") AND " + current_request(t); }
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
        ret = OB_ERR_DATA_TOO_LONG;
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
// Consume only an explicit ALTER job. Spec fencing makes a committed batch
// idempotent across crashes; native DML creates row tasks in the same transaction.
int backfill(const Task &job)
{
  int ret = OB_SUCCESS;
  Definition definition;
  const int definition_ret = definition.resolve(job);
  const bool stale = definition_ret == OB_ENTRY_NOT_EXIST;
  if (definition_ret != OB_SUCCESS && !stale) { return definition_ret; }
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {}
  else {
    InternalSessionGuard internal(trans);
    int64_t changed = 0;
    if (!stale) {
      auto &session = static_cast<ObInnerSQLConnection *>(trans.get_connection())->get_session();
      const uint64_t previous_retry = session.get_ai_column_retry_column_id();
      session.set_ai_column_retry_column_id(job.column);
      session.set_ai_column_internal(false);
      const std::string sql = "UPDATE " + definition.qualified + " SET " + definition.result_name
          + "=NULL WHERE " + definition.request_name + " IS NULL OR " + definition.spec_name
          + " IS NULL OR " + definition.spec_name + "<>" + literal(number(job.version)) + " LIMIT 128";
      ret = write(trans, sql, changed);
      session.set_ai_column_internal(true);
      session.set_ai_column_retry_column_id(previous_retry);
    }
    if (OB_SUCC(ret)) {
      int64_t affected = 0;
      const std::string owned = task_key(job) + " AND state=1 AND owner_token=" + literal(job.owner);
      if (stale || changed == 0) {
        ret = write(trans, std::string("DELETE FROM ") + TASKS + " WHERE " + owned, affected);
      } else {
        ret = write(trans, std::string("UPDATE ") + TASKS
            + " SET state=0,eligible_ts=NOW(6),lease_until=0,owner_token='' WHERE " + owned, affected);
      }
      if (OB_SUCC(ret) && affected != 1) { ret = OB_EAGAIN; }
    }
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
// Columns constrained together must be tested together. Ordinary inputs are
// immutable during publication; expand generated expressions only to find the
// target AI columns they depend on, not to connect unrelated shared inputs.
int publication_components(const std::vector<Task> &tasks, std::vector<size_t> &components)
{
  int ret = OB_SUCCESS;
  Definition definition;
  if (tasks.empty()) { return ret; }
  if (OB_FAIL(definition.resolve(tasks[0]))) { return ret; }
  const ObTableSchema &table = *definition.table;
  for (size_t i = 0; i < tasks.size(); ++i) { components.push_back(i); }
  auto root = [&](size_t i) {
    while (components[i] != i) { i = components[i]; }
    return i;
  };
  auto connect = [&](const ObIArray<uint64_t> &references) -> int {
    int ret = OB_SUCCESS;
    std::vector<uint64_t> pending, visited;
    std::vector<size_t> targets;
    for (int64_t i = 0; i < references.count(); ++i) { pending.push_back(references.at(i)); }
    for (size_t i = 0; OB_SUCC(ret) && i < pending.size(); ++i) {
      const uint64_t id = pending[i];
      if (std::find(visited.begin(), visited.end(), id) != visited.end()) { continue; }
      visited.push_back(id);
      for (size_t j = 0; j < tasks.size(); ++j) {
        if (tasks[j].column == id) { targets.push_back(j); }
      }
      const auto *column = table.get_column_schema(id);
      if (column != nullptr && column->is_generated_column()) {
        ObSEArray<uint64_t, 8> dependencies;
        if (OB_FAIL(column->get_cascaded_column_ids(dependencies))) {}
        for (int64_t j = 0; OB_SUCC(ret) && j < dependencies.count(); ++j) {
          const uint64_t dependency = dependencies.at(j);
          if (std::find(pending.begin(), pending.end(), dependency) == pending.end()) {
            pending.push_back(dependency);
          }
        }
      }
    }
    for (size_t i = 1; i < targets.size(); ++i) { components[root(targets[i])] = root(targets[0]); }
    return ret;
  };
  for (auto it = table.constraint_begin(); OB_SUCC(ret) && it != table.constraint_end(); ++it) {
    ObSEArray<uint64_t, 8> references;
    for (auto col = (*it)->cst_col_begin(); OB_SUCC(ret) && col != (*it)->cst_col_end(); ++col) {
      ret = references.push_back(*col);
    }
    if (OB_SUCC(ret)) { ret = connect(references); }
  }
  const auto &indexes = table.get_simple_index_infos();
  for (int64_t i = 0; OB_SUCC(ret) && i < indexes.count(); ++i) {
    const ObTableSchema *index = nullptr;
    if (OB_FAIL(definition.guard.get_table_schema(indexes.at(i).table_id_, index))) {}
    else if (index == nullptr) { ret = OB_SCHEMA_EAGAIN; }
    else if (index->is_unique_index()) {
      ObSEArray<uint64_t, 8> references;
      for (auto it = index->column_begin(); OB_SUCC(ret) && it != index->column_end(); ++it) {
        if ((*it)->get_index_position() > 0) { ret = references.push_back((*it)->get_column_id()); }
      }
      if (OB_SUCC(ret)) { ret = connect(references); }
    }
  }
  const auto &foreign_keys = table.get_foreign_key_infos();
  for (int64_t i = 0; OB_SUCC(ret) && i < foreign_keys.count(); ++i) {
    const auto &fk = foreign_keys.at(i);
    if (fk.child_table_id_ == table.get_table_id()) { ret = connect(fk.child_column_ids_); }
    if (OB_SUCC(ret) && fk.parent_table_id_ == table.get_table_id()) { ret = connect(fk.parent_column_ids_); }
  }
  for (size_t i = 0; i < components.size(); ++i) { components[i] = root(i); }
  return ret;
}

bool permanent_publication_error(int error)
{
  switch (error) {
    case OB_ERR_PRIMARY_KEY_DUPLICATE:
    case OB_ERR_DUPLICATED_UNIQUE_KEY:
    case OB_ERR_CHECK_CONSTRAINT_VIOLATED:
    case OB_ERR_DATA_TOO_LONG:
    case OB_DATA_OUT_OF_RANGE:
    case OB_ERR_TRUNCATED_WRONG_VALUE:
    case OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD:
    case OB_ERR_INVALID_JSON_TEXT:
    case OB_ERR_INVALID_JSON_VALUE_FOR_CAST:
    case OB_ERR_INVALID_VECTOR_DIM:
      return true;
    default:
      return false; // Lock, transaction and schema errors must remain retryable.
  }
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
    std::string update, condition, qualified;
    std::vector<std::string> column_updates, failed_updates;
    for (size_t i = 0; OB_SUCC(ret) && ready && !stale && i < tasks.size(); ++i) {
      Definition d;
      if (OB_FAIL(d.resolve(tasks[i]))) {
        if (ret == OB_ENTRY_NOT_EXIST) { stale = true; ret = OB_SUCCESS; }
      } else {
        if (i == 0) {
          qualified = d.qualified;
          update = "UPDATE " + qualified + " SET ";
          condition = "(" + group.predicate + ") AND ";
        }
        else { update += ","; condition += " AND "; }
        const bool success = tasks[i].state == State::SUCCEEDED;
        update += d.result_name + "=" + (success && !tasks[i].null_result ? literal(tasks[i].payload) : "NULL")
            + "," + d.state_name + "=" + (success ? "1" : "2");
        const std::string assignment = d.result_name + "="
            + (success && !tasks[i].null_result ? literal(tasks[i].payload) : "NULL")
            + "," + d.state_name + "=" + (success ? "1" : "2");
        column_updates.push_back(assignment);
        failed_updates.push_back(d.result_name + "=NULL," + d.state_name + "=2");
        condition += "(" + d.current_request(tasks[i]) + ")";
      }
    }
    int64_t affected = 0;
    if (OB_SUCC(ret) && ready && !tasks.empty()) {
      if (!stale) {
        if (OB_FAIL(write(trans, "SAVEPOINT ai_column_group", affected))) {}
        else {
          ret = write(trans, update + " WHERE " + condition, affected);
          if (permanent_publication_error(ret)) {
            // CHECK/UNIQUE-coupled columns must never be judged in isolation.
            // Each component is tried once; all changes commit with the group.
            trans.reset_last_error(); // The proxy otherwise rejects recovery SQL after a failed write.
            ret = write(trans, "ROLLBACK TO SAVEPOINT ai_column_group", affected);
            std::vector<size_t> components;
            if (OB_SUCC(ret)) { ret = publication_components(tasks, components); }
            for (size_t i = 0; OB_SUCC(ret) && i < components.size(); ++i) {
              if (components[i] != i) { continue; }
              std::string candidate = "UPDATE " + qualified + " SET ";
              std::string failure = candidate;
              bool first = true;
              for (size_t j = 0; j < components.size(); ++j) {
                if (components[j] == i) {
                  if (!first) { candidate += ","; failure += ","; }
                  candidate += column_updates[j];
                  failure += failed_updates[j];
                  first = false;
                }
              }
              if (OB_FAIL(write(trans, "SAVEPOINT ai_column_candidate", affected))) {}
              else {
                ret = write(trans, candidate + " WHERE " + condition, affected);
                if (permanent_publication_error(ret)) {
                  trans.reset_last_error();
                  if (OB_FAIL(write(trans, "ROLLBACK TO SAVEPOINT ai_column_candidate", affected))) {}
                  else { ret = write(trans, failure + " WHERE " + condition, affected); }
                }
              }
            }
          }
        }
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
        + " t WHERE state IN (2,4) AND eligible_ts<=NOW(6) AND NOT EXISTS (SELECT 1 FROM " + TASKS
        + " n WHERE n.table_id=t.table_id AND n.request_id=t.request_id AND n.rowkey_hash=t.rowkey_hash AND n.rowkey_predicate=t.rowkey_predicate"
          " AND n.state NOT IN (2,4)) ORDER BY eligible_ts LIMIT 1";
    if (OB_FAIL(GCTX.sql_proxy_->read(res, query.c_str()))) {}
    else if (res.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(res.get_result()->next())) { if (ret == OB_ITER_END) { ret = OB_SUCCESS; } }
    else if (OB_FAIL(read_task(*res.get_result(), task))) {}
    else { found = true; }
  }
  if (OB_SUCC(ret) && found) {
    ret = publish(task);
    if (OB_FAIL(ret)) {
      // A blocked group must not monopolize recovery selection or task claiming.
      int64_t affected = 0;
      const int defer_ret = write(*GCTX.sql_proxy_, std::string("UPDATE ") + TASKS
          + " SET eligible_ts=TIMESTAMPADD(SECOND,1,NOW(6)) WHERE " + group_key(task), affected);
      if (defer_ret != OB_SUCCESS) { LOG_WARN("failed to defer AI column publication", K(defer_ret)); }
    }
  }
  return ret;
}
} // namespace
int AiColumnWorker::process_one()
{
  int ret = OB_SUCCESS;
  // Runtime modules start before persisted user schemas finish loading. A task
  // resolved in that window looks stale and would be permanently discarded.
  // Both recovery publication and claiming must wait for complete server startup.
  if (GCTX.status_ != ObServiceStatus::SS_SERVING) { return ret; }
  Task task;
  bool found = false;
  if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
  else {
    const int recovery_ret = recover_publication();
    if (recovery_ret != OB_SUCCESS && recovery_ret != OB_TABLE_NOT_EXIST) {
      LOG_WARN("AI column publication recovery deferred", K(recovery_ret));
    }
    if (OB_FAIL(claim(task, found))) {}
    else if (found && task.predicate.empty()) { ret = backfill(task); }
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
  }
  return ret;
}
void AiColumnWorker::run1()
{
  lib::set_thread_name("AiColumn");
  while (!has_set_stop()) {
    const int ret = process_one();
    if (ret != OB_SUCCESS && ret != OB_TABLE_NOT_EXIST && REACH_TIME_INTERVAL(10000000)) {
      LOG_ERROR("AI column worker iteration failed", K(ret));
    }
    usleep(100000);
  }
}
} }
