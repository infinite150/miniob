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
// Created by Wangyunlai on 2021/5/7.
//

#include "condition_filter.h"
#include "common/log/log.h"
#include "common/value.h"
#include "common/type/data_type.h"
#include "storage/record/record_manager.h"
#include "storage/table/table.h"
#include <math.h>
#include <stddef.h>

using namespace common;

ConditionFilter::~ConditionFilter() {}

DefaultConditionFilter::DefaultConditionFilter()
{
  left_.is_attr     = false;
  left_.attr_length = 0;
  left_.attr_offset = 0;

  right_.is_attr     = false;
  right_.attr_length = 0;
  right_.attr_offset = 0;
}
DefaultConditionFilter::~DefaultConditionFilter() {}

RC DefaultConditionFilter::init(const ConDesc &left, const ConDesc &right, AttrType attr_type, CompOp comp_op)
{
  if (comp_op != IS_NULL_OP && comp_op != IS_NOT_NULL_OP) {
    if (attr_type <= AttrType::UNDEFINED || attr_type >= AttrType::MAXTYPE) {
      LOG_ERROR("Invalid condition with unsupported attribute type: %d", attr_type);
      return RC::INVALID_ARGUMENT;
    }
  }

  if (comp_op < EQUAL_TO || comp_op >= NO_OP) {
    LOG_ERROR("Invalid condition with unsupported compare operation: %d", comp_op);
    return RC::INVALID_ARGUMENT;
  }

  left_      = left;
  right_     = right;
  attr_type_ = attr_type;
  comp_op_   = comp_op;
  return RC::SUCCESS;
}

RC DefaultConditionFilter::init(Table &table, const ConditionSqlNode &condition)
{
  const TableMeta &table_meta = table.table_meta();
  ConDesc          left;
  ConDesc          right;
  null_bitmap_offset_ = table_meta.null_bitmap_offset();

  AttrType type_left  = AttrType::UNDEFINED;
  AttrType type_right = AttrType::UNDEFINED;

  if (1 == condition.left_is_attr) {
    left.is_attr                = true;
    const FieldMeta *field_left = table_meta.field(condition.left_attr.attribute_name.c_str());
    if (nullptr == field_left) {
      LOG_WARN("No such field in condition. %s.%s", table.name(), condition.left_attr.attribute_name.c_str());
      return RC::SCHEMA_FIELD_MISSING;
    }
    left.attr_length = field_left->len();
    left.attr_offset = field_left->offset();
    left.field_index = table_meta.field_index_by_offset(left.attr_offset);

    type_left = field_left->type();
  } else {
    left.is_attr = false;
    left.value   = condition.left_value;  // 校验type 或者转换类型
    type_left    = condition.left_value.attr_type();

    left.attr_length = 0;
    left.attr_offset = 0;
  }

  if (1 == condition.right_is_attr) {
    right.is_attr                = true;
    const FieldMeta *field_right = table_meta.field(condition.right_attr.attribute_name.c_str());
    if (nullptr == field_right) {
      LOG_WARN("No such field in condition. %s.%s", table.name(), condition.right_attr.attribute_name.c_str());
      return RC::SCHEMA_FIELD_MISSING;
    }
    right.attr_length = field_right->len();
    right.attr_offset = field_right->offset();
    right.field_index = table_meta.field_index_by_offset(right.attr_offset);
    type_right        = field_right->type();
  } else {
    right.is_attr = false;
    right.value   = condition.right_value;
    type_right    = condition.right_value.attr_type();

    right.attr_length = 0;
    right.attr_offset = 0;
  }

  // IS NULL / IS NOT NULL and NULL-constant comparisons: skip type compatibility check
  if (condition.comp == IS_NULL_OP || condition.comp == IS_NOT_NULL_OP ||
      type_left == AttrType::UNDEFINED || type_right == AttrType::UNDEFINED) {
    AttrType attr_type = (type_left != AttrType::UNDEFINED) ? type_left : type_right;
    return init(left, right, attr_type, condition.comp);
  }

  // 校验和转换
  if (type_left != type_right) {
    RC rc = RC::SUCCESS;

    if (left.is_attr && !right.is_attr && type_left == AttrType::DATES && type_right == AttrType::CHARS) {
      Value cast_value;
      rc = DataType::type_instance(AttrType::DATES)->set_value_from_str(cast_value, right.value.get_string());
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to cast right value from string to date. value=%s",
                 right.value.to_string().c_str());
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      right.value  = std::move(cast_value);
      type_right   = AttrType::DATES;
    }
    else if (right.is_attr && !left.is_attr && type_right == AttrType::DATES && type_left == AttrType::CHARS) {
      Value cast_value;
      rc = DataType::type_instance(AttrType::DATES)->set_value_from_str(cast_value, left.value.get_string());
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to cast left value from string to date. value=%s",
                 left.value.to_string().c_str());
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      left.value = std::move(cast_value);
      type_left  = AttrType::DATES;
    }

    if (type_left != type_right) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  return init(left, right, type_left, condition.comp);
}

bool DefaultConditionFilter::filter(const Record &rec) const
{
  Value left_value;
  Value right_value;

  if (left_.is_attr) {  // value
    const char *bitmap  = rec.data() + null_bitmap_offset_;
    const bool  is_null = (left_.field_index >= 0) && ((bitmap[left_.field_index / 8] & (1 << (left_.field_index % 8))) != 0);
    if (is_null) {
      left_value.reset();
    } else {
      left_value.set_type(attr_type_);
      left_value.set_data(rec.data() + left_.attr_offset, left_.attr_length);
    }
  } else {
    left_value.set_value(left_.value);
  }

  if (right_.is_attr) {
    const char *bitmap  = rec.data() + null_bitmap_offset_;
    const bool  is_null = (right_.field_index >= 0) && ((bitmap[right_.field_index / 8] & (1 << (right_.field_index % 8))) != 0);
    if (is_null) {
      right_value.reset();
    } else {
      right_value.set_type(attr_type_);
      right_value.set_data(rec.data() + right_.attr_offset, right_.attr_length);
    }
  } else {
    right_value.set_value(right_.value);
  }

  if (comp_op_ == IS_NULL_OP) {
    return left_value.is_null();
  }
  if (comp_op_ == IS_NOT_NULL_OP) {
    return !left_value.is_null();
  }

  if (left_value.is_null() || right_value.is_null()) {
    return false;
  }

  int cmp_result = left_value.compare(right_value);

  switch (comp_op_) {
    case EQUAL_TO: return 0 == cmp_result;
    case LESS_EQUAL: return cmp_result <= 0;
    case NOT_EQUAL: return cmp_result != 0;
    case LESS_THAN: return cmp_result < 0;
    case GREAT_EQUAL: return cmp_result >= 0;
    case GREAT_THAN: return cmp_result > 0;

    default: break;
  }

  LOG_PANIC("Never should print this.");
  return cmp_result;  // should not go here
}

CompositeConditionFilter::~CompositeConditionFilter()
{
  if (memory_owner_) {
    delete[] filters_;
    filters_ = nullptr;
  }
}

RC CompositeConditionFilter::init(const ConditionFilter *filters[], int filter_num, bool own_memory)
{
  filters_      = filters;
  filter_num_   = filter_num;
  memory_owner_ = own_memory;
  return RC::SUCCESS;
}
RC CompositeConditionFilter::init(const ConditionFilter *filters[], int filter_num)
{
  return init(filters, filter_num, false);
}

RC CompositeConditionFilter::init(Table &table, const ConditionSqlNode *conditions, int condition_num)
{
  if (condition_num == 0) {
    return RC::SUCCESS;
  }
  if (conditions == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  RC                rc                = RC::SUCCESS;
  ConditionFilter **condition_filters = new ConditionFilter *[condition_num];
  for (int i = 0; i < condition_num; i++) {
    DefaultConditionFilter *default_condition_filter = new DefaultConditionFilter();
    rc                                               = default_condition_filter->init(table, conditions[i]);
    if (rc != RC::SUCCESS) {
      delete default_condition_filter;
      for (int j = i - 1; j >= 0; j--) {
        delete condition_filters[j];
        condition_filters[j] = nullptr;
      }
      delete[] condition_filters;
      condition_filters = nullptr;
      return rc;
    }
    condition_filters[i] = default_condition_filter;
  }
  return init((const ConditionFilter **)condition_filters, condition_num, true);
}

bool CompositeConditionFilter::filter(const Record &rec) const
{
  for (int i = 0; i < filter_num_; i++) {
    if (!filters_[i]->filter(rec)) {
      return false;
    }
  }
  return true;
}
