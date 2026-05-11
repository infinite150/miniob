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

bool keyword_at_top_level(const string &sql, size_t pos, const char *keyword)
{
  const size_t len = strlen(keyword);
  if (pos + len > sql.size()) {
    return false;
  }
  if (pos > 0 && is_ident_char(sql[pos - 1])) {
    return false;
  }
  if (pos + len < sql.size() && is_ident_char(sql[pos + len])) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    if (tolower(static_cast<unsigned char>(sql[pos + i])) != tolower(static_cast<unsigned char>(keyword[i]))) {
      return false;
    }
  }
  return true;
}

size_t find_top_level_keyword(const string &sql, const char *keyword, size_t begin_pos = 0)
{
  int  depth       = 0;
  char quote_ch    = 0;
  bool escaped     = false;
  const size_t len = strlen(keyword);

  for (size_t i = begin_pos; i < sql.size(); i++) {
    char ch = sql[i];
    if (quote_ch != 0) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == quote_ch) {
        quote_ch = 0;
      }
      continue;
    }

    if (ch == '\'' || ch == '"' || ch == '`') {
      quote_ch = ch;
      escaped  = false;
      continue;
    }
    if (ch == '(') {
      depth++;
      continue;
    }
    if (ch == ')') {
      if (depth > 0) {
        depth--;
      }
      continue;
    }

    if (depth == 0 && i + len <= sql.size() && keyword_at_top_level(sql, i, keyword)) {
      return i;
    }
  }
  return string::npos;
}

size_t find_top_level_order_by(const string &sql, size_t begin_pos = 0)
{
  size_t pos = begin_pos;
  while (true) {
    pos = find_top_level_keyword(sql, "order", pos);
    if (pos == string::npos) {
      return pos;
    }

    size_t by_pos = pos + strlen("order");
    skip_blanks(sql, by_pos);
    if (keyword_at_top_level(sql, by_pos, "by")) {
      return pos;
    }
    pos++;
  }
}

void try_patch_having_from_raw_sql(const char *st, ParsedSqlResult *sql_result)
{
  if (st == nullptr || sql_result == nullptr) {
    return;
  }
  auto &nodes = sql_result->sql_nodes();
  if (nodes.empty() || nodes.front() == nullptr || nodes.front()->flag != SCF_SELECT) {
    return;
  }

  ParsedSqlNode *sql_node = nodes.front().get();
  if (sql_node->selection.having_expr != nullptr) {
    return;
  }

  string raw_sql = st;
  trim_sql(raw_sql);
  if (raw_sql.empty()) {
    return;
  }

  const size_t having_pos = find_top_level_keyword(raw_sql, "having");
  if (having_pos == string::npos) {
    return;
  }

  const size_t cond_begin = having_pos + strlen("having");
  size_t       cond_end   = find_top_level_order_by(raw_sql, cond_begin);
  if (cond_end == string::npos) {
    cond_end = raw_sql.size();
  }
  if (cond_end <= cond_begin) {
    return;
  }

  string having_condition = raw_sql.substr(cond_begin, cond_end - cond_begin);
  trim_sql(having_condition);
  if (having_condition.empty()) {
    return;
  }

  // Reuse WHERE condition grammar to parse HAVING expression.
  // `where_expr` is only available in the SELECT ... FROM ... production.
  string          probe_sql = "select 1 from __miniob_having_probe where " + having_condition;
  ParsedSqlResult probe_result;
  sql_parse(probe_sql.c_str(), &probe_result);
  auto &probe_nodes = probe_result.sql_nodes();
  if (probe_nodes.size() != 1 || probe_nodes.front() == nullptr || probe_nodes.front()->flag != SCF_SELECT) {
    return;
  }

  if (probe_nodes.front()->selection.condition_expr != nullptr) {
    sql_node->selection.having_expr                 = probe_nodes.front()->selection.condition_expr;
    probe_nodes.front()->selection.condition_expr = nullptr;

    // Some old lex/yacc combinations may emit an extra trailing SCF_ERROR node
    // for SELECT ... GROUP BY ... HAVING ...; keep the patched SELECT only.
    if (nodes.size() > 1) {
      nodes.erase(nodes.begin() + 1, nodes.end());
    }
  }
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

static bool try_parse_view_sql_first(const char *st, ParsedSqlResult *sql_result)
{
  if (st == nullptr || sql_result == nullptr) {
    return false;
  }
  const string raw_sql = st;
  if (try_parse_create_view(raw_sql, sql_result)) {
    return true;
  }
  if (try_parse_drop_view(raw_sql, sql_result)) {
    return true;
  }
  return false;
}

RC parse(const char *st, ParsedSqlResult *sql_result)
{
  // Parse CREATE VIEW / DROP VIEW with the manual parser first, because the yacc
  // grammar relies on token_name() which cannot correctly extract the SELECT SQL
  // text for multi-token ranges (column tracking is per-token in this lexer).
  if (!try_parse_view_sql_first(st, sql_result)) {
    sql_parse(st, sql_result);
    try_parse_view_sql_fallback(st, sql_result);
    try_patch_having_from_raw_sql(st, sql_result);
  }
  return RC::SUCCESS;
}
