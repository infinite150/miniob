#include "sql/stmt/create_table_select_stmt.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/parser/parse_defs.h"
#include "storage/db/db.h"

RC CreateTableSelectStmt::create(Db *db, const CreateTableSelectSqlNode &create_table_select, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr || common::is_blank(create_table_select.relation_name.c_str()) ||
      common::is_blank(create_table_select.select_sql.c_str())) {
    LOG_WARN("invalid create table select args");
    return RC::INVALID_ARGUMENT;
  }

  if (db->find_table(create_table_select.relation_name.c_str()) != nullptr) {
    LOG_WARN("table already exists: %s", create_table_select.relation_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  stmt = new CreateTableSelectStmt(
      create_table_select.relation_name, create_table_select.select_sql, create_table_select.attr_infos);
  return RC::SUCCESS;
}
