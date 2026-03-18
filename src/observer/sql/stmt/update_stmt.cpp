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
#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/value.h"
#include "sql/expr/expression.h"
#include "sql/parser/expression_binder.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::UpdateStmt(Table *table,
                       vector<const FieldMeta *> field_metas,
                       vector<unique_ptr<Expression>> value_expressions,
                       FilterStmt *filter_stmt)
    : table_(table),
      field_metas_(std::move(field_metas)),
      value_expressions_(std::move(value_expressions)),
      filter_stmt_(filter_stmt)
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
  vector<unique_ptr<Expression>> value_expressions;

  if (!update.updates.empty()) {
    for (auto &p : update.updates) {
      const char *field_name = p.first.c_str();
      if (common::is_blank(field_name)) {
        LOG_WARN("invalid blank field name");
        return RC::INVALID_ARGUMENT;
      }
      Table *table = db->find_table(table_name);
      if (table == nullptr) {
        LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
        return RC::SCHEMA_TABLE_NOT_EXIST;
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
      // Bind SET expression (may be ValueExpr or SubQueryExpr)
      unique_ptr<Expression> expr = std::move(p.second);
      if (!expr) {
        return RC::INVALID_ARGUMENT;
      }

      // Only do type/length checks for constant values at stmt time.
      if (expr->type() == ExprType::VALUE) {
        const Value &v = static_cast<ValueExpr *>(expr.get())->get_value();
        if (v.is_null()) {
          if (!field_meta->nullable()) {
            LOG_WARN("update null into non-nullable field. table=%s, field=%s", table_name, field_name);
            return RC::INVALID_ARGUMENT;
          }
        } else {
          if (field_meta->type() == AttrType::CHARS && v.attr_type() == AttrType::CHARS && v.length() > field_meta->len()) {
            LOG_WARN("update string too long for field. table=%s, field=%s, len=%d, max=%d",
                     table_name, field_name, v.length(), field_meta->len());
            return RC::INVALID_ARGUMENT;
          }
        }
      }

      field_metas.push_back(field_meta);
      value_expressions.emplace_back(std::move(expr));
    }
  } else {
    const char *field_name = update.attribute_name.c_str();
    if (common::is_blank(field_name)) {
      LOG_WARN("invalid argument. field_name is blank");
      return RC::INVALID_ARGUMENT;
    }
    Table *table = db->find_table(table_name);
    if (table == nullptr) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
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
    // legacy single-field update: use value_expression if provided
    unique_ptr<Expression> expr = std::move(update.value_expression);
    if (!expr) {
      Value v;
      v.set_null();
      expr = make_unique<ValueExpr>(v);
    }
    field_metas.push_back(field_meta);
    value_expressions.emplace_back(std::move(expr));
  }

  Table *table = db->find_table(table_name);
  unordered_map<string, Table *> table_map;
  table_map.emplace(string(table_name), table);

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

  // Bind expressions against current table context
  BinderContext binder_context;
  binder_context.add_table(table);
  ExpressionBinder binder(binder_context);
  vector<unique_ptr<Expression>> bound;
  bound.reserve(value_expressions.size());
  for (auto &expr : value_expressions) {
    vector<unique_ptr<Expression>> tmp;
    rc = binder.bind_expression(expr, tmp);
    if (OB_FAIL(rc) || tmp.empty()) {
      LOG_WARN("failed to bind update value expression. rc=%s", strrc(rc));
      return rc;
    }
    bound.emplace_back(std::move(tmp[0]));
  }

  stmt = new UpdateStmt(table, std::move(field_metas), std::move(bound), filter_stmt);
  return RC::SUCCESS;
}
