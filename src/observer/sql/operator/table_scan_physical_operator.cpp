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
// Created by WangYunlai on 2021/6/9.
//

#include "sql/operator/table_scan_physical_operator.h"
#include "event/sql_debug.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "storage/table/table.h"

using namespace std;

RC TableScanPhysicalOperator::open(Trx *trx)
{
  for (unique_ptr<Expression> &expr : predicates_) {
    ExpressionIterator::for_each_subquery(*expr, [trx](SubQueryExpr &sq) {
      sq.set_trx(trx);
    });
  }
  table_->add_ref();
  RC rc = table_->get_record_scanner(record_scanner_, trx, mode_);
  if (rc == RC::SUCCESS) {
    tuple_.set_schema(table_, table_->table_meta().field_metas());
    if (!table_alias_.empty()) { tuple_.set_table_alias(table_alias_); }
  }
  trx_ = trx;
  return rc;
}

RC TableScanPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;

  bool filter_result = false;
  while (OB_SUCC(rc = record_scanner_->next(current_record_))) {
    LOG_TRACE("got a record. rid=%s", current_record_.rid().to_string().c_str());
    
    tuple_.set_record(&current_record_);
    rc = filter(tuple_, filter_result);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("record filtered failed=%s", strrc(rc));
      return rc;
    }

    if (filter_result) {
      sql_debug("get a tuple: %s", tuple_.to_string().c_str());
      break;
    } else {
      sql_debug("a tuple is filtered: %s", tuple_.to_string().c_str());
    }
  }
  return rc;
}

RC TableScanPhysicalOperator::close() {
  RC rc = RC::SUCCESS;
  if (table_ != nullptr) {
    table_->release();
  }
  if (record_scanner_ != nullptr) {
    rc = record_scanner_->close_scan();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close record scanner");
    }
    delete record_scanner_;
    record_scanner_ = nullptr;
  }
  return rc;

}

Tuple *TableScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

string TableScanPhysicalOperator::param() const { return table_->name(); }

void TableScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC TableScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  const Tuple *eval_tuple = &tuple;
  if (parent_tuple_ != nullptr) {
    // 子查询/相关子查询场景：当父层与子层来自同一张表且谓词使用了未限定列名时，
    // JoinedTuple::find_cell 默认优先查 left_，会导致谓词错误地读取到父层字段。
    // 这里将“扫描到的当前 tuple”放在 left_，从而让内层扫描结果优先匹配。
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
