/* Copyright (c) 2021OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/insert_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

InsertStmt::InsertStmt(Table *table, const Value *values, int value_amount)
    : table_(table), values_(values), value_amount_(value_amount)
{}

InsertStmt::InsertStmt(Table *table, vector<vector<Value>> value_rows)
    : table_(table), value_rows_(std::move(value_rows))
{}

RC InsertStmt::create(Db *db, const InsertSqlNode &inserts, Stmt *&stmt)
{
  const char *table_name = inserts.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // INSERT ... SELECT：先创建 stmt，select 由 ExecuteStage 执行
  if (!inserts.select_sql.empty()) {
    InsertStmt *insert_stmt = new InsertStmt();
    insert_stmt->table_      = table;
    insert_stmt->select_sql_ = inserts.select_sql;
    stmt                     = insert_stmt;
    return RC::SUCCESS;
  }

  const TableMeta &table_meta = table->table_meta();
  const int        field_num  = table_meta.field_num() - table_meta.sys_field_num();

  if (!inserts.value_rows.empty()) {
    for (const auto &row : inserts.value_rows) {
      if (static_cast<int>(row.size()) != field_num) {
        LOG_WARN("schema mismatch. value num=%zu, field num=%d", row.size(), field_num);
        return RC::SCHEMA_FIELD_MISSING;
      }
    }
    stmt = new InsertStmt(table, inserts.value_rows);
    return RC::SUCCESS;
  }

  if (inserts.values.empty()) {
    LOG_WARN("invalid argument. value_num=0");
    return RC::INVALID_ARGUMENT;
  }

  const Value *values    = inserts.values.data();
  const int    value_num = static_cast<int>(inserts.values.size());
  if (field_num != value_num) {
    LOG_WARN("schema mismatch. value num=%d, field num=%d", value_num, field_num);
    return RC::SCHEMA_FIELD_MISSING;
  }

  stmt = new InsertStmt(table, values, value_num);
  return RC::SUCCESS;
}
