/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/order_by_physical_operator.h"

#include <algorithm>
#include <climits>

#include "common/log/log.h"
#include "sql/expr/expression_iterator.h"

using namespace std;

OrderByPhysicalOperator::OrderByPhysicalOperator(vector<unique_ptr<Expression>> &&order_exprs, vector<bool> &&asc_flags)
    : order_exprs_(std::move(order_exprs)), asc_flags_(std::move(asc_flags))
{}

RC OrderByPhysicalOperator::open(Trx *trx)
{
  ASSERT(children_.size() == 1, "order by operator should have 1 child");
  PhysicalOperator *child = children_[0].get();

  for (unique_ptr<Expression> &expr : order_exprs_) {
    ExpressionIterator::for_each_subquery(*expr, [trx](SubQueryExpr &sq) { sq.set_trx(trx); });
  }

  RC rc = child->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open order-by child. rc=%s", strrc(rc));
    return rc;
  }

  tuples_.clear();
  next_idx_     = 0;
  current_tuple_ = nullptr;

  while (OB_SUCC(rc = child->next())) {
    Tuple *t = child->current_tuple();
    if (t == nullptr) {
      LOG_WARN("null tuple in order by");
      return RC::INTERNAL;
    }
    ValueListTuple row;
    rc = ValueListTuple::make(*t, row);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize tuple for order by. rc=%s", strrc(rc));
      child->close();
      return rc;
    }
    tuples_.push_back(std::move(row));
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("order by child next failed. rc=%s", strrc(rc));
    child->close();
    return rc;
  }

  auto less_than = [this](const ValueListTuple &a, const ValueListTuple &b) -> bool {
    const size_t n = order_exprs_.size();
    for (size_t i = 0; i < n; i++) {
      Value va;
      Value vb;
      RC rc1 = order_exprs_[i]->get_value(a, va);
      RC rc2 = order_exprs_[i]->get_value(b, vb);
      if (rc1 != RC::SUCCESS || rc2 != RC::SUCCESS) {
        return false;
      }
      int c = 0;
      if (va.is_null() && vb.is_null()) {
        c = 0;
      } else if (va.is_null()) {
        c = -1;
      } else if (vb.is_null()) {
        c = 1;
      } else {
        c = va.compare(vb);
        if (c == INT32_MAX) {
          c = 0;
        }
      }
      if (c != 0) {
        if (asc_flags_[i]) {
          return c < 0;
        }
        return c > 0;
      }
    }
    return false;
  };

  stable_sort(tuples_.begin(), tuples_.end(), less_than);
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::next()
{
  if (next_idx_ >= tuples_.size()) {
    return RC::RECORD_EOF;
  }
  current_tuple_ = &tuples_[next_idx_++];
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::close()
{
  tuples_.clear();
  next_idx_      = 0;
  current_tuple_ = nullptr;
  if (!children_.empty()) {
    return children_[0]->close();
  }
  return RC::SUCCESS;
}

Tuple *OrderByPhysicalOperator::current_tuple()
{
  return current_tuple_;
}

RC OrderByPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    return RC::UNIMPLEMENTED;
  }
  return children_[0]->tuple_schema(schema);
}
