#include "sql/executor/create_table_select_executor.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/executor/sql_result.h"
#include "sql/operator/physical_operator.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/parser/parse.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/create_table_select_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/view_rewriter.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

using namespace std;

RC CreateTableSelectExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt *stmt = sql_event->stmt();
  ASSERT(stmt->type() == StmtType::CREATE_TABLE_SELECT,
      "invalid stmt type for create table select executor");

  auto *cts_stmt = static_cast<CreateTableSelectStmt *>(stmt);
  Db   *db       = sql_event->session_event()->session()->get_current_db();
  if (db == nullptr) {
    return RC::SCHEMA_DB_NOT_EXIST;
  }

  const string &table_name = cts_stmt->table_name();
  const string &select_sql = cts_stmt->select_sql();

  // 1. Parse the SELECT to determine the output schema
  ParsedSqlResult parsed_sql_result;
  parse(select_sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().size() != 1) {
    LOG_WARN("invalid select sql in create table as select: %s", select_sql.c_str());
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *select_node = parsed_sql_result.sql_nodes().front().get();
  if (select_node == nullptr || select_node->flag != SCF_SELECT) {
    LOG_WARN("create table as select requires a select statement");
    return RC::INVALID_ARGUMENT;
  }

  // Rewrite views in the SELECT
  RC rc = rewrite_sql_for_view(db, *select_node);
  if (OB_FAIL(rc)) {
    return rc;
  }

  // Create SelectStmt to get column info
  Stmt *select_stmt_base = nullptr;
  rc                     = SelectStmt::create(db, select_node->selection, select_stmt_base);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create select stmt for ctas");
    return rc;
  }
  unique_ptr<Stmt> select_stmt_holder(select_stmt_base);
  auto            *view_select_stmt = static_cast<SelectStmt *>(select_stmt_holder.get());

  const auto &query_exprs = view_select_stmt->query_expressions();
  if (query_exprs.empty()) {
    LOG_WARN("ctas select has no output columns");
    return RC::INVALID_ARGUMENT;
  }

  // 2. Derive column definitions from the SELECT output
  vector<AttrInfoSqlNode> attr_infos;
  for (size_t i = 0; i < query_exprs.size(); i++) {
    AttrInfoSqlNode attr;
    // Column name
    const char *expr_name = query_exprs[i] != nullptr ? query_exprs[i]->name() : nullptr;
    if (!common::is_blank(expr_name)) {
      attr.name = expr_name;
    } else {
      attr.name = "c" + to_string(i + 1);
    }

    // Column type from expression
    AttrType value_type = query_exprs[i] != nullptr ? query_exprs[i]->value_type() : AttrType::INTS;
    attr.type   = value_type;
    attr.length = query_exprs[i] != nullptr ? query_exprs[i]->value_length() : 4;
    attr.nullable = false;

    attr_infos.push_back(attr);
  }

  // 3. Create the table
  rc = db->create_table(table_name.c_str(), attr_infos, {}, StorageFormat::ROW_FORMAT);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create table %s for ctas", table_name.c_str());
    return rc;
  }

  // 4. Execute the SELECT and insert results
  //   Build logical plan from the SelectStmt
  unique_ptr<LogicalOperator> logical_oper;
  rc = LogicalPlanGenerator().create(select_stmt_base, logical_oper);
  if (OB_FAIL(rc)) {
    return rc;
  }

  logical_oper->generate_general_child();

  unique_ptr<PhysicalOperator> physical_oper;
  rc = PhysicalPlanGenerator().create(*logical_oper, physical_oper,
      sql_event->session_event()->session());
  if (OB_FAIL(rc)) {
    return rc;
  }

  Table *table = db->find_table(table_name.c_str());
  if (table == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // Execute the physical operator and insert rows
  rc = physical_oper->open(nullptr);
  if (OB_FAIL(rc)) {
    table->sync();
    return rc;
  }

  int inserted = 0;
  while (OB_SUCC(rc = physical_oper->next())) {
    Tuple *tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      break;
    }

    const TableMeta &table_meta      = table->table_meta();
    int              user_field_num  = table_meta.field_num() - table_meta.sys_field_num();
    vector<Value>    values(user_field_num);

    for (int i = 0; i < user_field_num; i++) {
      rc = tuple->cell_at(i, values[i]);
      if (OB_FAIL(rc)) {
        break;
      }
    }
    if (OB_FAIL(rc)) {
      break;
    }

    Record record;
    rc = table->make_record(user_field_num, values.data(), record);
    if (OB_FAIL(rc)) {
      break;
    }

    rc = table->insert_record(record);
    if (OB_FAIL(rc)) {
      break;
    }
    inserted++;
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  physical_oper->close();
  table->sync();

  if (OB_SUCC(rc)) {
    LOG_INFO("ctas: inserted %d rows into %s", inserted, table_name.c_str());
  }
  return rc;
}
