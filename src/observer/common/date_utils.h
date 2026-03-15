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

#include <string>

/**
 * @brief 日期工具：字符串与整数(Julian day number)互转，仅用于 DATE 类型
 * @details 不依赖 src/common，逻辑自包含于 observer
 */

/**
 * 将 "yyyy-mm-dd" 或 "yyyy-m-d" 解析为 Julian day number，存入 date
 * @return 0 成功，-1 解析失败或日期非法
 */
int string_to_date(const std::string &str, int &date);

/**
 * 将 Julian day number 格式化为 "yyyy-mm-dd"
 */
std::string date_to_string(int date);
