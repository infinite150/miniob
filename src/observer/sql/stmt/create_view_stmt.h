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
// Created by Codex on 2026/4/13.
//

#pragma once

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "sql/stmt/stmt.h"

class Db;

/**
 * @brief create view statement
 * @ingroup Statement
 */
class CreateViewStmt : public Stmt
{
public:
  CreateViewStmt(string view_name, vector<string> column_names, string select_sql)
      : view_name_(std::move(view_name)), column_names_(std::move(column_names)), select_sql_(std::move(select_sql))
  {}
  virtual ~CreateViewStmt() = default;

  StmtType type() const override { return StmtType::CREATE_VIEW; }

  const string         &view_name() const { return view_name_; }
  const vector<string> &column_names() const { return column_names_; }
  const string         &select_sql() const { return select_sql_; }

  static RC create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt);

private:
  string         view_name_;
  vector<string> column_names_;
  string         select_sql_;
};

