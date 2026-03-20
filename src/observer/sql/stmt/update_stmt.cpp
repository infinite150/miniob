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

#include "sql/stmt/update_stmt.h"
#include "common/lang/unordered_map.h"
#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/value.h"
#include "sql/expr/expression.h"
#include "sql/parser/expression_binder.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::UpdateStmt(Table *table, vector<const FieldMeta *> field_metas, vector<unique_ptr<Expression>> assignment_exprs,
    FilterStmt *filter_stmt)
    : table_(table), field_metas_(std::move(field_metas)), assignment_exprs_(std::move(assignment_exprs)), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (filter_stmt_ != nullptr) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC UpdateStmt::create(Db *db, UpdateSqlNode &update, Stmt *&stmt)
{
  stmt = nullptr;
  RC rc = RC::SUCCESS;
  const char *table_name = update.relation_name.c_str();
  if (db == nullptr || common::is_blank(table_name)) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  vector<const FieldMeta *> field_metas;
  vector<unique_ptr<Expression>> assignment_exprs;

  if (update.updates.empty()) {
    LOG_WARN("UpdateStmt: empty SET list");
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(table_name);
  if (table == nullptr) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  unordered_map<string, Table *> table_map;
  table_map.emplace(string(table_name), table);

  BinderContext binder_context;
  binder_context.add_table(table);

  for (auto &p : update.updates) {
    const char *field_name = p.first.c_str();
    if (common::is_blank(field_name)) {
      LOG_WARN("invalid blank field name");
      return RC::INVALID_ARGUMENT;
    }
    const TableMeta &table_meta = table->table_meta();
    const FieldMeta *field_meta = table_meta.field(field_name);
    if (field_meta == nullptr) {
      LOG_WARN("no such field. table=%s, field=%s", table_name, field_name);
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    if (!field_meta->visible()) {
      LOG_WARN("cannot update invisible(system) field. table=%s, field=%s", table_name, field_name);
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> expr = std::move(p.second);
    if (expr == nullptr) {
      LOG_WARN("null expression in SET");
      return RC::INVALID_ARGUMENT;
    }

    ExpressionBinder binder(binder_context);
    vector<unique_ptr<Expression>> bound;
    rc = binder.bind_expression(expr, bound);
    if (OB_FAIL(rc)) {
      LOG_WARN("bind update expression failed. rc=%s", strrc(rc));
      return rc;
    }
    if (bound.size() != 1) {
      LOG_WARN("bind update expression: expected 1 bound expression");
      return RC::INVALID_ARGUMENT;
    }

    rc = FilterStmt::prepare_subqueries_in_expression(bound[0].get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_WARN("prepare subqueries in update SET failed. rc=%s", strrc(rc));
      return rc;
    }

    if (bound[0]->type() == ExprType::VALUE) {
      ValueExpr *ve = static_cast<ValueExpr *>(bound[0].get());
      Value v       = ve->get_value();
      if (v.is_null()) {
        if (!field_meta->nullable()) {
          LOG_WARN("cannot set NULL on NOT NULL field. table=%s, field=%s", table_name, field_name);
          return RC::INVALID_ARGUMENT;
        }
      } else if (field_meta->type() != v.attr_type()) {
        Value casted;
        rc = Value::cast_to(v, field_meta->type(), casted);
        if (OB_FAIL(rc)) {
          LOG_WARN("field type mismatch. table=%s, field=%s", table_name, field_name);
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        bound[0] = make_unique<ValueExpr>(casted);
      }
    }

    field_metas.push_back(field_meta);
    assignment_exprs.push_back(std::move(bound[0]));
  }

  FilterStmt *filter_stmt = nullptr;
  rc = FilterStmt::create(db,
      table,
      &table_map,
      update.conditions.data(),
      static_cast<int>(update.conditions.size()),
      filter_stmt);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create filter statement. rc=%s", strrc(rc));
    return rc;
  }

  stmt = new UpdateStmt(table, std::move(field_metas), std::move(assignment_exprs), filter_stmt);
  return RC::SUCCESS;
}
