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
#include "sql/expr/expression.h"

class Table;
class FilterStmt;
class FieldMeta;

/**
 * @brief 更新语句
 * @ingroup Statement
 */
class UpdateStmt : public Stmt
{
public:
  UpdateStmt() = default;
  UpdateStmt(Table *table,
             vector<const FieldMeta *> field_metas,
             vector<unique_ptr<Expression>> value_expressions,
             FilterStmt *filter_stmt);
  ~UpdateStmt() override;

  StmtType type() const override { return StmtType::UPDATE; }

public:
  static RC create(Db *db, UpdateSqlNode &update_sql, Stmt *&stmt);

public:
  Table *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const vector<unique_ptr<Expression>> &value_expressions() const { return value_expressions_; }
  vector<unique_ptr<Expression>> &value_expressions_mut() { return value_expressions_; }
  const FieldMeta *field_meta() const { return field_metas_.empty() ? nullptr : field_metas_[0]; }
  FilterStmt      *filter_stmt() const { return filter_stmt_; }

private:
  Table                    *table_        = nullptr;
  vector<const FieldMeta *> field_metas_;
  vector<unique_ptr<Expression>> value_expressions_;
  FilterStmt               *filter_stmt_ = nullptr;
};
