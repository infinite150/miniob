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
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC CountAggregator::accumulate(const Value &value)
{
  if (value_is_null(value)) {
    return RC::SUCCESS;
  }
  count_++;
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  result.set_int(count_);
  return RC::SUCCESS;
}

RC SumAggregator::accumulate(const Value &value)
{
  if (value_is_null(value)) {
    return RC::SUCCESS;
  }
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }

  ASSERT(value.attr_type() == value_.attr_type(),
         "type mismatch. value type: %s, value_.type: %s",
         attr_type_to_string(value.attr_type()),
         attr_type_to_string(value_.attr_type()));

  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value &result)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    result.set_int(0);
    return RC::SUCCESS;
  }
  result = value_;
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  if (value_is_null(value)) {
    return RC::SUCCESS;
  }
  count_++;
  // 参考 database_project：用 get_float() 做数值累加，避免 UNDEFINED 类型导致 Value::add 失败
  float v = value.get_float();
  if (sum_.attr_type() == AttrType::UNDEFINED) {
    sum_.set_float(v);
    return RC::SUCCESS;
  }
  float s = sum_.get_float();
  sum_.set_float(s + v);
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  if (count_ == 0) {
    result.reset();  // AVG 空集返回 NULL，符合 SQL 语义
    return RC::SUCCESS;
  }
  // 参考 database_project：用 get_float() 做除法，result = sum / count
  float avg_val = sum_.get_float() / static_cast<float>(count_);
  result.set_float(avg_val);
  return RC::SUCCESS;
}

RC MaxAggregator::accumulate(const Value &value)
{
  if (value_is_null(value)) {
    return RC::SUCCESS;
  }
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  if (value.compare(value_) > 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value &result)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    result.reset();  // 全 NULL 时返回 NULL
    return RC::SUCCESS;
  }
  result = value_;
  return RC::SUCCESS;
}

RC MinAggregator::accumulate(const Value &value)
{
  if (value_is_null(value)) {
    return RC::SUCCESS;
  }
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  if (value.compare(value_) < 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value &result)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    result.reset();  // 全 NULL 时返回 NULL
    return RC::SUCCESS;
  }
  result = value_;
  return RC::SUCCESS;
}
