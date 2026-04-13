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
// Created by Meiyi
//

#include "sql/parser/parse.h"
#include <cctype>
#include "common/log/log.h"
#include "sql/expr/expression.h"

RC parse(char *st, ParsedSqlNode *sqln);

UpdateSqlNode::~UpdateSqlNode()
{
  for (auto &p : updates) {
    delete p.second;
    p.second = nullptr;
  }
  updates.clear();
}

ParsedSqlNode::ParsedSqlNode() : flag(SCF_ERROR) {}

ParsedSqlNode::ParsedSqlNode(SqlCommandFlag _flag) : flag(_flag) {}

void ParsedSqlResult::add_sql_node(unique_ptr<ParsedSqlNode> sql_node)
{
  sql_nodes_.emplace_back(std::move(sql_node));
}

////////////////////////////////////////////////////////////////////////////////

int sql_parse(const char *st, ParsedSqlResult *sql_result);

namespace {

void trim_sql(string &sql)
{
  size_t begin = 0;
  while (begin < sql.size() && isspace(static_cast<unsigned char>(sql[begin]))) {
    begin++;
  }

  size_t end = sql.size();
  while (end > begin && isspace(static_cast<unsigned char>(sql[end - 1]))) {
    end--;
  }

  sql = sql.substr(begin, end - begin);

  while (!sql.empty() && sql.back() == ';') {
    sql.pop_back();
    while (!sql.empty() && isspace(static_cast<unsigned char>(sql.back()))) {
      sql.pop_back();
    }
  }
}

void skip_blanks(const string &sql, size_t &pos)
{
  while (pos < sql.size() && isspace(static_cast<unsigned char>(sql[pos]))) {
    pos++;
  }
}

bool is_ident_start(char ch)
{
  return isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool is_ident_char(char ch)
{
  return isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool parse_ident(const string &sql, size_t &pos, string &ident)
{
  skip_blanks(sql, pos);
  if (pos >= sql.size() || !is_ident_start(sql[pos])) {
    return false;
  }

  size_t begin = pos;
  pos++;
  while (pos < sql.size() && is_ident_char(sql[pos])) {
    pos++;
  }

  ident = sql.substr(begin, pos - begin);
  return true;
}

bool consume_keyword(const string &sql, size_t &pos, const char *keyword)
{
  skip_blanks(sql, pos);
  const size_t keyword_len = strlen(keyword);
  if (pos + keyword_len > sql.size()) {
    return false;
  }

  for (size_t i = 0; i < keyword_len; i++) {
    if (tolower(static_cast<unsigned char>(sql[pos + i])) != tolower(static_cast<unsigned char>(keyword[i]))) {
      return false;
    }
  }

  if (pos + keyword_len < sql.size() && is_ident_char(sql[pos + keyword_len])) {
    return false;
  }

  pos += keyword_len;
  return true;
}

bool parse_column_name_list(const string &sql, size_t &pos, vector<string> &columns)
{
  skip_blanks(sql, pos);
  if (pos >= sql.size() || sql[pos] != '(') {
    return false;
  }
  pos++;

  columns.clear();
  while (true) {
    string column;
    if (!parse_ident(sql, pos, column)) {
      return false;
    }
    columns.emplace_back(std::move(column));

    skip_blanks(sql, pos);
    if (pos >= sql.size()) {
      return false;
    }
    if (sql[pos] == ',') {
      pos++;
      continue;
    }
    if (sql[pos] == ')') {
      pos++;
      return true;
    }
    return false;
  }
}

bool try_parse_create_view(const string &raw_sql, ParsedSqlResult *sql_result)
{
  string sql = raw_sql;
  trim_sql(sql);
  if (sql.empty()) {
    return false;
  }

  size_t pos = 0;
  if (!consume_keyword(sql, pos, "create") || !consume_keyword(sql, pos, "view")) {
    return false;
  }

  auto sql_node               = make_unique<ParsedSqlNode>(SCF_CREATE_VIEW);
  CreateViewSqlNode &view_sql = sql_node->create_view;
  if (!parse_ident(sql, pos, view_sql.view_name)) {
    return false;
  }

  skip_blanks(sql, pos);
  if (pos < sql.size() && sql[pos] == '(') {
    if (!parse_column_name_list(sql, pos, view_sql.column_names)) {
      return false;
    }
  }

  if (!consume_keyword(sql, pos, "as")) {
    return false;
  }

  skip_blanks(sql, pos);
  if (pos >= sql.size()) {
    return false;
  }

  view_sql.select_sql = sql.substr(pos);
  trim_sql(view_sql.select_sql);
  if (view_sql.select_sql.empty()) {
    return false;
  }

  auto &nodes = sql_result->sql_nodes();
  nodes.clear();
  nodes.emplace_back(std::move(sql_node));
  return true;
}

bool try_parse_drop_view(const string &raw_sql, ParsedSqlResult *sql_result)
{
  string sql = raw_sql;
  trim_sql(sql);
  if (sql.empty()) {
    return false;
  }

  size_t pos = 0;
  if (!consume_keyword(sql, pos, "drop") || !consume_keyword(sql, pos, "view")) {
    return false;
  }

  string view_name;
  if (!parse_ident(sql, pos, view_name)) {
    return false;
  }

  skip_blanks(sql, pos);
  if (pos != sql.size()) {
    return false;
  }

  auto sql_node               = make_unique<ParsedSqlNode>(SCF_DROP_VIEW);
  sql_node->drop_view.view_name = std::move(view_name);
  auto &nodes                 = sql_result->sql_nodes();
  nodes.clear();
  nodes.emplace_back(std::move(sql_node));
  return true;
}

void try_parse_view_sql_fallback(const char *st, ParsedSqlResult *sql_result)
{
  if (st == nullptr || sql_result == nullptr) {
    return;
  }
  auto &nodes = sql_result->sql_nodes();
  if (nodes.size() != 1 || nodes.front() == nullptr || nodes.front()->flag != SCF_ERROR) {
    return;
  }

  const string raw_sql = st;
  if (try_parse_create_view(raw_sql, sql_result)) {
    return;
  }
  try_parse_drop_view(raw_sql, sql_result);
}

}  // namespace

RC parse(const char *st, ParsedSqlResult *sql_result)
{
  sql_parse(st, sql_result);
  try_parse_view_sql_fallback(st, sql_result);
  return RC::SUCCESS;
}
