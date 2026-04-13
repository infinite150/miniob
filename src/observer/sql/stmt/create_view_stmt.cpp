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

#include "sql/stmt/create_view_stmt.h"

#include <cctype>

#include "common/lang/string.h"
#include "common/lang/unordered_set.h"
#include "common/log/log.h"
#include "sql/parser/parse.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/view_rewriter.h"
#include "storage/db/db.h"

using namespace std;

static string normalize_lower(const string &s)
{
  string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

RC CreateViewStmt::create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr || common::is_blank(create_view.view_name.c_str()) || common::is_blank(create_view.select_sql.c_str())) {
    LOG_WARN("invalid create view args. db=%p, view_name=%s", db, create_view.view_name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  if (db->find_table(create_view.view_name.c_str()) != nullptr || db->find_view(create_view.view_name.c_str()) != nullptr) {
    LOG_WARN("table/view already exists with name=%s", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  ParsedSqlResult parsed_sql_result;
  parse(create_view.select_sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().size() != 1) {
    LOG_WARN("invalid select sql in create view. sql=%s", create_view.select_sql.c_str());
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *select_sql_node = parsed_sql_result.sql_nodes().front().get();
  if (select_sql_node == nullptr || select_sql_node->flag != SCF_SELECT) {
    LOG_WARN("create view only accepts a select statement. sql=%s", create_view.select_sql.c_str());
    return RC::INVALID_ARGUMENT;
  }

  RC rc = rewrite_sql_for_view(db, *select_sql_node);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to rewrite view sql when create view. rc=%s", strrc(rc));
    return rc;
  }

  Stmt *select_stmt_base = nullptr;
  rc                     = SelectStmt::create(db, select_sql_node->selection, select_stmt_base);
  if (OB_FAIL(rc)) {
    LOG_WARN("invalid view select statement. rc=%s, sql=%s", strrc(rc), create_view.select_sql.c_str());
    return rc;
  }

  unique_ptr<Stmt> select_stmt_holder(select_stmt_base);
  if (select_stmt_holder == nullptr || select_stmt_holder->type() != StmtType::SELECT) {
    LOG_WARN("failed to build select stmt for create view");
    return RC::INTERNAL;
  }

  auto *view_select_stmt = static_cast<SelectStmt *>(select_stmt_holder.get());
  const auto &view_exprs = view_select_stmt->query_expressions();
  if (view_exprs.empty()) {
    LOG_WARN("view select has no output columns");
    return RC::INVALID_ARGUMENT;
  }

  vector<string> final_columns;
  if (!create_view.column_names.empty()) {
    if (create_view.column_names.size() != view_exprs.size()) {
      LOG_WARN("create view column count mismatch. user=%zu, output=%zu",
          create_view.column_names.size(), view_exprs.size());
      return RC::INVALID_ARGUMENT;
    }
    final_columns = create_view.column_names;
  } else {
    final_columns.reserve(view_exprs.size());
    for (size_t i = 0; i < view_exprs.size(); i++) {
      string name = view_exprs[i] != nullptr && view_exprs[i]->name() != nullptr ? view_exprs[i]->name() : "";
      if (name.empty()) {
        name = "c" + to_string(i + 1);
      }
      final_columns.emplace_back(std::move(name));
    }
  }

  unordered_set<string> seen_columns;
  for (const string &column : final_columns) {
    if (common::is_blank(column.c_str())) {
      LOG_WARN("empty column name in create view");
      return RC::INVALID_ARGUMENT;
    }
    const string key = normalize_lower(column);
    if (seen_columns.count(key) != 0) {
      LOG_WARN("duplicate column name in view definition: %s", column.c_str());
      return RC::INVALID_ARGUMENT;
    }
    seen_columns.insert(key);
  }

  stmt = new CreateViewStmt(create_view.view_name, std::move(final_columns), create_view.select_sql);
  return RC::SUCCESS;
}
