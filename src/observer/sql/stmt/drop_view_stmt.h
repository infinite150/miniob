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
#include "sql/stmt/stmt.h"

class Db;

/**
 * @brief drop view statement
 * @ingroup Statement
 */
class DropViewStmt : public Stmt
{
public:
  explicit DropViewStmt(string view_name) : view_name_(std::move(view_name)) {}
  virtual ~DropViewStmt() = default;

  StmtType type() const override { return StmtType::DROP_VIEW; }

  const string &view_name() const { return view_name_; }

  static RC create(Db *db, const DropViewSqlNode &drop_view, Stmt *&stmt);

private:
  string view_name_;
};

