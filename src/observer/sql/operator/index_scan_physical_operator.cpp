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
// Created by Wangyunlai on 2022/07/08.
//

#include "sql/operator/index_scan_physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "storage/index/index.h"
#include "storage/trx/trx.h"

IndexScanPhysicalOperator::IndexScanPhysicalOperator(Table *table, Index *index, ReadWriteMode mode, const Value *left_value,
    bool left_inclusive, const Value *right_value, bool right_inclusive)
    : table_(table),
      index_(index),
      mode_(mode),
      left_inclusive_(left_inclusive),
      right_inclusive_(right_inclusive)
{
  if (left_value) {
    left_value_ = *left_value;
  }
  if (right_value) {
    right_value_ = *right_value;
  }
}

RC IndexScanPhysicalOperator::open(Trx *trx)
{
  if (nullptr == table_ || nullptr == index_) {
    return RC::INTERNAL;
  }
  for (unique_ptr<Expression> &expr : predicates_) {
    ExpressionIterator::for_each_subquery(*expr, [trx](SubQueryExpr &sq) {
      sq.set_trx(trx);
    });
  }
  table_->add_ref();

  const char *left_key   = left_value_.data();
  int         left_len   = left_value_.length();
  const char *right_key  = right_value_.data();
  int         right_len  = right_value_.length();
  char        left_buf[256];
  char        right_buf[256];

  int prefix_len = index_->build_prefix_range_keys(left_value_, left_buf, right_buf, sizeof(left_buf));
  if (prefix_len > 0) {
    left_key  = left_buf;
    right_key = right_buf;
    left_len = right_len = prefix_len;
  }

  IndexScanner *index_scanner = index_->create_scanner(left_key, left_len, left_inclusive_, right_key, right_len, right_inclusive_);
  if (nullptr == index_scanner) {
    LOG_WARN("failed to create index scanner");
    return RC::INTERNAL;
  }
  index_scanner_ = index_scanner;

  tuple_.set_schema(table_, table_->table_meta().field_metas());

  trx_ = trx;
  return RC::SUCCESS;
}

RC IndexScanPhysicalOperator::next()
{
  // TODO: 需要适配 lsm-tree 引擎
  RID rid;
  RC  rc = RC::SUCCESS;

  bool filter_result = false;
  while (RC::SUCCESS == (rc = index_scanner_->next_entry(&rid))) {
    rc = table_->get_record(rid, current_record_);
    if (OB_FAIL(rc)) {
      LOG_TRACE("failed to get record. rid=%s, rc=%s", rid.to_string().c_str(), strrc(rc));
      return rc;
    }

    LOG_TRACE("got a record. rid=%s", rid.to_string().c_str());

    tuple_.set_record(&current_record_);
    rc = filter(tuple_, filter_result);
    if (OB_FAIL(rc)) {
      LOG_TRACE("failed to filter record. rc=%s", strrc(rc));
      return rc;
    }

    if (!filter_result) {
      LOG_TRACE("record filtered");
      continue;
    }

    rc = trx_->visit_record(table_, current_record_, mode_);
    if (rc == RC::RECORD_INVISIBLE) {
      LOG_TRACE("record invisible");
      continue;
    } else {
      return rc;
    }
  }

  return rc;
}

RC IndexScanPhysicalOperator::close()
{
  if (table_ != nullptr) {
    table_->release();
  }
  if (index_scanner_ != nullptr) {
    index_scanner_->destroy();
    index_scanner_ = nullptr;
  }
  return RC::SUCCESS;
}

Tuple *IndexScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

void IndexScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC IndexScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  const Tuple *eval_tuple = &tuple;
  if (parent_tuple_ != nullptr) {
    // 与 TableScan / Predicate 一致：内层扫描行在 left，父层 tuple 在 right，
    // 未限定列名时优先匹配当前扫描表，避免命中外层同名列。
    combined_tuple_.set_left(&tuple);
    combined_tuple_.set_right(const_cast<Tuple *>(parent_tuple_));
    eval_tuple = &combined_tuple_;
  }
  for (unique_ptr<Expression> &expr : predicates_) {
    rc = expr->get_value(*eval_tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool tmp_result = value.get_boolean();
    if (!tmp_result) {
      result = false;
      return rc;
    }
  }

  result = true;
  return rc;
}

string IndexScanPhysicalOperator::param() const
{
  return string(index_->index_meta().name()) + " ON " + table_->name();
}
