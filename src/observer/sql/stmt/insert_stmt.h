/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
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

#pragma once

#include "common/sys/rc.h"
#include "sql/stmt/stmt.h"
#include "common/lang/vector.h"

class Table;
class Db;

/**
 * @brief 插入语句
 * @ingroup Statement
 */
class InsertStmt : public Stmt
{
public:
  InsertStmt() = default;
  InsertStmt(Table *table, const Value *values, int value_amount);
  InsertStmt(Table *table, vector<vector<Value>> value_rows);

  StmtType type() const override { return StmtType::INSERT; }

public:
  static RC create(Db *db, const InsertSqlNode &insert_sql, Stmt *&stmt);

public:
  Table       *table() const { return table_; }
  const Value *values() const { return values_; }
  int          value_amount() const { return value_amount_; }
  const vector<vector<Value>> &value_rows() const { return value_rows_; }
  bool         is_batch() const { return !value_rows_.empty(); }
  bool         is_insert_select() const { return !select_sql_.empty(); }
  const string &select_sql() const { return select_sql_; }

private:
  Table                *table_        = nullptr;
  const Value          *values_       = nullptr;
  int                   value_amount_ = 0;
  vector<vector<Value>> value_rows_;   ///< 批量插入时使用
  string                select_sql_;   ///< INSERT ... SELECT 子查询 SQL
};
