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
  UpdateStmt(Table *table, vector<const FieldMeta *> field_metas, vector<Value> values, FilterStmt *filter_stmt);
  ~UpdateStmt() override;

  StmtType type() const override { return StmtType::UPDATE; }

public:
  static RC create(Db *db, const UpdateSqlNode &update_sql, Stmt *&stmt);

public:
  Table *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const vector<Value>            &values() const { return values_; }
  const FieldMeta *field_meta() const { return field_metas_.empty() ? nullptr : field_metas_[0]; }
  const Value     &value() const { return values_.empty() ? value_empty_ : values_[0]; }
  FilterStmt      *filter_stmt() const { return filter_stmt_; }

private:
  Table                    *table_        = nullptr;
  vector<const FieldMeta *> field_metas_;
  vector<Value>             values_;
  Value                     value_empty_;  ///< 空占位，field_metas_ 为空时 value() 返回引用
  FilterStmt               *filter_stmt_ = nullptr;
};
