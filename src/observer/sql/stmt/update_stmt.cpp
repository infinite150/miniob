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
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"
#include "sql/expr/expression.h"

UpdateStmt::UpdateStmt(
    Table *table, vector<const FieldMeta *> field_metas, vector<unique_ptr<Expression>> rhs_exprs, FilterStmt *filter_stmt)
    : table_(table), field_metas_(std::move(field_metas)), rhs_exprs_(std::move(rhs_exprs)), filter_stmt_(filter_stmt)
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

  Table *table = db->find_table(table_name);
  if (table == nullptr) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  unordered_map<string, Table *> table_map;
  table_map.emplace(string(table_name), table);

  vector<const FieldMeta *> field_metas;
  vector<unique_ptr<Expression>> rhs_exprs;

  auto bind_one = [&](const char *field_name, unique_ptr<Expression> expr) -> RC {
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
    rc = prepare_subquery_stmts(expr.get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_WARN("prepare subqueries in SET failed. rc=%s", strrc(rc));
      return rc;
    }
    BinderContext binder_context;
    binder_context.add_table(table);
    ExpressionBinder binder(binder_context);
    vector<unique_ptr<Expression>> bound;
    rc = binder.bind_expression(expr, bound);
    if (OB_FAIL(rc) || bound.size() != 1) {
      LOG_WARN("bind SET expression failed. rc=%s", strrc(rc));
      return rc != RC::SUCCESS ? rc : RC::INVALID_ARGUMENT;
    }
    unique_ptr<Expression> bound_expr = std::move(bound[0]);
    if (bound_expr->type() == ExprType::VALUE) {
      Value value_to_set = static_cast<ValueExpr *>(bound_expr.get())->get_value();
      if (value_to_set.is_null()) {
        if (!field_meta->nullable()) {
          LOG_WARN("cannot set NULL on NOT NULL field. table=%s, field=%s", table_name, field_name);
          return RC::INVALID_ARGUMENT;
        }
      } else if (field_meta->type() != value_to_set.attr_type()) {
        Value casted;
        rc = Value::cast_to(value_to_set, field_meta->type(), casted);
        if (OB_FAIL(rc)) {
          LOG_WARN("field type mismatch. table=%s, field=%s", table_name, field_name);
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        bound_expr = make_unique<ValueExpr>(casted);
      }
    }
    field_metas.push_back(field_meta);
    rhs_exprs.push_back(std::move(bound_expr));
    return RC::SUCCESS;
  };

  if (!update.updates.empty()) {
    for (auto &p : update.updates) {
      unique_ptr<Expression> e(p.second);
      p.second = nullptr;
      rc       = bind_one(p.first.c_str(), std::move(e));
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    update.updates.clear();
  } else {
    const char *field_name = update.attribute_name.c_str();
    if (common::is_blank(field_name)) {
      LOG_WARN("invalid argument. field_name is blank");
      return RC::INVALID_ARGUMENT;
    }
    unique_ptr<Expression> e = make_unique<ValueExpr>(update.value);
    rc                       = bind_one(field_name, std::move(e));
    if (OB_FAIL(rc)) {
      return rc;
    }
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

  stmt = new UpdateStmt(table, std::move(field_metas), std::move(rhs_exprs), filter_stmt);
  return RC::SUCCESS;
}
