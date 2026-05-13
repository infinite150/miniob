#pragma once

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "sql/stmt/stmt.h"

struct AttrInfoSqlNode;
class Db;

/**
 * @brief create table as select statement
 * @ingroup Statement
 */
class CreateTableSelectStmt : public Stmt
{
public:
  CreateTableSelectStmt(string table_name, string select_sql,
      vector<AttrInfoSqlNode> attr_infos = {})
      : table_name_(std::move(table_name)), select_sql_(std::move(select_sql)),
        attr_infos_(std::move(attr_infos))
  {}
  virtual ~CreateTableSelectStmt() = default;

  StmtType type() const override { return StmtType::CREATE_TABLE_SELECT; }

  const string                  &table_name() const { return table_name_; }
  const string                  &select_sql() const { return select_sql_; }
  const vector<AttrInfoSqlNode> &attr_infos() const { return attr_infos_; }

  static RC create(Db *db, const CreateTableSelectSqlNode &create_table_select, Stmt *&stmt);

private:
  string                  table_name_;
  string                  select_sql_;
  vector<AttrInfoSqlNode> attr_infos_;
};
