/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "common/log/log.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/optimizer/rewriter.h"
#include "sql/operator/physical_operator.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "session/session.h"
#include "storage/db/db.h"
#include "storage/trx/trx.h"

using namespace std;

SubQueryExpr::SubQueryExpr(SelectSqlNode &sql_node)
{
  sql_node_ = make_unique<SelectSqlNode>();
  sql_node_->expressions = std::move(sql_node.expressions);
  sql_node_->relations   = std::move(sql_node.relations);
  sql_node_->conditions  = std::move(sql_node.conditions);
  sql_node_->condition_expr = sql_node.condition_expr;
  sql_node.condition_expr   = nullptr;
  sql_node_->group_by = std::move(sql_node.group_by);
}

SubQueryExpr::~SubQueryExpr() = default;

RC SubQueryExpr::generate_select_stmt(Db *db, const unordered_map<string, Table *> &tables)
{
  Stmt *select_stmt = nullptr;
  RC    rc          = SelectStmt::create(db, *sql_node_, select_stmt, tables);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create select stmt for subquery. rc=%s", strrc(rc));
    return rc;
  }
  if (select_stmt->type() != StmtType::SELECT) {
    return RC::INVALID_ARGUMENT;
  }
  SelectStmt *ss = static_cast<SelectStmt *>(select_stmt);
  if (ss->query_expressions().size() > 1) {
    LOG_WARN("subquery must return single column");
    delete ss;
    return RC::INVALID_ARGUMENT;
  }
  stmt_ = unique_ptr<SelectStmt>(ss);
  return RC::SUCCESS;
}

RC SubQueryExpr::generate_logical_oper()
{
  LogicalPlanGenerator generator;
  RC rc = generator.create(stmt_.get(), logical_oper_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("subquery logical oper generate failed. rc=%s", strrc(rc));
    return rc;
  }
  return RC::SUCCESS;
}

RC SubQueryExpr::generate_physical_oper(Session *session)
{
  if (logical_oper_ == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  Rewriter rewriter;
  bool change_made = false;
  do {
    change_made = false;
    RC rc_rewrite = rewriter.rewrite(logical_oper_, change_made);
    if (rc_rewrite != RC::SUCCESS) {
      LOG_WARN("subquery logical plan rewrite failed. rc=%s", strrc(rc_rewrite));
      return rc_rewrite;
    }
  } while (change_made);
  PhysicalPlanGenerator generator;
  RC rc = generator.create(*logical_oper_, physical_oper_, session);
  if (rc != RC::SUCCESS) {
    LOG_WARN("subquery physical oper generate failed. rc=%s", strrc(rc));
    return rc;
  }
  bind_nested_subquery_parents();
  return RC::SUCCESS;
}

void SubQueryExpr::set_parent_tuple_on_plan(const Tuple *tuple) const
{
  if (physical_oper_ != nullptr) {
    physical_oper_->set_parent_tuple(tuple);
  }
}

const Tuple *SubQueryExpr::resolve_parent_tuple(const Tuple &tuple) const
{
  // 深度嵌套相关子查询：调用者传入的 tuple 包含完整上下文（含最外层表如 csq_1）
  // 必须使用调用者 tuple，否则内层子查询无法访问跨层的外层表字段
  return &tuple;
}

const Tuple *SubQueryExpr::get_parent_tuple_from_plan() const
{
  return physical_oper_ != nullptr ? physical_oper_->get_parent_tuple() : nullptr;
}

void SubQueryExpr::bind_nested_subquery_parents()
{
  if (stmt_ == nullptr) {
    return;
  }
  auto bind = [this](Expression &expr) {
    ExpressionIterator::for_each_subquery(expr, [this](SubQueryExpr &inner) { inner.set_parent(this); });
  };
  if (FilterStmt *filter = stmt_->filter_stmt(); filter != nullptr && filter->condition()) {
    bind(*filter->condition());
  }
  for (auto &expr : stmt_->query_expressions()) {
    if (expr) {
      bind(*expr);
    }
  }
  for (auto &expr : stmt_->group_by()) {
    if (expr) {
      bind(*expr);
    }
  }
  for (const auto &jt : stmt_->join_tables()) {
    for (FilterStmt *on : jt.on_conds()) {
      if (on != nullptr && on->condition()) {
        bind(*on->condition());
      }
    }
  }
}

RC SubQueryExpr::open(Trx *trx)
{
  trx_ = trx;
  if (physical_oper_ == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  if (trx == nullptr) {
    LOG_WARN("subquery open with null trx");
    return RC::INVALID_ARGUMENT;
  }
  return physical_oper_->open(trx);
}

RC SubQueryExpr::close()
{
  if (physical_oper_ != nullptr) {
    return physical_oper_->close();
  }
  return RC::SUCCESS;
}

bool SubQueryExpr::has_more_row(const Tuple &tuple) const
{
  if (physical_oper_ == nullptr) {
    return false;
  }
  const Tuple *pt = resolve_parent_tuple(tuple);
  physical_oper_->set_parent_tuple(pt);
  RC rc = physical_oper_->next();
  return rc == RC::SUCCESS;
}

RC SubQueryExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (physical_oper_ == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  const Tuple *pt = resolve_parent_tuple(tuple);
  physical_oper_->set_parent_tuple(pt);
  RC rc = physical_oper_->next();
  if (rc != RC::SUCCESS) {
    if (rc == RC::RECORD_EOF) {
      // Empty subquery: set value for API consistency; caller uses rc to detect
      value.set_null();
    }
    return rc;
  }
  Tuple *row = physical_oper_->current_tuple();
  if (row == nullptr) {
    return RC::INTERNAL;
  }
  return row->cell_at(0, value);
}
