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
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

UpdateStmt::UpdateStmt(Table *table, vector<const FieldMeta *> field_metas, vector<unique_ptr<Expression>> value_exprs, FilterStmt *filter_stmt)
    : table_(table), field_metas_(std::move(field_metas)), value_exprs_(std::move(value_exprs)), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (filter_stmt_ != nullptr) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  stmt = nullptr;
  RC rc = RC::SUCCESS;
  const char *table_name = update.relation_name.c_str();
  if (db == nullptr || common::is_blank(table_name)) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  vector<const FieldMeta *> field_metas;
  vector<unique_ptr<Expression>> value_exprs_to_set;

  if (!update.update_exprs.empty()) {
    for (const auto &p : update.update_exprs) {
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

      unique_ptr<Expression> expr(p.second);  // take ownership
      // For constant VALUE expr, we can validate/cast early (still enforced again at execution time).
      if (expr && expr->type() == ExprType::VALUE) {
        const Value &v = static_cast<ValueExpr *>(expr.get())->get_value();
        if (v.is_null()) {
          if (!field_meta->nullable()) {
            LOG_WARN("update null into non-nullable field. table=%s, field=%s", table_name, field_name);
            return RC::INVALID_ARGUMENT;
          }
        } else {
          if ((field_meta->type() == AttrType::CHARS || field_meta->type() == AttrType::TEXTS) &&
              v.attr_type() == AttrType::CHARS && v.length() > field_meta->len()) {
            LOG_WARN("update string too long for field. table=%s, field=%s, len=%d, max=%d",
                     table_name, field_name, v.length(), field_meta->len());
            return RC::INVALID_ARGUMENT;
          }
          if (field_meta->type() != v.attr_type()) {
            Value casted;
            rc = Value::cast_to(v, field_meta->type(), casted);
            if (OB_FAIL(rc)) {
              LOG_WARN("field type mismatch. table=%s, field=%s", table_name, field_name);
              return RC::SCHEMA_FIELD_TYPE_MISMATCH;
            }
            expr = make_unique<ValueExpr>(casted);
          }
        }
      }
      field_metas.push_back(field_meta);
      value_exprs_to_set.push_back(std::move(expr));
    }
  } else {
    // legacy single-column update path not used when using expression-based parser
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

  stmt = new UpdateStmt(table, std::move(field_metas), std::move(value_exprs_to_set), filter_stmt);
  return RC::SUCCESS;
}
