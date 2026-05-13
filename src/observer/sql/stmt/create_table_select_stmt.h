#pragma once

#include "common/lang/string.h"
#include "sql/stmt/stmt.h"

class Db;

/**
 * @brief create table as select statement
 * @ingroup Statement
 */
class CreateTableSelectStmt : public Stmt
{
public:
  CreateTableSelectStmt(string table_name, string select_sql)
      : table_name_(std::move(table_name)), select_sql_(std::move(select_sql))
  {}
  virtual ~CreateTableSelectStmt() = default;

  StmtType type() const override { return StmtType::CREATE_TABLE_SELECT; }

  const string &table_name() const { return table_name_; }
  const string &select_sql() const { return select_sql_; }

  static RC create(Db *db, const CreateTableSelectSqlNode &create_table_select, Stmt *&stmt);

private:
  string table_name_;
  string select_sql_;
};
