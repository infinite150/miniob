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
// Created by Wangyunlai on 2022/12/30.
//

#include "sql/optimizer/predicate_pushdown_rewriter.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include <strings.h>
#include <unordered_map>
#include <unordered_set>

using namespace std;

static void collect_tables_in_expr(Expression *expr, unordered_set<string> &tables)
{
  if (expr->type() == ExprType::FIELD) {
    auto *field_expr = static_cast<FieldExpr *>(expr);
    if (field_expr->table_name() != nullptr) {
      tables.insert(field_expr->table_name());
    }
  }
  ExpressionIterator::iterate_child_expr(*expr, [&tables](unique_ptr<Expression> &child) {
    if (child) {
      collect_tables_in_expr(child.get(), tables);
    }
    return RC::SUCCESS;
  });
}

static void collect_table_gets(LogicalOperator *oper, unordered_map<string, TableGetLogicalOperator *> &table_map)
{
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    auto *table_get = static_cast<TableGetLogicalOperator *>(oper);
    if (table_get->table() != nullptr) {
      table_map[string(table_get->table()->name())] = table_get;
      // Also register alias (e.g., "t1" for self-joins) so that
      // qualified column references can be matched correctly.
      if (!table_get->alias().empty()) {
        table_map[table_get->alias()] = table_get;
      }
    }
    return;
  }
  for (auto &child : oper->children()) {
    if (child) {
      collect_table_gets(child.get(), table_map);
    }
  }
}

RC PredicatePushdownRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  RC rc = RC::SUCCESS;
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  if (oper->children().size() != 1) {
    return rc;
  }

  unique_ptr<LogicalOperator> &child_oper = oper->children().front();
  if (child_oper->type() != LogicalOperatorType::TABLE_GET &&
      child_oper->type() != LogicalOperatorType::JOIN &&
      child_oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  vector<unique_ptr<Expression>> &predicate_oper_exprs = oper->expressions();
  if (predicate_oper_exprs.size() != 1) {
    return rc;
  }

  unique_ptr<Expression> &predicate_expr = predicate_oper_exprs.front();
  if (!predicate_expr) {
    return rc;
  }

  if (child_oper->type() == LogicalOperatorType::TABLE_GET) {
    auto table_get_oper = static_cast<TableGetLogicalOperator *>(child_oper.get());
    vector<unique_ptr<Expression>> pushdown_exprs;
    const char *table_name = table_get_oper->table() != nullptr ? table_get_oper->table()->name() : nullptr;
    rc = get_exprs_can_pushdown(predicate_expr, pushdown_exprs, table_name);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get pushdown expressions. rc=%s", strrc(rc));
      return rc;
    }

    if (!predicate_expr || is_empty_predicate(predicate_expr)) {
      LOG_TRACE("all expressions of predicate operator were pushdown to table get operator, then make a fake one");
      Value value((bool)true);
      predicate_expr = unique_ptr<Expression>(new ValueExpr(value));
    }

    if (!pushdown_exprs.empty()) {
      change_made = true;
      table_get_oper->set_predicates(std::move(pushdown_exprs));
    }
    return rc;
  }

  if (child_oper->type() == LogicalOperatorType::JOIN || child_oper->type() == LogicalOperatorType::PREDICATE) {
    if (predicate_expr->type() != ExprType::CONJUNCTION) {
      return rc;
    }
    auto *conjunction_expr = static_cast<ConjunctionExpr *>(predicate_expr.get());
    if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::OR) {
      return rc;
    }

    unordered_map<string, TableGetLogicalOperator *> table_map;
    collect_table_gets(child_oper.get(), table_map);

    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
    bool any_pushed = false;

    for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
      unique_ptr<Expression> &conjunct = *iter;
      if (!conjunct) {
        ++iter;
        continue;
      }

      unordered_set<string> tables;
      collect_tables_in_expr(conjunct.get(), tables);

      if (tables.size() == 1) {
        string table_name = *tables.begin();
        TableGetLogicalOperator *table_get = nullptr;
        for (auto &kv : table_map) {
          if (strcasecmp(kv.first.c_str(), table_name.c_str()) == 0) {
            table_get = kv.second;
            break;
          }
        }
        if (table_get != nullptr) {
          table_get->predicates().push_back(std::move(conjunct));
          any_pushed = true;
        }
      }
      ++iter;
    }

    if (any_pushed) {
      change_made = true;
      for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
        if (!*iter) {
          iter = child_exprs.erase(iter);
        } else {
          ++iter;
        }
      }
      if (conjunction_expr->children().empty()) {
        Value value((bool)true);
        predicate_expr = unique_ptr<Expression>(new ValueExpr(value));
      }
    }
  }
  return rc;
}

bool PredicatePushdownRewriter::is_empty_predicate(unique_ptr<Expression> &expr)
{
  bool bool_ret = false;
  if (!expr) {
    return true;
  }

  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    if (conjunction_expr->children().empty()) {
      bool_ret = true;
    }
  }

  return bool_ret;
}

/**
 * 查看表达式是否可以直接下放到table get算子的filter
 * @param expr 是当前的表达式。如果可以下放给table get 算子，执行完成后expr就失效了
 * @param pushdown_exprs 当前所有要下放给table get 算子的filter。此函数执行多次，
 *                       pushdown_exprs 只会增加，不要做清理操作
 * @param table_name 目标表名，用于过滤相关子查询谓词（引用外层表的谓词不下推）
 */
RC PredicatePushdownRewriter::get_exprs_can_pushdown(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &pushdown_exprs, const char *table_name)
{
  RC rc = RC::SUCCESS;
  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    // OR 谓词不下推，保留在 Predicate 层执行
    if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::OR) {
      return rc;
    }

    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
    for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
      // 对每个子表达式，判断是否可以下放到table get 算子
      // 如果可以的话，就从当前孩子节点中删除他
      rc = get_exprs_can_pushdown(*iter, pushdown_exprs, table_name);
      if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get pushdown expressions. rc=%s", strrc(rc));
      return rc;
    }

      if (!*iter) {
        iter = child_exprs.erase(iter);
      } else {
        ++iter;
      }
    }
  } else if (expr->type() == ExprType::COMPARISON) {
    // 含子查询的谓词不下推，避免相关子查询在 TableScan 中无法正确获取 outer tuple
    bool has_subquery = false;
    ExpressionIterator::for_each_subquery(*expr, [&has_subquery](SubQueryExpr &) { has_subquery = true; });
    if (has_subquery) {
      return rc;
    }
    // 相关子查询谓词不下推：引用非目标表的谓词需保留在 Predicate 层，由 parent_tuple 提供外层行
    if (table_name != nullptr) {
      unordered_set<string> tables;
      collect_tables_in_expr(expr.get(), tables);
      for (const string &t : tables) {
        if (strcasecmp(t.c_str(), table_name) != 0) {
          return rc;  // 引用外层表，不下推
        }
      }
    }
    pushdown_exprs.emplace_back(std::move(expr));
  }
  return rc;
}
