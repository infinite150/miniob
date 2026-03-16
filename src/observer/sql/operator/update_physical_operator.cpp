/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "sql/expr/tuple.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const FieldMeta *field_meta, const Value &value)
    : table_(table), field_meta_(field_meta), value_(value)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  unique_ptr<PhysicalOperator> &child = children_[0];
  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  trx_ = trx;

  // 第一阶段：遍历子算子，收集待更新记录
  while (OB_SUCC(rc = child->next())) {
    Tuple *tuple = child->current_tuple();
    if (nullptr == tuple) {
      LOG_WARN("failed to get current record: %s", strrc(rc));
      break;
    }

    RowTuple *row_tuple  = static_cast<RowTuple *>(tuple);
    Record   &old_record = row_tuple->record();

    records_.emplace_back(std::move(old_record));
  }

  child->close();

  if (rc != RC::RECORD_EOF && rc != RC::SUCCESS) {
    LOG_WARN("failed to iterate records for update. rc=%s", strrc(rc));
    return rc;
  }

  // 第二阶段：对收集到的记录执行 delete + insert 完成更新
  for (Record &old_record : records_) {
    Record new_record;
    RC     rc2 = new_record.copy_data(old_record.data(), old_record.len());
    if (rc2 != RC::SUCCESS) {
      LOG_WARN("failed to copy record data");
      return rc2;
    }

    rc2 = table_->update_record_field(new_record, field_meta_, value_);
    if (rc2 != RC::SUCCESS) {
      LOG_WARN("failed to update record field: %s", strrc(rc2));
      return rc2;
    }

    rc2 = trx_->delete_record(table_, old_record);
    if (rc2 != RC::SUCCESS) {
      LOG_WARN("failed to delete record: %s", strrc(rc2));
      return rc2;
    }

    rc2 = trx_->insert_record(table_, new_record);
    if (rc2 != RC::SUCCESS) {
      LOG_WARN("failed to insert record: %s", strrc(rc2));
      return rc2;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next() { return RC::RECORD_EOF; }

RC UpdatePhysicalOperator::close() { return RC::SUCCESS; }

