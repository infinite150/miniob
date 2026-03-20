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
#include <cstdint>

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
  if (attr_type <= AttrType::UNDEFINED || attr_type >= AttrType::MAXTYPE) {
    LOG_ERROR("Invalid condition with unsupported attribute type: %d", attr_type);
    return RC::INVALID_ARGUMENT;
  }

  if (comp_op < EQUAL_TO || comp_op >= NO_OP) {
    LOG_ERROR("Invalid condition with unsupported compare operation: %d", comp_op);
    return RC::INVALID_ARGUMENT;
  }

  left_      = left;
  right_     = right;
  attr_type_ = attr_type;
  comp_op_   = comp_op;
  null_bitmap_offset_ = -1;
  left_field_idx_     = -1;
  right_field_idx_    = -1;
  return RC::SUCCESS;
}

RC DefaultConditionFilter::init(Table &table, const ConditionSqlNode &condition)
{
  const TableMeta &table_meta = table.table_meta();
  ConDesc          left;
  ConDesc          right;

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
    type_right        = field_right->type();
  } else {
    right.is_attr = false;
    right.value   = condition.right_value;
    type_right    = condition.right_value.attr_type();

    right.attr_length = 0;
    right.attr_offset = 0;
  }

  // 校验和转换
  //  if (!field_type_compare_compatible_table[type_left][type_right]) {
  //    // 不能比较的两个字段， 要把信息传给客户端
  //    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  //  }
  // NOTE：这里原来没有实现不同类型的数据比较，比如整数跟浮点数之间的对比。
  // 这里针对 DATE 与 CHARS 的组合做特殊处理：当字段是 DATE，而常量是字符串时，
  // 尝试把字符串解析成 DATE；解析失败则认为类型不匹配。
  if (type_left != type_right) {
    RC rc = RC::SUCCESS;

    // 左边是字段，右边是常量
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
    // 右边是字段，左边是常量
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

    // 其他类型组合目前仍然认为不支持
    if (type_left != type_right) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }

  RC rc = init(left, right, type_left, condition.comp);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  null_bitmap_offset_ = table_meta.null_bitmap_offset();
  left_field_idx_     = -1;
  right_field_idx_    = -1;
  if (left.is_attr) {
    const FieldMeta *field_left = table_meta.field(condition.left_attr.attribute_name.c_str());
    for (int i = 0; i < table_meta.field_num(); i++) {
      if (table_meta.field(i) == field_left) {
        left_field_idx_ = i;
        break;
      }
    }
  }
  if (right.is_attr) {
    const FieldMeta *field_right = table_meta.field(condition.right_attr.attribute_name.c_str());
    for (int i = 0; i < table_meta.field_num(); i++) {
      if (table_meta.field(i) == field_right) {
        right_field_idx_ = i;
        break;
      }
    }
  }
  return RC::SUCCESS;
}

static bool record_field_is_null(const Record &rec, int bitmap_offset, int field_idx)
{
  if (bitmap_offset < 0 || field_idx < 0) {
    return false;
  }
  const int bitmap_bytes = (field_idx + 8) / 8;
  if (rec.len() < bitmap_offset + bitmap_bytes) {
    return false;
  }
  const uint8_t *bitmap = reinterpret_cast<const uint8_t *>(rec.data() + bitmap_offset);
  const int        byte_index = field_idx / 8;
  const int        bit_index  = field_idx % 8;
  return (bitmap[byte_index] & static_cast<uint8_t>(1U << bit_index)) != 0;
}

bool DefaultConditionFilter::filter(const Record &rec) const
{
  Value left_value;
  Value right_value;

  if (left_.is_attr) {
    if (record_field_is_null(rec, null_bitmap_offset_, left_field_idx_)) {
      left_value.set_null();
    } else {
      left_value.set_type(attr_type_);
      left_value.set_data(rec.data() + left_.attr_offset, left_.attr_length);
    }
  } else {
    left_value.set_value(left_.value);
  }

  if (right_.is_attr) {
    if (record_field_is_null(rec, null_bitmap_offset_, right_field_idx_)) {
      right_value.set_null();
    } else {
      right_value.set_type(attr_type_);
      right_value.set_data(rec.data() + right_.attr_offset, right_.attr_length);
    }
  } else {
    right_value.set_value(right_.value);
  }

  // WHERE：比较遇 NULL 视为未知，不选中该行（与 ComparisonExpr 一致）
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
