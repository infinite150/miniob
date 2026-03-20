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
#include "sql/stmt/filter_stmt.h"
#include "sql/parser/expression_binder.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/expr/expression.h"

UpdateStmt::UpdateStmt(Table *table, vector<const FieldMeta *> field_metas,
    vector<std::unique_ptr<Expression>> set_exprs, FilterStmt *filter_stmt)
    : table_(table),
      field_metas_(std::move(field_metas)),
      set_exprs_(std::move(set_exprs)),
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

  vector<const FieldMeta *>               field_metas;
  vector<std::unique_ptr<Expression>>    set_exprs;

  Table *table = db->find_table(table_name);
  if (table == nullptr) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta = table->table_meta();
  if (!update.updates.empty()) {
    for (auto &p : update.updates) {
      const char *field_name = p.first.c_str();
      if (common::is_blank(field_name)) {
        LOG_WARN("invalid blank field name");
        return RC::INVALID_ARGUMENT;
      }

      const FieldMeta *field_meta = table_meta.field(field_name);
      if (field_meta == nullptr) {
        LOG_WARN("no such field. table=%s, field=%s", table_name, field_name);
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      if (!field_meta->visible()) {
        LOG_WARN("cannot update invisible(system) field. table=%s, field=%s", table_name, field_name);
        return RC::INVALID_ARGUMENT;
      }

      Expression *expr = p.second.get();
      if (expr == nullptr) {
        LOG_WARN("update set expression is null. table=%s, field=%s", table_name, field_name);
        return RC::INVALID_ARGUMENT;
      }

      field_metas.push_back(field_meta);
      set_exprs.emplace_back(std::move(p.second)); // take ownership
    }
  } else {
    // 兼容旧语法的兜底：直接用 ValueExpr 包装常量
    const char *field_name = update.attribute_name.c_str();
    if (common::is_blank(field_name)) {
      LOG_WARN("invalid argument. field_name is blank");
      return RC::INVALID_ARGUMENT;
    }

    const FieldMeta *field_meta = table_meta.field(field_name);
    if (field_meta == nullptr) {
      LOG_WARN("no such field. table=%s, field=%s", table_name, field_name);
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    if (!field_meta->visible()) {
      LOG_WARN("cannot update invisible(system) field. table=%s, field=%s", table_name, field_name);
      return RC::INVALID_ARGUMENT;
    }

    field_metas.push_back(field_meta);
    set_exprs.emplace_back(std::make_unique<ValueExpr>(update.value));
  }

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

  // 对 UPDATE SET 表达式做绑定（至少保证 RelAttr/子查询语义正确）
  BinderContext binder_context;
  binder_context.add_table(table);
  ExpressionBinder expression_binder(binder_context);

  vector<std::unique_ptr<Expression>> bound_set_exprs;
  bound_set_exprs.reserve(set_exprs.size());
  for (auto &expr_ptr : set_exprs) {
    vector<std::unique_ptr<Expression>> bound;
    unique_ptr<Expression> &expr_ref = expr_ptr;
    rc = expression_binder.bind_expression(expr_ref, bound);
    if (OB_FAIL(rc) || bound.size() != 1) {
      LOG_WARN("failed to bind update set expression. rc=%s", strrc(rc));
      return rc;
    }
    bound_set_exprs.emplace_back(std::move(bound[0]));
  }

  stmt = new UpdateStmt(table, std::move(field_metas), std::move(bound_set_exprs), filter_stmt);
  return RC::SUCCESS;
}
