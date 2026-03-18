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
// Created by Wangyunlai on 2024/05/30.
//

#include "sql/expr/expression_iterator.h"
#include "sql/expr/expression.h"
#include "common/log/log.h"

using namespace std;

RC ExpressionIterator::iterate_child_expr(Expression &expr, function<RC(unique_ptr<Expression> &)> callback)
{
  RC rc = RC::SUCCESS;

  switch (expr.type()) {
    case ExprType::CAST: {
      auto &cast_expr = static_cast<CastExpr &>(expr);
      rc = callback(cast_expr.child());
    } break;

    case ExprType::COMPARISON: {

      auto &comparison_expr = static_cast<ComparisonExpr &>(expr);
      rc = callback(comparison_expr.left());

      if (OB_SUCC(rc)) {
        rc = callback(comparison_expr.right());
      }

    } break;

    case ExprType::CONJUNCTION: {
      auto &conjunction_expr = static_cast<ConjunctionExpr &>(expr);
      for (auto &child : conjunction_expr.children()) {
        rc = callback(child);
        if (OB_FAIL(rc)) {
          break;
        }
      }
    } break;

    case ExprType::ARITHMETIC: {

      auto &arithmetic_expr = static_cast<ArithmeticExpr &>(expr);
      rc = callback(arithmetic_expr.left());
      if (OB_SUCC(rc)) {
        rc = callback(arithmetic_expr.right());
      }
    } break;

    case ExprType::AGGREGATION: {
      auto &aggregate_expr = static_cast<AggregateExpr &>(expr);
      rc = callback(aggregate_expr.child());
    } break;

    case ExprType::FUNCTION: {
      auto &func_expr = static_cast<FunctionExpr &>(expr);
      for (size_t i = 0; i < func_expr.child_count() && OB_SUCC(rc); i++) {
        rc = callback(func_expr.child(i));
      }
    } break;

    case ExprType::UNBOUND_FUNCTION: {
      auto &func_expr = static_cast<UnboundFunctionExpr &>(expr);
      for (auto &child : func_expr.children()) {
        rc = callback(child);
        if (OB_FAIL(rc)) break;
      }
    } break;

    case ExprType::SUBQUERY: {
      // SubQueryExpr has no child expressions to iterate
    } break;

    case ExprType::VALUE_LIST: {
      auto &vlist_expr = static_cast<ValueListExpr &>(expr);
      for (auto &child : vlist_expr.values_mut()) {
        rc = callback(child);
        if (OB_FAIL(rc)) {
          break;
        }
      }
    } break;

    case ExprType::NONE:
    case ExprType::STAR:
    case ExprType::UNBOUND_FIELD:
    case ExprType::FIELD:
    case ExprType::VALUE: {
      // Do nothing
    } break;

    default: {
      ASSERT(false, "Unknown expression type");
    }
  }

  return rc;
}

void ExpressionIterator::for_each_subquery(Expression &expr, function<void(SubQueryExpr &)> callback)
{
  if (expr.type() == ExprType::SUBQUERY) {
    callback(static_cast<SubQueryExpr &>(expr));
    return;
  }
  auto visitor = [&callback](unique_ptr<Expression> &child) {
    if (child) {
      ExpressionIterator::for_each_subquery(*child, callback);
    }
    return RC::SUCCESS;
  };
  iterate_child_expr(expr, visitor);
}