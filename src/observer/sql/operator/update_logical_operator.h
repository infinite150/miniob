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
// Created by GPT on 2026/03/11.
//

#pragma once

#include "sql/operator/logical_operator.h"
#include "common/lang/vector.h"

class FieldMeta;

/**
 * @brief 逻辑算子：更新（与 Insert/Delete 类似，持有表与更新目标；子节点为 TableGet 或 Predicate 提供待更新行）
 * @ingroup LogicalOperator
 */
class UpdateLogicalOperator : public LogicalOperator
{
public:
  UpdateLogicalOperator(Table *table, const vector<const FieldMeta *> &field_metas, const vector<Value> &values);
  virtual ~UpdateLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::UPDATE; }
  OpType              get_op_type() const override { return OpType::LOGICALUPDATE; }

  Table                          *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const vector<Value>             &values() const { return values_; }

private:
  Table                    *table_       = nullptr;
  vector<const FieldMeta *> field_metas_;
  vector<Value>             values_;
};

