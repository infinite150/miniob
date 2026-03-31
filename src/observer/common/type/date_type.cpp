/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/date_type.h"
#include "common/lang/comparator.h"
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include "common/value.h"
#include "storage/common/column.h"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace
{

static const int DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

inline bool is_leap_year(int year)
{
  if (year % 4 != 0) return false;
  if (year % 100 != 0) return true;
  return (year % 400 == 0);
}

inline int days_in_month(int year, int month)
{
  if (month == 2 && is_leap_year(year)) return 29;
  if (month >= 1 && month <= 12) return DAYS_PER_MONTH[month - 1];
  return 0;
}

/**
 * 将字符串形式的日期解析为内部编码的整数值。
 * 内部编码为 YYYYMMDD 的 int32_t，便于比较与索引。
 */
RC date_parse(const string &str, int32_t &out_value)
{
  if (str.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  const char *s = str.c_str();
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
    len--;
  }
  if (len < 8) {
    return RC::INVALID_ARGUMENT;  // 至少 YYYY-M-D
  }

  int  year = 0, month = 0, day = 0;
  char sep1 = 0, sep2 = 0;
  int  n = 0;
  if (sscanf(s, "%d%c%d%c%d%n", &year, &sep1, &month, &sep2, &day, &n) < 5 || static_cast<size_t>(n) != len) {
    return RC::INVALID_ARGUMENT;
  }

  if (sep1 != '-' || sep2 != '-') {
    return RC::INVALID_ARGUMENT;
  }

  if (month < 1 || month > 12) {
    return RC::INVALID_ARGUMENT;
  }

  int dim = days_in_month(year, month);
  if (day < 1 || day > dim) {
    return RC::INVALID_ARGUMENT;
  }

  int64_t encoded = static_cast<int64_t>(year) * 10000 +
                    static_cast<int64_t>(month) * 100 +
                    static_cast<int64_t>(day);

  if (encoded > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) ||
      encoded < static_cast<int64_t>(std::numeric_limits<int32_t>::min())) {
    return RC::INVALID_ARGUMENT;
  }

  out_value = static_cast<int32_t>(encoded);
  return RC::SUCCESS;
}

inline void encoded_to_ymd(int32_t encoded, int &year, int &month, int &day)
{
  year  = encoded / 10000;
  month = (encoded / 100) % 100;
  day   = encoded % 100;
}

}  // namespace

int DateType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::DATES, "left type is not date");

  int32_t left_val = left.get_date();
  int32_t right_val = 0;

  if (right.attr_type() == AttrType::DATES) {
    right_val = right.get_date();
  } else if (is_string_type(right.attr_type())) {
    Value right_date;
    RC rc = set_value_from_str(right_date, right.get_string());
    if (rc != RC::SUCCESS) {
      return INT32_MAX;
    }
    right_val = right_date.get_date();
  } else {
    return INT32_MAX;
  }

  return common::compare_int(&left_val, &right_val);
}

int DateType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  ASSERT(left.attr_type() == AttrType::DATES, "left type is not date");
  ASSERT(right.attr_type() == AttrType::DATES, "right type is not date");

  const int32_t *l = reinterpret_cast<const int32_t *>(left.data());
  const int32_t *r = reinterpret_cast<const int32_t *>(right.data());

  return common::compare_int((void *)&l[left_idx], (void *)&r[right_idx]);
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
  if (type == AttrType::DATES) {
    if (is_string_type(val.attr_type())) {
      return set_value_from_str(result, val.get_string());
    }
    if (val.attr_type() == AttrType::DATES) {
      result.set_date(val.get_date());
      return RC::SUCCESS;
    }
  }

  if (type == AttrType::CHARS) {
    string s;
    RC rc = to_string(val, s);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    result.set_string(s.c_str());
    return RC::SUCCESS;
  }
  if (type == AttrType::TEXTS) {
    string s;
    RC rc = to_string(val, s);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    result.set_text(s.c_str());
    return RC::SUCCESS;
  }

  LOG_WARN("unsupported date cast to type %d", static_cast<int>(type));
  return RC::SCHEMA_FIELD_TYPE_MISMATCH;
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  int32_t encoded = 0;
  RC rc = date_parse(data, encoded);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  val.set_date(encoded);
  return RC::SUCCESS;
}

RC DateType::to_string(const Value &val, string &result) const
{
  ASSERT(val.attr_type() == AttrType::DATES, "value type is not date");

  int year = 0;
  int month = 0;
  int day = 0;
  encoded_to_ymd(val.get_date(), year, month, day);

  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  result = buf;
  return RC::SUCCESS;
}
