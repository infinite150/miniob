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
// Created by Wangyunlai on 2022/6/5.
//

#pragma once

#include "common/sys/rc.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/field/field.h"

class FieldMeta;
class Db;
class Table;

/**
 * @brief 表示select语句
 * @ingroup Statement
 */
class SelectStmt : public Stmt
{
public:
  class JoinTables
  {
  public:
    JoinTables() = default;
    JoinTables(const JoinTables &)            = delete;
    JoinTables &operator=(const JoinTables &) = delete;
    JoinTables(JoinTables &&other) noexcept
        : join_tables_(std::move(other.join_tables_)), on_conds_(std::move(other.on_conds_))
    {}
    JoinTables &operator=(JoinTables &&other) noexcept
    {
      if (this != &other) {
        for (FilterStmt *fu : on_conds_) {
          if (fu != nullptr) {
            delete fu;
          }
        }
        join_tables_ = std::move(other.join_tables_);
        on_conds_    = std::move(other.on_conds_);
      }
      return *this;
    }
    ~JoinTables()
    {
      for (FilterStmt *fu : on_conds_) {
        if (fu != nullptr) {
          delete fu;
        }
      }
      on_conds_.clear();
    }
    void push_join_table(Table *table, FilterStmt *on_filter)
    {
      join_tables_.push_back(table);
      on_conds_.push_back(on_filter);
    }
    const vector<Table *>      &join_tables() const { return join_tables_; }
    const vector<FilterStmt *> &on_conds() const { return on_conds_; }

  private:
    vector<Table *>      join_tables_;
    vector<FilterStmt *> on_conds_;
  };

  SelectStmt() = default;
  ~SelectStmt() override;

  StmtType type() const override { return StmtType::SELECT; }

public:
  static RC create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt,
      const unordered_map<string, Table *> &parent_table_map = {});

public:
  const vector<Table *> &tables() const { return tables_; }
  FilterStmt           *filter_stmt() const { return filter_stmt_; }

  const vector<JoinTables> &join_tables() const { return join_tables_; }

  vector<unique_ptr<Expression>> &query_expressions() { return query_expressions_; }
  vector<unique_ptr<Expression>> &group_by() { return group_by_; }
  unique_ptr<Expression>         &having() { return having_expr_; }
  vector<unique_ptr<Expression>> &order_by() { return order_by_; }
  vector<bool>                   &order_by_asc() { return order_by_asc_; }

private:
  vector<unique_ptr<Expression>> query_expressions_;
  vector<Table *>                tables_;
  FilterStmt                    *filter_stmt_ = nullptr;
  vector<JoinTables>             join_tables_;
  vector<unique_ptr<Expression>> group_by_;
  unique_ptr<Expression>         having_expr_;
  vector<unique_ptr<Expression>> order_by_;
  vector<bool>                   order_by_asc_;
};
