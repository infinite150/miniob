/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/date_utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Julian day number 公式（与 common/time/datetime 一致）
int julian_date(int year, int month, int day)
{
  int a = (14 - month) / 12;
  int y = year + 4800 - a;
  int m = month + 12 * a - 3;
  return (day + (153 * m + 2) / 5 + y * 365 + y / 4 - y / 100 + y / 400 - 32045);
}

void get_ymd(int jday, int &year, int &month, int &day)
{
  int a = jday + 32044;
  int b = (4 * a + 3) / 146097;
  int c = a - (b * 146097) / 4;
  int d = (4 * c + 3) / 1461;
  int e = c - (1461 * d) / 4;
  int m = (5 * e + 2) / 153;
  day   = e - (153 * m + 2) / 5 + 1;
  month = m + 3 - 12 * (m / 10);
  year  = b * 100 + d - 4800 + m / 10;
}

int max_day_in_month(int year, int month)
{
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12) {
    return 31;
  }
  if (month == 4 || month == 6 || month == 9 || month == 11) {
    return 30;
  }
  // February
  bool leap = (year % 400 == 0) || ((year % 100 != 0) && (year % 4 == 0));
  return leap ? 29 : 28;
}

}  // namespace

int string_to_date(const std::string &str, int &date)
{
  int year = 0, month = 0, day = 0;
  int n = std::sscanf(str.c_str(), "%d-%d-%d", &year, &month, &day);
  if (n != 3) {
    return -1;
  }
  if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1) {
    return -1;
  }
  if (day > max_day_in_month(year, month)) {
    return -1;
  }
  date = julian_date(year, month, day);
  return 0;
}

std::string date_to_string(int date)
{
  int year = 0, month = 0, day = 0;
  get_ymd(date, year, month, day);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return std::string(buf);
}
