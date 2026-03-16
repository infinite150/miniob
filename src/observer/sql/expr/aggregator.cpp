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

RC SumAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  result = value_;
  return RC::SUCCESS;
}

RC CountAggregator::accumulate(const Value & /*value*/)
{
  if (count_ < 0) {
    count_ = 0;
  }
  count_ += 1;
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  result.set_int(count_);
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  AttrType type = value.attr_type();
  if (type == AttrType::INTS) {
    sum_ += static_cast<float>(value.get_int());
  } else if (type == AttrType::FLOATS) {
    sum_ += value.get_float();
  } else {
    LOG_WARN("unsupported attr type for AVG aggregator: %d", static_cast<int>(type));
    return RC::UNIMPLEMENTED;
  }
  count_ += 1;
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  if (count_ <= 0) {
    // 没有行时返回 0.0，简单题场景下足够
    result.set_float(0.0f);
  } else {
    result.set_float(sum_ / static_cast<float>(count_));
  }
  return RC::SUCCESS;
}

RC MaxAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }

  if (value.attr_type() != value_.attr_type()) {
    LOG_WARN("type mismatch in MAX aggregator. left=%s right=%s",
        attr_type_to_string(value_.attr_type()),
        attr_type_to_string(value.attr_type()));
    return RC::UNIMPLEMENTED;
  }

  if (value.compare(value_) > 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value &result)
{
  result = value_;
  return RC::SUCCESS;
}

RC MinAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }

  if (value.attr_type() != value_.attr_type()) {
    LOG_WARN("type mismatch in MIN aggregator. left=%s right=%s",
        attr_type_to_string(value_.attr_type()),
        attr_type_to_string(value.attr_type()));
    return RC::UNIMPLEMENTED;
  }

  if (value.compare(value_) < 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value &result)
{
  result = value_;
  return RC::SUCCESS;
}
