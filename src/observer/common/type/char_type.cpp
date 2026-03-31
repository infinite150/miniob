/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/lang/comparator.h"
#include "common/log/log.h"
#include "common/type/char_type.h"
#include "common/type/data_type.h"
#include "common/value.h"

int CharType::compare(const Value &left, const Value &right) const
{
  if (left.attr_type() != AttrType::CHARS || !is_string_type(right.attr_type())) {
    LOG_WARN("CHAR compare with unexpected operand type. left=%d right=%d", left.attr_type(), right.attr_type());
    const string left_str  = left.to_string();
    const string right_str = right.to_string();
    return common::compare_string(
        (void *)left_str.c_str(), left_str.size(), (void *)right_str.c_str(), right_str.size());
  }
  return common::compare_string((void *)left.data(), left.length(), (void *)right.data(), right.length());
}

RC CharType::set_value_from_str(Value &val, const string &data) const
{
  val.set_string(data.c_str());
  return RC::SUCCESS;
}

RC CharType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::DATES:
      return DataType::type_instance(AttrType::DATES)->set_value_from_str(result, val.get_string());
    case AttrType::TEXTS:
      result.set_text(val.get_string().c_str());
      return RC::SUCCESS;
    default: return RC::UNIMPLEMENTED;
  }
  return RC::SUCCESS;
}

int CharType::cast_cost(AttrType type)
{
  if (type == AttrType::CHARS) {
    return 0;
  }
  if (type == AttrType::TEXTS) {
    return 1;
  }
  if (type == AttrType::DATES) {
    return 1;
  }
  return INT32_MAX;
}

RC CharType::to_string(const Value &val, string &result) const
{
  if (val.value_.pointer_value_ == nullptr) {
    result.clear();
    return RC::SUCCESS;
  }
  stringstream ss;
  ss << val.value_.pointer_value_;
  result = ss.str();
  return RC::SUCCESS;
}