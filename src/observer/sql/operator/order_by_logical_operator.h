/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/logical_operator.h"

/**
 * @brief ORDER BY 逻辑算子：在投影之前对行排序（排序键基于下层算子输出的行）
 */
class OrderByLogicalOperator : public LogicalOperator
{
public:
  OrderByLogicalOperator(vector<unique_ptr<Expression>> &&order_exprs, vector<bool> &&asc_flags);

  ~OrderByLogicalOperator() override = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType              get_op_type() const override { return OpType::ORDERBY; }

  auto &order_exprs() { return order_exprs_; }
  auto &asc_flags() { return asc_flags_; }

private:
  vector<unique_ptr<Expression>> order_exprs_;
  vector<bool>                   asc_flags_;
};
