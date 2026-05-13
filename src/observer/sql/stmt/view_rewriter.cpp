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
// Created by Codex on 2026/4/13.
//

#include "sql/stmt/view_rewriter.h"

#include <cctype>
#include <strings.h>

#include "common/lang/string.h"
#include "common/lang/unordered_map.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "sql/parser/parse.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

using namespace std;

namespace {

constexpr int kMaxViewExpandDepth = 8;

string normalize_lower(const string &s)
{
  string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool ieq(const string &lhs, const string &rhs)
{
  return 0 == strcasecmp(lhs.c_str(), rhs.c_str());
}

bool ieq(const char *lhs, const string &rhs)
{
  return lhs != nullptr && 0 == strcasecmp(lhs, rhs.c_str());
}

RC parse_select_sql(const string &select_sql, SelectSqlNode &select_sql_node)
{
  ParsedSqlResult parsed_sql_result;
  parse(select_sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().size() != 1) {
    LOG_WARN("invalid view select sql: %s", select_sql.c_str());
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *sql_node = parsed_sql_result.sql_nodes().front().get();
  if (sql_node == nullptr || sql_node->flag != SCF_SELECT) {
    LOG_WARN("view select sql is not a select statement");
    return RC::INVALID_ARGUMENT;
  }
  select_sql_node = std::move(sql_node->selection);
  return RC::SUCCESS;
}

RC expand_single_table_star_exprs(Db *db, const ViewMeta &view_meta, SelectSqlNode &select_sql)
{
  if (db == nullptr || select_sql.expressions.size() != 1) {
    return RC::SUCCESS;
  }

  Expression *expr = select_sql.expressions[0].get();
  if (expr == nullptr || expr->type() != ExprType::STAR) {
    return RC::SUCCESS;
  }

  if (select_sql.relations.size() != 1 || !select_sql.relations[0].join_relations.empty()) {
    return RC::SUCCESS;
  }

  const string &base_table = select_sql.relations[0].base_relation.first;
  Table        *table      = db->find_table(base_table.c_str());
  if (table == nullptr) {
    LOG_WARN("base table not found while expanding star for view. view=%s, table=%s", view_meta.name.c_str(), base_table.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta = table->table_meta();
  vector<unique_ptr<Expression>> expanded_exprs;
  expanded_exprs.reserve(table_meta.field_num() - table_meta.sys_field_num());
  for (int i = table_meta.sys_field_num(); i < table_meta.field_num(); i++) {
    const FieldMeta *field_meta = table_meta.field(i);
    auto             field_expr = make_unique<UnboundFieldExpr>(base_table, field_meta->name());
    field_expr->set_name(field_meta->name());
    expanded_exprs.emplace_back(std::move(field_expr));
  }

  if (!view_meta.column_names.empty() && view_meta.column_names.size() != expanded_exprs.size()) {
    LOG_WARN("view columns mismatch with star expansion. view=%s, columns=%zu, expanded=%zu",
        view_meta.name.c_str(),
        view_meta.column_names.size(),
        expanded_exprs.size());
    return RC::INVALID_ARGUMENT;
  }

  select_sql.expressions.swap(expanded_exprs);
  return RC::SUCCESS;
}

vector<string> derive_view_columns(const ViewMeta &view_meta, const SelectSqlNode &select_sql)
{
  if (!view_meta.column_names.empty() && view_meta.column_names.size() == select_sql.expressions.size()) {
    return view_meta.column_names;
  }

  vector<string> columns;
  columns.reserve(select_sql.expressions.size());
  for (size_t i = 0; i < select_sql.expressions.size(); i++) {
    string column_name =
        select_sql.expressions[i] != nullptr && select_sql.expressions[i]->name() != nullptr
            ? select_sql.expressions[i]->name()
            : "";
    if (column_name.empty()) {
      column_name = "c" + to_string(i + 1);
    }
    columns.emplace_back(std::move(column_name));
  }
  return columns;
}

const Expression *find_column_expr(const unordered_map<string, const Expression *> &column_exprs, const string &column)
{
  auto iter = column_exprs.find(normalize_lower(column));
  return iter == column_exprs.end() ? nullptr : iter->second;
}

RC rewrite_expr_for_view(unique_ptr<Expression> &expr, const unordered_map<string, const Expression *> &column_exprs,
    const string &view_name, const string &view_alias)
{
  if (expr == nullptr) {
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::UNBOUND_FIELD) {
    auto *field_expr = static_cast<UnboundFieldExpr *>(expr.get());
    const bool qualified = !common::is_blank(field_expr->table_name());
    if (qualified && !ieq(field_expr->table_name(), view_name) && !ieq(field_expr->table_name(), view_alias)) {
      return RC::SUCCESS;
    }

    const Expression *mapped_expr = find_column_expr(column_exprs, field_expr->field_name());
    if (mapped_expr == nullptr) {
      LOG_WARN("view column not found: %s", field_expr->field_name());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    string old_name = expr->name() != nullptr ? expr->name() : "";
    expr            = mapped_expr->copy();
    if (!old_name.empty()) {
      expr->set_name(old_name);
    }
    return RC::SUCCESS;
  }

  auto visitor = [&](unique_ptr<Expression> &child_expr) -> RC {
    return rewrite_expr_for_view(child_expr, column_exprs, view_name, view_alias);
  };
  return ExpressionIterator::iterate_child_expr(*expr, visitor);
}

bool star_targets_view(const StarExpr *star_expr, const string &view_name, const string &view_alias)
{
  if (star_expr == nullptr) {
    return false;
  }
  const char *table_name = star_expr->table_name();
  if (common::is_blank(table_name) || 0 == strcmp(table_name, "*")) {
    return true;
  }
  return ieq(table_name, view_name) || ieq(table_name, view_alias);
}

Expression *combine_and(Expression *left, Expression *right)
{
  if (left == nullptr) {
    return right;
  }
  if (right == nullptr) {
    return left;
  }
  vector<unique_ptr<Expression>> children;
  children.emplace_back(left);
  children.emplace_back(right);
  return new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
}

bool can_passthrough_view_query(const SelectSqlNode &outer_select, const string &view_name, const string &view_alias)
{
  if (outer_select.condition_expr != nullptr || !outer_select.group_by.empty() || !outer_select.order_by_exprs.empty()) {
    return false;
  }
  if (outer_select.expressions.size() != 1) {
    return false;
  }
  const Expression *expr = outer_select.expressions[0].get();
  if (expr == nullptr || expr->type() != ExprType::STAR) {
    return false;
  }
  return star_targets_view(static_cast<const StarExpr *>(expr), view_name, view_alias);
}

bool has_aggregate_exprs(const SelectSqlNode &select)
{
  for (const auto &expr : select.expressions) {
    if (expr != nullptr && expr->type() == ExprType::UNBOUND_AGGREGATION) {
      return true;
    }
  }
  return false;
}

RC rewrite_select_sql(Db *db, SelectSqlNode &select_sql, int depth);

RC rewrite_select_from_single_view(Db *db, SelectSqlNode &outer_select, const ViewMeta &view_meta, int depth)
{
  if (outer_select.relations.size() != 1) {
    return RC::UNSUPPORTED;
  }

  const string view_name  = outer_select.relations[0].base_relation.first;
  const string view_alias =
      outer_select.relations[0].base_relation.second.empty() ? view_name : outer_select.relations[0].base_relation.second;

  SelectSqlNode inner_select;
  RC            rc = parse_select_sql(view_meta.select_sql, inner_select);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = rewrite_select_sql(db, inner_select, depth + 1);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (can_passthrough_view_query(outer_select, view_name, view_alias)) {
    outer_select = std::move(inner_select);
    return RC::SUCCESS;
  }

  rc = expand_single_table_star_exprs(db, view_meta, inner_select);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (!inner_select.group_by.empty() || !inner_select.order_by_exprs.empty()) {
    LOG_WARN("complex view is only supported by direct passthrough query. view=%s", view_name.c_str());
    return RC::UNSUPPORTED;
  }

  if (inner_select.expressions.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  vector<string> view_columns = derive_view_columns(view_meta, inner_select);
  if (view_columns.size() != inner_select.expressions.size()) {
    LOG_WARN("view columns mismatch. view=%s", view_name.c_str());
    return RC::INVALID_ARGUMENT;
  }

  unordered_map<string, const Expression *> column_exprs;
  for (size_t i = 0; i < view_columns.size(); i++) {
    column_exprs[normalize_lower(view_columns[i])] = inner_select.expressions[i].get();
  }

  bool is_agg_view = has_aggregate_exprs(inner_select) && inner_select.group_by.empty();

  vector<unique_ptr<Expression>> rewritten_expressions;
  for (auto &expr : outer_select.expressions) {
    if (expr != nullptr && expr->type() == ExprType::STAR &&
        star_targets_view(static_cast<StarExpr *>(expr.get()), view_name, view_alias)) {
      for (size_t i = 0; i < view_columns.size(); i++) {
        unique_ptr<Expression> rewritten = inner_select.expressions[i]->copy();
        rewritten->set_name(view_columns[i]);
        rewritten_expressions.emplace_back(std::move(rewritten));
      }
      continue;
    }

    // For aggregate views (no GROUP BY), outer aggregates on view
    // columns are redundant — the inner expression already produces
    // the correct result.  Replace here and skip rewrite_expr_for_view
    // to avoid re-interpreting inner field references as view columns.
    bool pre_handled = false;
    if (is_agg_view && expr != nullptr && expr->type() == ExprType::UNBOUND_AGGREGATION) {
      auto *agg = static_cast<UnboundAggregateExpr *>(expr.get());
      if (agg->child() != nullptr) {
        // count(*) → max(1)
        bool is_count = (agg->aggregate_name() != nullptr && 0 == strcasecmp(agg->aggregate_name(), "count"));
        if (is_count || agg->child()->type() == ExprType::STAR) {
          string name = expr->name() != nullptr ? expr->name() : "";
          auto replacement = make_unique<UnboundAggregateExpr>("max", make_unique<ValueExpr>(Value(1)));
          if (!name.empty()) {
            replacement->set_name(name);
          }
          rewritten_expressions.emplace_back(std::move(replacement));
          pre_handled = true;
        }
        // agg(view_column) → inner expression
        else if (agg->child()->type() == ExprType::UNBOUND_FIELD) {
          auto       *field_expr = static_cast<UnboundFieldExpr *>(agg->child().get());
          const string &col_name  = field_expr->field_name();
          auto        iter       = column_exprs.find(normalize_lower(col_name));
          if (iter != column_exprs.end()) {
            string name = expr->name() != nullptr ? expr->name() : "";
            auto replacement = iter->second->copy();
            if (!name.empty()) {
              replacement->set_name(name);
            }
            rewritten_expressions.emplace_back(std::move(replacement));
            pre_handled = true;
          }
        }
      }
    }
    if (pre_handled) {
      continue;
    }

    rc = rewrite_expr_for_view(expr, column_exprs, view_name, view_alias);
    if (OB_FAIL(rc)) {
      return rc;
    }
    rewritten_expressions.emplace_back(std::move(expr));
  }
  outer_select.expressions.swap(rewritten_expressions);

  if (outer_select.condition_expr != nullptr) {
    unique_ptr<Expression> outer_condition(outer_select.condition_expr);
    outer_select.condition_expr = nullptr;
    rc                          = rewrite_expr_for_view(outer_condition, column_exprs, view_name, view_alias);
    if (OB_FAIL(rc)) {
      return rc;
    }
    outer_select.condition_expr = outer_condition.release();
  }

  for (auto &group_expr : outer_select.group_by) {
    rc = rewrite_expr_for_view(group_expr, column_exprs, view_name, view_alias);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  for (auto &order_expr : outer_select.order_by_exprs) {
    rc = rewrite_expr_for_view(order_expr, column_exprs, view_name, view_alias);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  // Merge view WHERE with outer WHERE.
  Expression *inner_condition = inner_select.condition_expr;
  inner_select.condition_expr = nullptr;
  Expression *outer_condition = outer_select.condition_expr;
  outer_select.condition_expr = combine_and(inner_condition, outer_condition);

  if (!inner_select.conditions.empty()) {
    outer_select.conditions.insert(
        outer_select.conditions.begin(), inner_select.conditions.begin(), inner_select.conditions.end());
  }

  outer_select.relations = std::move(inner_select.relations);
  return RC::SUCCESS;
}

RC rewrite_select_sql(Db *db, SelectSqlNode &select_sql, int depth)
{
  if (depth > kMaxViewExpandDepth) {
    LOG_WARN("too deep view expansion");
    return RC::INVALID_ARGUMENT;
  }

  if (select_sql.relations.empty()) {
    return RC::SUCCESS;
  }

  const ViewMeta *view_meta = nullptr;
  int             view_count = 0;
  for (const InnerJoinSqlNode &from_item : select_sql.relations) {
    const ViewMeta *base_view = db->find_view(from_item.base_relation.first.c_str());
    if (base_view != nullptr) {
      view_count++;
      view_meta = base_view;
    }
    for (const auto &join_relation : from_item.join_relations) {
      if (db->find_view(join_relation.first.c_str()) != nullptr) {
        LOG_WARN("view in JOIN is not supported currently");
        return RC::UNSUPPORTED;
      }
    }
  }

  if (view_count == 0) {
    return RC::SUCCESS;
  }
  if (view_count != 1 || select_sql.relations.size() != 1 || !select_sql.relations[0].join_relations.empty() ||
      view_meta == nullptr) {
    LOG_WARN("only single-view query is supported currently");
    return RC::UNSUPPORTED;
  }

  return rewrite_select_from_single_view(db, select_sql, *view_meta, depth);
}

RC map_attr_from_view(RelAttrSqlNode &attr, const string &view_name, const string &base_table,
    const unordered_map<string, string> &view_to_base)
{
  const bool has_rel = !common::is_blank(attr.relation_name.c_str());
  const bool from_view =
      !has_rel || ieq(attr.relation_name, view_name) || ieq(attr.relation_name, base_table);
  if (!from_view) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  auto iter = view_to_base.find(normalize_lower(attr.attribute_name));
  if (iter == view_to_base.end()) {
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  attr.relation_name  = base_table;
  attr.attribute_name = iter->second;
  return RC::SUCCESS;
}

bool extract_rel_attr(Expression *expr, RelAttrSqlNode &attr)
{
  if (expr == nullptr || expr->type() != ExprType::UNBOUND_FIELD) {
    return false;
  }
  auto *field_expr    = static_cast<UnboundFieldExpr *>(expr);
  attr.relation_name  = common::is_blank(field_expr->table_name()) ? "" : field_expr->table_name();
  attr.attribute_name = field_expr->field_name();
  return true;
}

bool extract_value(Expression *expr, Value &value)
{
  if (expr == nullptr || expr->type() != ExprType::VALUE) {
    return false;
  }
  value = static_cast<ValueExpr *>(expr)->get_value();
  return true;
}

RC append_simple_conditions_from_expr(Expression *expr, vector<ConditionSqlNode> &conditions)
{
  if (expr == nullptr) {
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::CONJUNCTION) {
    auto *conj = static_cast<ConjunctionExpr *>(expr);
    if (conj->conjunction_type() != ConjunctionExpr::Type::AND) {
      return RC::UNSUPPORTED;
    }
    for (auto &child : conj->children()) {
      RC rc = append_simple_conditions_from_expr(child.get(), conditions);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::COMPARISON) {
    auto *cmp_expr = static_cast<ComparisonExpr *>(expr);
    ConditionSqlNode condition;
    condition.comp = cmp_expr->comp();

    RelAttrSqlNode left_attr;
    if (extract_rel_attr(cmp_expr->left().get(), left_attr)) {
      condition.left_is_attr = 1;
      condition.left_attr    = std::move(left_attr);
    } else if (extract_value(cmp_expr->left().get(), condition.left_value)) {
      condition.left_is_attr = 0;
    } else {
      return RC::UNSUPPORTED;
    }

    RelAttrSqlNode right_attr;
    if (extract_rel_attr(cmp_expr->right().get(), right_attr)) {
      condition.right_is_attr = 1;
      condition.right_attr    = std::move(right_attr);
    } else if (extract_value(cmp_expr->right().get(), condition.right_value)) {
      condition.right_is_attr = 0;
    } else {
      return RC::UNSUPPORTED;
    }

    conditions.emplace_back(std::move(condition));
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::IS_NULL) {
    auto *is_null_expr = static_cast<IsNullExpr *>(expr);
    RelAttrSqlNode attr;
    if (!extract_rel_attr(is_null_expr->child().get(), attr)) {
      return RC::UNSUPPORTED;
    }

    ConditionSqlNode condition;
    condition.left_is_attr  = 1;
    condition.left_attr     = std::move(attr);
    condition.comp          = is_null_expr->is_not() ? IS_NOT_NULL_OP : IS_NULL_OP;
    condition.right_is_attr = 0;
    condition.right_value.set_null();
    conditions.emplace_back(std::move(condition));
    return RC::SUCCESS;
  }

  return RC::UNSUPPORTED;
}

RC normalize_filter_attr_to_base(
    RelAttrSqlNode &attr, const string &base_table, const string &base_alias, const TableMeta &table_meta)
{
  const bool has_rel = !common::is_blank(attr.relation_name.c_str());
  if (has_rel && !ieq(attr.relation_name, base_table) && (base_alias.empty() || !ieq(attr.relation_name, base_alias))) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }
  if (table_meta.field(attr.attribute_name.c_str()) == nullptr) {
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  attr.relation_name = base_table;
  return RC::SUCCESS;
}

RC normalize_filter_conditions_to_base(vector<ConditionSqlNode> &conditions, const string &base_table,
    const string &base_alias, const TableMeta &table_meta)
{
  for (ConditionSqlNode &condition : conditions) {
    if (condition.left_is_attr) {
      RC rc = normalize_filter_attr_to_base(condition.left_attr, base_table, base_alias, table_meta);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    if (condition.right_is_attr) {
      RC rc = normalize_filter_attr_to_base(condition.right_attr, base_table, base_alias, table_meta);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  return RC::SUCCESS;
}

RC build_simple_updatable_view_mapping(Db *db, const string &view_name, const ViewMeta &view_meta, string &base_table,
    vector<string> &view_columns, unordered_map<string, string> &view_to_base,
    vector<ConditionSqlNode> *view_filter_conditions = nullptr, bool require_mappable_filter = false)
{
  SelectSqlNode view_select;
  RC            rc = parse_select_sql(view_meta.select_sql, view_select);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = rewrite_select_sql(db, view_select, 0);
  if (OB_FAIL(rc)) {
    return rc;
  }

  rc = expand_single_table_star_exprs(db, view_meta, view_select);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (view_select.relations.size() != 1 || !view_select.relations[0].join_relations.empty() ||
      !view_select.group_by.empty() || !view_select.order_by_exprs.empty()) {
    LOG_WARN("view is not updatable: %s", view_name.c_str());
    return RC::UNSUPPORTED;
  }

  base_table = view_select.relations[0].base_relation.first;
  Table *base_table_handle = db->find_table(base_table.c_str());
  if (base_table_handle == nullptr) {
    LOG_WARN("view base table not found: %s", base_table.c_str());
    return RC::UNSUPPORTED;
  }
  const string base_alias = view_select.relations[0].base_relation.second;

  if (view_filter_conditions != nullptr) {
    view_filter_conditions->clear();
    if (!view_select.conditions.empty()) {
      view_filter_conditions->insert(
          view_filter_conditions->end(), view_select.conditions.begin(), view_select.conditions.end());
    }
    if (view_select.condition_expr != nullptr) {
      rc = append_simple_conditions_from_expr(view_select.condition_expr, *view_filter_conditions);
      if (OB_FAIL(rc)) {
        if (require_mappable_filter) {
          return rc;
        }
        view_filter_conditions->clear();
      }
    }
    rc = normalize_filter_conditions_to_base(
        *view_filter_conditions, base_table, base_alias, base_table_handle->table_meta());
    if (OB_FAIL(rc)) {
      if (require_mappable_filter) {
        return rc;
      }
      view_filter_conditions->clear();
    }
  } else if (require_mappable_filter && (view_select.condition_expr != nullptr || !view_select.conditions.empty())) {
    return RC::UNSUPPORTED;
  }

  view_columns = derive_view_columns(view_meta, view_select);
  if (view_columns.size() != view_select.expressions.size()) {
    return RC::INVALID_ARGUMENT;
  }

  for (size_t i = 0; i < view_select.expressions.size(); i++) {
    Expression *expr = view_select.expressions[i].get();
    if (expr == nullptr || expr->type() != ExprType::UNBOUND_FIELD) {
      return RC::UNSUPPORTED;
    }
    auto *field_expr = static_cast<UnboundFieldExpr *>(expr);
    if (!common::is_blank(field_expr->table_name()) && !ieq(field_expr->table_name(), base_table) &&
        (base_alias.empty() || !ieq(field_expr->table_name(), base_alias))) {
      return RC::UNSUPPORTED;
    }
    view_to_base[normalize_lower(view_columns[i])] = field_expr->field_name();
  }

  return RC::SUCCESS;
}

RC rewrite_update_sql(Db *db, UpdateSqlNode &update_sql)
{
  const ViewMeta *view_meta = db->find_view(update_sql.relation_name.c_str());
  if (view_meta == nullptr) {
    return RC::SUCCESS;
  }

  string                              base_table;
  vector<string>                      view_columns;
  unordered_map<string, string>       view_to_base;
  vector<ConditionSqlNode>            view_filter_conditions;
  RC rc = build_simple_updatable_view_mapping(
      db, update_sql.relation_name, *view_meta, base_table, view_columns, view_to_base, &view_filter_conditions, true);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (!update_sql.updates.empty()) {
    for (auto &item : update_sql.updates) {
      auto iter = view_to_base.find(normalize_lower(item.first));
      if (iter == view_to_base.end()) {
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      item.first = iter->second;
    }
  } else if (!update_sql.attribute_name.empty()) {
    auto iter = view_to_base.find(normalize_lower(update_sql.attribute_name));
    if (iter == view_to_base.end()) {
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    update_sql.attribute_name = iter->second;
  }

  for (ConditionSqlNode &condition : update_sql.conditions) {
    if (condition.left_is_attr) {
      rc = map_attr_from_view(condition.left_attr, update_sql.relation_name, base_table, view_to_base);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    if (condition.right_is_attr) {
      rc = map_attr_from_view(condition.right_attr, update_sql.relation_name, base_table, view_to_base);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  if (!view_filter_conditions.empty()) {
    update_sql.conditions.insert(
        update_sql.conditions.end(), view_filter_conditions.begin(), view_filter_conditions.end());
  }

  update_sql.relation_name = base_table;
  return RC::SUCCESS;
}

RC rewrite_delete_sql(Db *db, DeleteSqlNode &delete_sql)
{
  const ViewMeta *view_meta = db->find_view(delete_sql.relation_name.c_str());
  if (view_meta == nullptr) {
    return RC::SUCCESS;
  }

  string                              base_table;
  vector<string>                      view_columns;
  unordered_map<string, string>       view_to_base;
  vector<ConditionSqlNode>            view_filter_conditions;
  RC rc = build_simple_updatable_view_mapping(
      db, delete_sql.relation_name, *view_meta, base_table, view_columns, view_to_base, &view_filter_conditions, true);
  if (OB_FAIL(rc)) {
    return rc;
  }

  for (ConditionSqlNode &condition : delete_sql.conditions) {
    if (condition.left_is_attr) {
      rc = map_attr_from_view(condition.left_attr, delete_sql.relation_name, base_table, view_to_base);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    if (condition.right_is_attr) {
      rc = map_attr_from_view(condition.right_attr, delete_sql.relation_name, base_table, view_to_base);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  if (!view_filter_conditions.empty()) {
    delete_sql.conditions.insert(
        delete_sql.conditions.end(), view_filter_conditions.begin(), view_filter_conditions.end());
  }

  delete_sql.relation_name = base_table;
  return RC::SUCCESS;
}

RC rewrite_insert_sql(Db *db, InsertSqlNode &insert_sql)
{
  const ViewMeta *view_meta = db->find_view(insert_sql.relation_name.c_str());
  if (view_meta == nullptr) {
    return RC::SUCCESS;
  }

  string                              base_table;
  vector<string>                      view_columns;
  unordered_map<string, string>       view_to_base;
  RC rc = build_simple_updatable_view_mapping(
      db, insert_sql.relation_name, *view_meta, base_table, view_columns, view_to_base, nullptr, false);
  if (OB_FAIL(rc)) {
    return rc;
  }

  Table *table = db->find_table(base_table.c_str());
  if (table == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta      = table->table_meta();
  const int        user_field_num  = table_meta.field_num() - table_meta.sys_field_num();
  unordered_map<string, int> base_field_index;
  for (int i = 0; i < user_field_num; i++) {
    const FieldMeta *field_meta = table_meta.field(table_meta.sys_field_num() + i);
    base_field_index[normalize_lower(field_meta->name())] = i;
  }

  auto rewrite_one_row = [&](vector<Value> &row) -> RC {
    if (row.size() != view_columns.size()) {
      return RC::SCHEMA_FIELD_MISSING;
    }
    vector<Value> rewritten_row(user_field_num);
    for (Value &value : rewritten_row) {
      value.set_null();
    }
    for (size_t i = 0; i < row.size(); i++) {
      auto iter = view_to_base.find(normalize_lower(view_columns[i]));
      if (iter == view_to_base.end()) {
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      auto field_iter = base_field_index.find(normalize_lower(iter->second));
      if (field_iter == base_field_index.end()) {
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      rewritten_row[field_iter->second] = row[i];
    }
    row = std::move(rewritten_row);
    return RC::SUCCESS;
  };

  if (!insert_sql.value_rows.empty()) {
    for (vector<Value> &row : insert_sql.value_rows) {
      rc = rewrite_one_row(row);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  } else {
    rc = rewrite_one_row(insert_sql.values);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  insert_sql.relation_name = base_table;
  return RC::SUCCESS;
}

}  // namespace

RC rewrite_sql_for_view(Db *db, ParsedSqlNode &sql_node)
{
  if (db == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  switch (sql_node.flag) {
    case SCF_SELECT: {
      RC rc = rewrite_select_sql(db, sql_node.selection, 0);
      if (OB_FAIL(rc)) {
        return rc;
      }
      // If the rewritten SELECT has no FROM and only constants,
      // convert to CALC so it executes as a single-row expression eval
      // (FROM-less SELECTs may not execute correctly).
      SelectSqlNode &sel = sql_node.selection;
      if (sel.relations.empty() && sel.condition_expr == nullptr && sel.group_by.empty() &&
          !sel.expressions.empty()) {
        bool all_value = true;
        for (const auto &e : sel.expressions) {
          if (e->type() != ExprType::VALUE) {
            all_value = false;
            break;
          }
        }
        if (all_value) {
          sql_node.flag = SCF_CALC;
          sql_node.calc.expressions.swap(sel.expressions);
        }
      }
      return rc;
    }
    case SCF_UPDATE: {
      return rewrite_update_sql(db, sql_node.update);
    }
    case SCF_DELETE: {
      return rewrite_delete_sql(db, sql_node.deletion);
    }
    case SCF_INSERT: {
      return rewrite_insert_sql(db, sql_node.insertion);
    }
    default: {
      return RC::SUCCESS;
    }
  }
}
