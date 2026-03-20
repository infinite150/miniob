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
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "sql/expr/tuple.h"
#include "sql/expr/arithmetic_operator.hpp"
#include "storage/common/column.h"
#include "common/type/data_type.h"
#include <cmath>
#include <functional>
#include <regex>
#include <string>

using namespace std;

RC StarExpr::get_value(const Tuple &tuple, Value &value) const
{
  // For COUNT(*), return a non-null value so CountAggregator can count each row
  value.set_int(1);
  return RC::SUCCESS;
}

RC StarExpr::get_column(Chunk &chunk, Column &column)
{
  // For COUNT(*), provide a column with count = chunk.rows() for aggregate_state_update_by_column
  Value one;
  one.set_int(1);
  column.init(one, chunk.rows());
  return RC::SUCCESS;
}

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(table_name(), field_name()), value);
}

////////////////////////////////////////////////////////////////////////////////
RC IsNullExpr::get_value(const Tuple &tuple, Value &value) const
{
  Value child_value;
  RC rc = child_->get_value(tuple, child_value);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  bool result = value_is_null(child_value);
  if (is_not_) {
    result = !result;
  }
  value.set_boolean(result);
  return RC::SUCCESS;
}

RC IsNullExpr::try_get_value(Value &value) const
{
  Value child_value;
  RC rc = child_->try_get_value(child_value);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  bool result = value_is_null(child_value);
  if (is_not_) {
    result = !result;
  }
  value.set_boolean(result);
  return RC::SUCCESS;
}

bool FieldExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FIELD) {
    return false;
  }
  const auto &other_field_expr = static_cast<const FieldExpr &>(other);
  return table_name() == other_field_expr.table_name() && field_name() == other_field_expr.field_name();
}

// TODO: 在进行表达式计算时，`chunk` 包含了所有列，因此可以通过 `field_id` 获取到对应列。
// 后续可以优化成在 `FieldExpr` 中存储 `chunk` 中某列的位置信息。
RC FieldExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    column.reference(chunk.column(field().meta()->field_id()));
  }
  return RC::SUCCESS;
}

bool ValueExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::VALUE) {
    return false;
  }
  const auto &other_value_expr = static_cast<const ValueExpr &>(other);
  return value_.compare(other_value_expr.get_value()) == 0;
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

ValueListExpr::ValueListExpr(vector<unique_ptr<Expression>> &&values) : values_(std::move(values)) {}

RC ValueListExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (index_ >= values_.size()) {
    value.set_type(AttrType::UNDEFINED);
    return RC::RECORD_EOF;
  }
  RC rc = values_[index_]->get_value(tuple, value);
  index_++;
  return rc;
}

unique_ptr<Expression> ValueListExpr::copy() const
{
  vector<unique_ptr<Expression>> copies;
  for (const auto &v : values_) {
    copies.push_back(v->copy());
  }
  return make_unique<ValueListExpr>(std::move(copies));
}

RC ValueExpr::get_column(Chunk &chunk, Column &column)
{
  column.init(value_, chunk.rows());
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type) : child_(std::move(child)), cast_type_(cast_type)
{}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const
{
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }
  rc = Value::cast_to(value, cast_type_, cast_value);
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &result) const
{
  Value value;
  RC rc = child_->get_value(tuple, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

RC CastExpr::get_column(Chunk &chunk, Column &column)
{
  Column child_column;
  RC rc = child_->get_column(chunk, child_column);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  column.init(cast_type_, child_column.attr_len());
  for (int i = 0; i < child_column.count(); ++i) {
    Value value = child_column.get_value(i);
    Value cast_value;
    rc = cast(value, cast_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    column.append_value(cast_value);
  }
  return rc;
}

RC CastExpr::try_get_value(Value &result) const
{
  Value value;
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

////////////////////////////////////////////////////////////////////////////////
static void replace_all(std::string &str, const std::string &from, const std::string &to)
{
  if (from.empty()) {
    return;
  }
  size_t pos = 0;
  while (std::string::npos != (pos = str.find(from, pos))) {
    str.replace(pos, from.length(), to);
    pos += to.length();
  }
}
static bool str_like(const Value &left, const Value &right)
{
  std::string raw_str(left.attr_type() == AttrType::CHARS ? left.get_string() : "");
  std::string raw_reg(right.attr_type() == AttrType::CHARS ? right.get_string() : "");
  replace_all(raw_reg, "_", "[^']");
  replace_all(raw_reg, "%", "[^']*");
  std::regex reg(raw_reg.c_str(), std::regex_constants::ECMAScript | std::regex_constants::icase);
  bool res = std::regex_match(raw_str, reg);
  return res;
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right))
{
}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right, bool &result) const
{
  RC rc = RC::SUCCESS;

  if (value_is_null(left) || value_is_null(right)) {
    result = false;
    return RC::SUCCESS;
  }

  if (comp_ == LIKE_OP || comp_ == NOT_LIKE_OP) {
    if (left.attr_type() != AttrType::CHARS || right.attr_type() != AttrType::CHARS) {
      LOG_WARN("[NOT_]LIKE_OP requires both operands to be CHARS type");
      return RC::INVALID_ARGUMENT;
    }
    result = comp_ == LIKE_OP ? str_like(left, right) : !str_like(left, right);
    return rc;
  }

  // DATE vs CHARS: 转换失败（非法日期）应返回 INVALID_ARGUMENT，使查询返回 FAILURE
  if ((left.attr_type() == AttrType::DATES && right.attr_type() == AttrType::CHARS) ||
      (left.attr_type() == AttrType::CHARS && right.attr_type() == AttrType::DATES)) {
    Value tmp_date;
    const Value &chars_val = (left.attr_type() == AttrType::CHARS) ? left : right;
    RC conv_rc = DataType::type_instance(AttrType::DATES)->set_value_from_str(tmp_date, chars_val.get_string());
    if (conv_rc != RC::SUCCESS) {
      return RC::INVALID_ARGUMENT;
    }
  }

  int cmp_result = left.compare(right);
  result         = false;
  switch (comp_) {
    case EQUAL_TO: {
      result = (0 == cmp_result);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

RC ComparisonExpr::try_get_value(Value &cell) const
{
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *  left_value_expr  = static_cast<ValueExpr *>(left_.get());
    ValueExpr *  right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell        = left_value_expr->get_value();
    const Value &right_cell       = right_value_expr->get_value();

    bool value = false;
    RC   rc    = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  if (comp_ == EXISTS_OP || comp_ == NOT_EXISTS_OP) {
    SubQueryExpr *subquery = (right_->type() == ExprType::SUBQUERY) ? static_cast<SubQueryExpr *>(right_.get()) : nullptr;
    if (!subquery) {
      return RC::INVALID_ARGUMENT;
    }
    subquery->set_parent_tuple_on_plan(&tuple);
    rc = subquery->open(subquery->get_trx());
    if (rc != RC::SUCCESS) {
      subquery->close();
      return rc;
    }
    rc = right_->get_value(tuple, value);
    subquery->close();
    if (rc == RC::RECORD_EOF) {
      value.set_boolean(comp_ == NOT_EXISTS_OP);
      return RC::SUCCESS;
    }
    if (rc != RC::SUCCESS) {
      return rc;
    }
    value.set_boolean(comp_ == EXISTS_OP);
    return RC::SUCCESS;
  }

  if (comp_ == IN_OP || comp_ == NOT_IN_OP) {
    Value left_value;
    RC    rc = left_->get_value(tuple, left_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (value_is_null(left_value)) {
      value.set_boolean(false);
      return RC::SUCCESS;
    }
    SubQueryExpr *subquery = (right_->type() == ExprType::SUBQUERY) ? static_cast<SubQueryExpr *>(right_.get()) : nullptr;
    ValueListExpr *vlist = (right_->type() == ExprType::VALUE_LIST) ? static_cast<ValueListExpr *>(right_.get()) : nullptr;
    if (!subquery && !vlist) {
      return RC::INVALID_ARGUMENT;
    }
    if (subquery) {
      subquery->set_parent_tuple_on_plan(&tuple);
      rc = subquery->open(subquery->get_trx());
      if (rc != RC::SUCCESS) {
        subquery->close();
        return rc;
      }
    } else {
      vlist->reset();
    }
    bool found   = false;
    bool has_null = false;
    Value right_value;
    while (RC::SUCCESS == (rc = right_->get_value(tuple, right_value))) {
      if (value_is_null(right_value)) {
        has_null = true;
      } else if (left_value.compare(right_value) == 0) {
        found = true;
        if (comp_ == IN_OP) {
          break;  // IN: early exit when match found
        }
      }
    }
    if (subquery) {
      subquery->close();
    }
    if (rc != RC::RECORD_EOF && rc != RC::SUCCESS) {
      return rc;
    }
    // NOT IN semantics: empty set -> true; has NULL -> false; else -> !found
    value.set_boolean(comp_ == IN_OP ? found : (has_null ? false : !found));
    return RC::SUCCESS;
  }

  Value left_value;
  Value right_value;
  SubQueryExpr *left_subquery  = (left_->type() == ExprType::SUBQUERY) ? static_cast<SubQueryExpr *>(left_.get()) : nullptr;
  SubQueryExpr *right_subquery = (right_->type() == ExprType::SUBQUERY) ? static_cast<SubQueryExpr *>(right_.get()) : nullptr;

  if (left_subquery) {
    left_subquery->set_parent_tuple_on_plan(&tuple);
    rc = left_subquery->open(left_subquery->get_trx());
    if (rc != RC::SUCCESS) {
      left_subquery->close();
      return rc;
    }
  }
  rc = left_->get_value(tuple, left_value);
  if (left_subquery) {
    if (rc == RC::RECORD_EOF) {
      left_value.set_null();
      rc = RC::SUCCESS;
    }
    if (left_subquery->has_more_row(tuple)) {
      left_subquery->close();
      return RC::INVALID_ARGUMENT;  // 标量子查询返回多行，= 只能匹配单值，必须 FAILURE
    }
    left_subquery->close();
  }
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (right_subquery) {
    right_subquery->set_parent_tuple_on_plan(&tuple);
    rc = right_subquery->open(right_subquery->get_trx());
    if (rc != RC::SUCCESS) {
      right_subquery->close();
      return rc;
    }
  }
  rc = right_->get_value(tuple, right_value);
  if (right_subquery) {
    if (rc == RC::RECORD_EOF) {
      right_value.set_null();
      rc = RC::SUCCESS;
    }
    if (right_subquery->has_more_row(tuple)) {
      right_subquery->close();
      return RC::INVALID_ARGUMENT;  // 标量子查询返回多行，只能 0 或 1 行，必须 FAILURE
    }
    right_subquery->close();
  }
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if ((left_subquery || right_subquery) && (left_value.is_null() || right_value.is_null())) {
    value.set_boolean(false);
    return RC::SUCCESS;
  }

  bool bool_value = false;
  rc             = compare_value(left_value, right_value, bool_value);
  if (rc == RC::SUCCESS) {
    value.set_boolean(bool_value);
  }
  return rc;
}

RC ComparisonExpr::eval(Chunk &chunk, vector<uint8_t> &select)
{
  RC     rc = RC::SUCCESS;
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
    if (!((left_column.attr_type() == AttrType::DATES && right_column.attr_type() == AttrType::CHARS) ||
          (left_column.attr_type() == AttrType::CHARS && right_column.attr_type() == AttrType::DATES))) {
      LOG_WARN("cannot compare columns with different types");
      return RC::INTERNAL;
    }
  }
  if (left_column.attr_type() == AttrType::INTS) {
    rc = compare_column<int>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::FLOATS) {
    rc = compare_column<float>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::DATES || right_column.attr_type() == AttrType::DATES) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool  result    = false;
      rc               = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }
  } else if (left_column.attr_type() == AttrType::CHARS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }
  } else {
    LOG_WARN("unsupported data type %d", left_column.attr_type());
    return RC::INTERNAL;
  }
  return rc;
}

template <typename T>
RC ComparisonExpr::compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const
{
  RC rc = RC::SUCCESS;

  bool left_const  = left.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    compare_result<T, true, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else if (left_const && !right_const) {
    compare_result<T, true, false>((T *)left.data(), (T *)right.data(), right.count(), result, comp_);
  } else if (!left_const && right_const) {
    compare_result<T, false, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else {
    compare_result<T, false, false>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &children)
    : conjunction_type_(type), children_(std::move(children))
{}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  Value tmp_value;
  for (const unique_ptr<Expression> &expr : children_) {
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) || (conjunction_type_ == Type::OR && bool_value)) {
      value.set_boolean(bool_value);
      return rc;
    }
  }

  bool default_value = (conjunction_type_ == Type::AND);
  value.set_boolean(default_value);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left, Expression *right)
    : arithmetic_type_(type), left_(left), right_(right)
{}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : arithmetic_type_(type), left_(std::move(left)), right_(std::move(right))
{}

bool ArithmeticExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  auto &other_arith_expr = static_cast<const ArithmeticExpr &>(other);
  return arithmetic_type_ == other_arith_expr.arithmetic_type() && left_->equal(*other_arith_expr.left_) &&
         right_->equal(*other_arith_expr.right_);
}
AttrType ArithmeticExpr::value_type() const
{
  if (!right_) {
    return left_->value_type();
  }

  if ((left_->value_type() == AttrType::INTS) &&
   (right_->value_type() == AttrType::INTS) &&
      arithmetic_type_ != Type::DIV) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  value.set_type(target_type);

  switch (arithmetic_type_) {
    case Type::ADD: {
      Value::add(left_value, right_value, value);
    } break;

    case Type::SUB: {
      Value::subtract(left_value, right_value, value);
    } break;

    case Type::MUL: {
      Value::multiply(left_value, right_value, value);
    } break;

    case Type::DIV: {
      Value::divide(left_value, right_value, value);
    } break;

    case Type::NEGATIVE: {
      Value::negative(left_value, value);
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
RC ArithmeticExpr::execute_calc(
    const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const
{
  RC rc = RC::SUCCESS;
  switch (type) {
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::NEGATIVE:
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(
            (float *)left.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  if (rc == RC::SUCCESS) {
    result.set_count(result.capacity());
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return rc;
  }
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_column(left_column, right_column, column);
}

RC ArithmeticExpr::calc_column(const Column &left_column, const Column &right_column, Column &column) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  column.init(target_type, left_column.attr_len(), max(left_column.count(), right_column.count()));
  bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    column.set_column_type(Column::Type::CONSTANT_COLUMN);
    rc = execute_calc<true, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (left_const && !right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (!left_const && right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
  }
  return rc;
}

RC ArithmeticExpr::try_get_value(Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->try_get_value(left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

////////////////////////////////////////////////////////////////////////////////

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, Expression *child)
    : aggregate_name_(aggregate_name), child_(child)
{}

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child)
    : aggregate_name_(aggregate_name), child_(std::move(child))
{}

////////////////////////////////////////////////////////////////////////////////
AggregateExpr::AggregateExpr(Type type, Expression *child) : aggregate_type_(type), child_(child)
{
  std::function<RC(unique_ptr<Expression> &)> check_has_field;
  check_has_field = [&check_has_field](unique_ptr<Expression> &expr) -> RC {
    if (expr->type() == ExprType::FIELD) {
      return RC::INTERNAL;
    }
    return ExpressionIterator::iterate_child_expr(*expr, check_has_field);
  };
  param_is_constexpr_ = (RC::SUCCESS == ExpressionIterator::iterate_child_expr(*child_, check_has_field));
}

AggregateExpr::AggregateExpr(Type type, unique_ptr<Expression> child) : aggregate_type_(type), child_(std::move(child))
{
  std::function<RC(unique_ptr<Expression> &)> check_has_field;
  check_has_field = [&check_has_field](unique_ptr<Expression> &expr) -> RC {
    if (expr->type() == ExprType::FIELD) {
      return RC::INTERNAL;
    }
    return ExpressionIterator::iterate_child_expr(*expr, check_has_field);
  };
  param_is_constexpr_ = (RC::SUCCESS == ExpressionIterator::iterate_child_expr(*child_, check_has_field));
}

RC AggregateExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    rc = RC::INTERNAL;
  }
  return rc;
}

bool AggregateExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != type()) {
    return false;
  }
  const AggregateExpr &other_aggr_expr = static_cast<const AggregateExpr &>(other);
  return aggregate_type_ == other_aggr_expr.aggregate_type() && child_->equal(*other_aggr_expr.child());
}

unique_ptr<Aggregator> AggregateExpr::create_aggregator() const
{
  switch (aggregate_type_) {
    case Type::COUNT: return make_unique<CountAggregator>();
    case Type::SUM:   return make_unique<SumAggregator>();
    case Type::AVG:   return make_unique<AvgAggregator>();
    case Type::MAX:   return make_unique<MaxAggregator>();
    case Type::MIN:   return make_unique<MinAggregator>();
    default:          return make_unique<CountAggregator>();
  }
}

RC AggregateExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(name()), value);
}

const char *AggregateExpr::get_func_name() const
{
  switch (aggregate_type_) {
    case Type::COUNT: return "count";
    case Type::SUM: return "sum";
    case Type::AVG: return "avg";
    case Type::MAX: return "max";
    case Type::MIN: return "min";
    default: return "unknown";
  }
}

RC AggregateExpr::type_from_string(const char *type_str, AggregateExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "count")) {
    type = Type::COUNT;
  } else if (0 == strcasecmp(type_str, "sum")) {
    type = Type::SUM;
  } else if (0 == strcasecmp(type_str, "avg")) {
    type = Type::AVG;
  } else if (0 == strcasecmp(type_str, "max")) {
    type = Type::MAX;
  } else if (0 == strcasecmp(type_str, "min")) {
    type = Type::MIN;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

UnboundFunctionExpr::UnboundFunctionExpr(const char *func_name, vector<unique_ptr<Expression>> &&children)
    : func_name_(func_name), children_(std::move(children))
{}

unique_ptr<Expression> UnboundFunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> copies;
  for (const auto &c : children_) {
    copies.push_back(c->copy());
  }
  return make_unique<UnboundFunctionExpr>(func_name_.c_str(), std::move(copies));
}

////////////////////////////////////////////////////////////////////////////////

FunctionExpr::FunctionExpr(Type func_type, vector<unique_ptr<Expression>> &&children)
    : func_type_(func_type), children_(std::move(children))
{}

unique_ptr<Expression> FunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> copies;
  for (const auto &c : children_) {
    copies.push_back(c->copy());
  }
  return make_unique<FunctionExpr>(func_type_, std::move(copies));
}

AttrType FunctionExpr::value_type() const
{
  switch (func_type_) {
    case Type::LENGTH: return AttrType::INTS;
    case Type::ROUND: return (children_.size() == 2) ? AttrType::FLOATS : AttrType::INTS;
    case Type::DATE_FORMAT: return AttrType::CHARS;
    default: return AttrType::UNDEFINED;
  }
}

int FunctionExpr::value_length() const
{
  switch (func_type_) {
    case Type::LENGTH: return sizeof(int);
    case Type::ROUND: return (children_.size() == 2) ? sizeof(float) : sizeof(int);
    case Type::DATE_FORMAT: return 32;  // 格式化后的日期字符串最大长度
    default: return -1;
  }
}

RC FunctionExpr::eval_length(const Value &arg, Value &result) const
{
  if (arg.attr_type() != AttrType::CHARS) {
    return RC::INVALID_ARGUMENT;
  }
  int len = static_cast<int>(arg.get_string().length());
  result.set_int(len);
  return RC::SUCCESS;
}

RC FunctionExpr::eval_round(const Value &arg, Value &result) const
{
  if (arg.attr_type() != AttrType::FLOATS) {
    return RC::INVALID_ARGUMENT;
  }
  float val = arg.get_float();
  int rounded = static_cast<int>(roundf(val));
  result.set_int(rounded);
  return RC::SUCCESS;
}

RC FunctionExpr::eval_round(const Value &arg, const Value &precision_arg, Value &result) const
{
  if (arg.attr_type() != AttrType::FLOATS) {
    return RC::INVALID_ARGUMENT;
  }
  int prec = 0;
  if (precision_arg.attr_type() == AttrType::INTS) {
    prec = precision_arg.get_int();
  } else if (precision_arg.attr_type() == AttrType::FLOATS) {
    prec = static_cast<int>(precision_arg.get_float());
  } else {
    return RC::INVALID_ARGUMENT;
  }
  if (prec < 0) {
    return RC::INVALID_ARGUMENT;
  }
  float val = arg.get_float();
  float factor = powf(10.0f, static_cast<float>(prec));
  float rounded = roundf(val * factor) / factor;
  result.set_float(rounded);
  return RC::SUCCESS;
}

RC FunctionExpr::eval_date_format(const Value &date_val, const Value &format_val, Value &result) const
{
  Value actual_date;
  if (date_val.attr_type() == AttrType::DATES) {
    actual_date = date_val;
  } else if (date_val.attr_type() == AttrType::CHARS) {
    RC rc = DataType::type_instance(AttrType::DATES)->set_value_from_str(actual_date, date_val.get_string());
    if (rc != RC::SUCCESS) {
      return RC::INVALID_ARGUMENT;
    }
  } else {
    return RC::INVALID_ARGUMENT;
  }
  if (format_val.attr_type() != AttrType::CHARS) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t encoded = actual_date.get_date();
  int year = encoded / 10000;
  int month = (encoded / 100) % 100;
  int day = encoded % 100;

  static const char *MONTH_NAMES[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
  };

  const string &fmt = format_val.get_string();
  string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] == '%' && i + 1 < fmt.size()) {
      int two_digit = 0;
      switch (fmt[i + 1]) {
        case 'Y': out += std::to_string(year); break;
        case 'y':
          two_digit = (year >= 2000) ? (year - 2000) : (year - 1900);
          out += (two_digit < 10 ? "0" : "") + std::to_string(two_digit);
          break;
        case 'm': out += (month < 10 ? "0" : "") + std::to_string(month); break;
        case 'M': out += (month >= 1 && month <= 12) ? MONTH_NAMES[month - 1] : std::to_string(month); break;
        case 'c': out += std::to_string(month); break;
        case 'd': out += (day < 10 ? "0" : "") + std::to_string(day); break;
        case 'D': {
          string suffix;
          if (day >= 11 && day <= 13) {
            suffix = "th";
          } else {
            switch (day % 10) {
              case 1: suffix = "st"; break;
              case 2: suffix = "nd"; break;
              case 3: suffix = "rd"; break;
              default: suffix = "th"; break;
            }
          }
          out += std::to_string(day) + suffix;
          break;
        }
        case 'e': out += std::to_string(day); break;
        case 'H':
        case 'h':
        case 'i':
        case 's': out += "00"; break;  // date 无时分秒，简化为 00
        default: out += fmt[i + 1]; break;
      }
      i++;
    } else {
      out += fmt[i];
    }
  }
  result.set_string(out.c_str());
  return RC::SUCCESS;
}

RC FunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (func_type_ == Type::LENGTH) {
    if (children_.size() != 1) {
      return RC::INVALID_ARGUMENT;
    }
    Value arg;
    RC rc = children_[0]->get_value(tuple, arg);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    return eval_length(arg, value);
  } else if (func_type_ == Type::ROUND) {
    if (children_.size() == 1) {
      Value arg;
      RC rc = children_[0]->get_value(tuple, arg);
      if (rc != RC::SUCCESS) return rc;
      return eval_round(arg, value);
    } else if (children_.size() == 2) {
      Value arg, prec;
      RC rc = children_[0]->get_value(tuple, arg);
      if (rc != RC::SUCCESS) return rc;
      rc = children_[1]->get_value(tuple, prec);
      if (rc != RC::SUCCESS) return rc;
      return eval_round(arg, prec, value);
    } else {
      return RC::INVALID_ARGUMENT;
    }
  } else if (func_type_ == Type::DATE_FORMAT) {
    if (children_.size() != 2) {
      return RC::INVALID_ARGUMENT;
    }
    Value date_val, format_val;
    RC rc = children_[0]->get_value(tuple, date_val);
    if (rc != RC::SUCCESS) return rc;
    rc = children_[1]->get_value(tuple, format_val);
    if (rc != RC::SUCCESS) return rc;
    return eval_date_format(date_val, format_val, value);
  }
  return RC::INVALID_ARGUMENT;
}

RC FunctionExpr::try_get_value(Value &value) const
{
  if (func_type_ == Type::LENGTH) {
    if (children_.size() != 1) {
      return RC::INVALID_ARGUMENT;
    }
    Value arg;
    RC rc = children_[0]->try_get_value(arg);
    if (rc != RC::SUCCESS) return rc;
    return eval_length(arg, value);
  } else if (func_type_ == Type::ROUND) {
    if (children_.size() == 1) {
      Value arg;
      RC rc = children_[0]->try_get_value(arg);
      if (rc != RC::SUCCESS) return rc;
      return eval_round(arg, value);
    } else if (children_.size() == 2) {
      Value arg, prec;
      RC rc = children_[0]->try_get_value(arg);
      if (rc != RC::SUCCESS) return rc;
      rc = children_[1]->try_get_value(prec);
      if (rc != RC::SUCCESS) return rc;
      return eval_round(arg, prec, value);
    } else {
      return RC::INVALID_ARGUMENT;
    }
  } else if (func_type_ == Type::DATE_FORMAT) {
    if (children_.size() != 2) {
      return RC::INVALID_ARGUMENT;
    }
    Value date_val, format_val;
    RC rc = children_[0]->try_get_value(date_val);
    if (rc != RC::SUCCESS) return rc;
    rc = children_[1]->try_get_value(format_val);
    if (rc != RC::SUCCESS) return rc;
    return eval_date_format(date_val, format_val, value);
  }
  return RC::INVALID_ARGUMENT;
}

RC FunctionExpr::get_column(Chunk &chunk, Column &column)
{
  if (func_type_ == Type::LENGTH) {
    Column arg_col;
    RC rc = children_[0]->get_column(chunk, arg_col);
    if (rc != RC::SUCCESS) return rc;
    column.init(value_type(), value_length(), chunk.rows());
    for (int i = 0; i < chunk.rows(); i++) {
      Value arg_val = arg_col.get_value(i);
      Value result;
      rc = eval_length(arg_val, result);
      if (rc != RC::SUCCESS) return rc;
      rc = column.append_value(result);
      if (rc != RC::SUCCESS) return rc;
    }
    return RC::SUCCESS;
  } else if (func_type_ == Type::ROUND) {
    if (children_.size() == 1) {
      Column arg_col;
      RC rc = children_[0]->get_column(chunk, arg_col);
      if (rc != RC::SUCCESS) return rc;
      column.init(value_type(), value_length(), chunk.rows());
      for (int i = 0; i < chunk.rows(); i++) {
        Value arg_val = arg_col.get_value(i);
        Value result;
        rc = eval_round(arg_val, result);
        if (rc != RC::SUCCESS) return rc;
        rc = column.append_value(result);
        if (rc != RC::SUCCESS) return rc;
      }
      return RC::SUCCESS;
    } else if (children_.size() == 2) {
      Column arg_col, prec_col;
      RC rc = children_[0]->get_column(chunk, arg_col);
      if (rc != RC::SUCCESS) return rc;
      rc = children_[1]->get_column(chunk, prec_col);
      if (rc != RC::SUCCESS) return rc;
      column.init(value_type(), value_length(), chunk.rows());
      for (int i = 0; i < chunk.rows(); i++) {
        Value arg_val = arg_col.get_value(i);
        Value prec_val = prec_col.get_value(i);
        Value result;
        rc = eval_round(arg_val, prec_val, result);
        if (rc != RC::SUCCESS) return rc;
        rc = column.append_value(result);
        if (rc != RC::SUCCESS) return rc;
      }
      return RC::SUCCESS;
    } else {
      return RC::INVALID_ARGUMENT;
    }
  } else if (func_type_ == Type::DATE_FORMAT) {
    Column date_col, format_col;
    RC rc = children_[0]->get_column(chunk, date_col);
    if (rc != RC::SUCCESS) return rc;
    rc = children_[1]->get_column(chunk, format_col);
    if (rc != RC::SUCCESS) return rc;

    column.init(value_type(), value_length(), chunk.rows());
    for (int i = 0; i < chunk.rows(); i++) {
      Value date_val = date_col.get_value(i);
      Value format_val = format_col.get_value(i);
      Value result;
      rc = eval_date_format(date_val, format_val, result);
      if (rc != RC::SUCCESS) return rc;
      rc = column.append_value(result);
      if (rc != RC::SUCCESS) return rc;
    }
    return RC::SUCCESS;
  }
  return RC::INVALID_ARGUMENT;
}

RC FunctionExpr::type_from_string(const char *type_str, Type &type)
{
  if (0 == strcasecmp(type_str, "length")) {
    type = Type::LENGTH;
  } else if (0 == strcasecmp(type_str, "round")) {
    type = Type::ROUND;
  } else if (0 == strcasecmp(type_str, "date_format")) {
    type = Type::DATE_FORMAT;
  } else {
    return RC::INVALID_ARGUMENT;
  }
  return RC::SUCCESS;
}

RC get_value_for_update_assignment(Expression &expr, const Tuple &tuple, Trx *trx, Value &value)
{
  switch (expr.type()) {
    case ExprType::SUBQUERY: {
      auto *sq = static_cast<SubQueryExpr *>(&expr);
      sq->set_trx(trx);
      sq->set_parent_tuple_on_plan(&tuple);
      RC rc = sq->open(trx);
      if (rc != RC::SUCCESS) {
        sq->close();
        return rc;
      }
      rc = sq->get_value(tuple, value);
      if (OB_FAIL(rc)) {
        sq->close();
        return rc;
      }
      if (sq->has_more_row(tuple)) {
        sq->close();
        return RC::INVALID_ARGUMENT;
      }
      sq->close();
      return RC::SUCCESS;
    }
    case ExprType::CAST: {
      auto *cast = static_cast<CastExpr *>(&expr);
      Value child_val;
      RC rc = get_value_for_update_assignment(*cast->child().get(), tuple, trx, child_val);
      if (OB_FAIL(rc)) {
        return rc;
      }
      return cast->apply_cast_to(child_val, value);
    }
    case ExprType::ARITHMETIC: {
      auto *a = static_cast<ArithmeticExpr *>(&expr);
      Value left_val;
      RC rc = get_value_for_update_assignment(*a->left().get(), tuple, trx, left_val);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (a->right()) {
        Value right_val;
        rc = get_value_for_update_assignment(*a->right().get(), tuple, trx, right_val);
        if (OB_FAIL(rc)) {
          return rc;
        }
        return a->calc_value(left_val, right_val, value);
      }
      Value dummy;
      return a->calc_value(left_val, dummy, value);
    }
    default: {
      return expr.get_value(tuple, value);
    }
  }
}
