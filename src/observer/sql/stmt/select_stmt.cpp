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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include <cctype>
#include "common/lang/string.h"
#include "common/lang/unordered_set.h"
#include "common/log/log.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"

InnerJoinSqlNode::~InnerJoinSqlNode()
{
  for (Expression *expr : conditions) {
    delete expr;
  }
  conditions.clear();
}

InnerJoinSqlNode &InnerJoinSqlNode::operator=(InnerJoinSqlNode &&other) noexcept
{
  if (this != &other) {
    for (Expression *expr : conditions) {
      delete expr;
    }
    base_relation   = std::move(other.base_relation);
    join_relations = std::move(other.join_relations);
    conditions     = std::move(other.conditions);
  }
  return *this;
}

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

static string alias_key_normalized(const string &alias)
{
  string k;
  k.reserve(alias.size());
  for (char c : alias) {
    k.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  }
  return k;
}

static bool contains_aggregate_expr(Expression *expr)
{
  if (expr == nullptr) {
    return false;
  }
  if (expr->type() == ExprType::AGGREGATION) {
    return true;
  }

  bool has_aggregate = false;
  function<RC(unique_ptr<Expression> &)> dfs = [&](unique_ptr<Expression> &child) -> RC {
    if (child == nullptr || has_aggregate) {
      return RC::SUCCESS;
    }
    if (child->type() == ExprType::AGGREGATION) {
      has_aggregate = true;
      return RC::SUCCESS;
    }
    return ExpressionIterator::iterate_child_expr(*child, dfs);
  };
  ExpressionIterator::iterate_child_expr(*expr, dfs);
  return has_aggregate;
}

static RC process_from_clause(Db *db, vector<Table *> &tables, unordered_map<string, Table *> &table_map,
    BinderContext &binder_context, vector<InnerJoinSqlNode> &from_relations, vector<SelectStmt::JoinTables> &join_tables)
{
  RC rc = RC::SUCCESS;

  unordered_set<string> used_aliases_this_select;

  auto check_dup_alias = [&](const string &alias) -> RC {
    if (alias.empty()) {
      return RC::SUCCESS;
    }
    const string k = alias_key_normalized(alias);
    if (used_aliases_this_select.count(k)) {
      LOG_WARN("duplicate table alias in the same query level: %s", alias.c_str());
      return RC::INVALID_ARGUMENT;
    }
    used_aliases_this_select.insert(k);
    return RC::SUCCESS;
  };

  auto check_and_collect_table = [&](const string &table_name) -> RC {
    if (table_name.empty()) {
      LOG_WARN("invalid argument. relation name is null.");
      return RC::INVALID_ARGUMENT;
    }
    Table *table = db->find_table(table_name.c_str());
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name.c_str());
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
    return RC::SUCCESS;
  };

  for (InnerJoinSqlNode &relations : from_relations) {
    SelectStmt::JoinTables jt;

    rc = check_and_collect_table(relations.base_relation.first);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    Table *base_table = table_map[relations.base_relation.first];
    jt.push_join_table(base_table, nullptr);
    if (!relations.base_relation.second.empty()) {
      rc = check_dup_alias(relations.base_relation.second);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      table_map[relations.base_relation.second] = base_table;
      binder_context.add_table_alias(relations.base_relation.second.c_str(), base_table);
    }

    for (size_t j = 0; j < relations.join_relations.size(); j++) {
      const string &join_table_name = relations.join_relations[j].first;
      rc                            = check_and_collect_table(join_table_name);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      Table *join_table = table_map[join_table_name];
      if (!relations.join_relations[j].second.empty()) {
        rc = check_dup_alias(relations.join_relations[j].second);
        if (rc != RC::SUCCESS) {
          return rc;
        }
        table_map[relations.join_relations[j].second] = join_table;
        binder_context.add_table_alias(relations.join_relations[j].second.c_str(), join_table);
      }

      FilterStmt *on_filter = nullptr;
      if (j < relations.conditions.size() && relations.conditions[j] != nullptr) {
        Expression *cond = relations.conditions[j];
        relations.conditions[j] = nullptr;
        vector<unique_ptr<Expression>> bound;
        ExpressionBinder binder(binder_context);
        unique_ptr<Expression> cond_ptr(cond);
        rc = binder.bind_expression(cond_ptr, bound);
        if (rc != RC::SUCCESS || bound.size() != 1) {
          LOG_WARN("bind ON condition failed");
          return rc != RC::SUCCESS ? rc : RC::INVALID_ARGUMENT;
        }
        rc = FilterStmt::create(db, nullptr, &table_map, bound[0].release(), on_filter);
        if (rc != RC::SUCCESS) {
          LOG_WARN("create ON filter stmt failed");
          return rc;
        }
      }
      jt.push_join_table(join_table, on_filter);
    }
    relations.conditions.clear();
    join_tables.push_back(std::move(jt));
  }

  return RC::SUCCESS;
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt,
    const unordered_map<string, Table *> &parent_table_map)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // 相关子查询：外层表仅用于“带表名前缀”的解析（如 outer_t.id），
  // 不参与未限定字段（如 id）的候选集合，避免与子查询 FROM 表产生歧义。
  for (const auto &p : parent_table_map) {
    binder_context.add_table_alias(p.first.c_str(), p.second);
  }

  vector<Table *>                tables;
  unordered_map<string, Table *> table_map(parent_table_map.begin(), parent_table_map.end());
  vector<JoinTables>             join_tables;

  RC rc = process_from_clause(db, tables, table_map, binder_context, select_sql.relations, join_tables);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  ExpressionBinder expression_binder(binder_context);

  vector<unique_ptr<Expression>> bound_expressions;
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }
  for (unique_ptr<Expression> &e : bound_expressions) {
    rc = prepare_subquery_stmts(e.get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_INFO("prepare subqueries in select list failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }
  for (unique_ptr<Expression> &e : group_by_expressions) {
    rc = prepare_subquery_stmts(e.get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_INFO("prepare subqueries in group by failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  unique_ptr<Expression> having_expression;
  if (select_sql.having_expr != nullptr) {
    vector<unique_ptr<Expression>> bound_having;
    unique_ptr<Expression>         having_ptr(select_sql.having_expr);
    select_sql.having_expr = nullptr;
    rc                     = expression_binder.bind_expression(having_ptr, bound_having);
    if (rc != RC::SUCCESS || bound_having.size() != 1) {
      LOG_WARN("bind having expression failed");
      return rc != RC::SUCCESS ? rc : RC::INVALID_ARGUMENT;
    }

    rc = prepare_subquery_stmts(bound_having[0].get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_INFO("prepare subqueries in having failed. rc=%s", strrc(rc));
      return rc;
    }
    having_expression = std::move(bound_having[0]);
  }

  vector<unique_ptr<Expression>> order_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.order_by_exprs) {
    rc = expression_binder.bind_expression(expression, order_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind order by expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }
  for (unique_ptr<Expression> &e : order_by_expressions) {
    rc = prepare_subquery_stmts(e.get(), db, &table_map);
    if (OB_FAIL(rc)) {
      LOG_INFO("prepare subqueries in order by failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  FilterStmt *filter_stmt = nullptr;
  if (select_sql.condition_expr != nullptr) {
    vector<unique_ptr<Expression>> bound_conds;
    unique_ptr<Expression> cond_ptr(select_sql.condition_expr);
    select_sql.condition_expr = nullptr;
    rc = expression_binder.bind_expression(cond_ptr, bound_conds);
    if (rc != RC::SUCCESS || bound_conds.size() != 1) {
      LOG_WARN("bind condition expression failed");
      return rc;
    }
    if (contains_aggregate_expr(bound_conds[0].get())) {
      LOG_WARN("aggregate function is not allowed in WHERE clause");
      return RC::INVALID_ARGUMENT;
    }
    rc = FilterStmt::create(db, default_table, &table_map, bound_conds[0].release(), filter_stmt);
  } else {
    rc = FilterStmt::create(db,
        default_table,
        &table_map,
        select_sql.conditions.data(),
        static_cast<int>(select_sql.conditions.size()),
        filter_stmt);
  }
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_      = std::move(tables);
  select_stmt->join_tables_ = std::move(join_tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->having_expr_ = std::move(having_expression);
  select_stmt->order_by_.swap(order_by_expressions);
  select_stmt->order_by_asc_ = std::move(select_sql.order_by_asc);
  stmt                         = select_stmt;
  return RC::SUCCESS;
}
