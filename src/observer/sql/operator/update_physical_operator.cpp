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

#include <unordered_map>

#include "common/log/log.h"
#include "common/value.h"
#include "session/session.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include "storage/table/table.h"
#include "storage/trx/mvcc_trx.h"
#include "storage/trx/trx.h"
#include "sql/expr/tuple.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(
    Table *table, const vector<const FieldMeta *> &field_metas, vector<unique_ptr<Expression>> *rhs_exprs, Session *subquery_session)
    : table_(table), field_metas_(field_metas), rhs_exprs_(rhs_exprs), trx_(nullptr), subquery_session_(subquery_session)
{}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  trx_ = trx;
  child_closed_ = false;
  if (table_ != nullptr) {
    table_->add_ref();
  }
  Session *session = subquery_session_ != nullptr ? subquery_session_ : Session::current_session();
  if (rhs_exprs_ != nullptr) {
    for (auto &e : *rhs_exprs_) {
      if (e) {
        RC prep_rc = RC::SUCCESS;
        ExpressionIterator::for_each_subquery(*e, [session, trx, &prep_rc](SubQueryExpr &sq) {
          if (prep_rc != RC::SUCCESS) {
            return;
          }
          if (session != nullptr) {
            RC rc = sq.generate_logical_oper();
            if (OB_FAIL(rc)) {
              prep_rc = rc;
              return;
            }
            rc = sq.generate_physical_oper(session);
            if (OB_FAIL(rc)) {
              prep_rc = rc;
              return;
            }
          }
          sq.set_trx(trx);
        });
        if (OB_FAIL(prep_rc)) {
          LOG_WARN("UpdatePhysicalOperator: subquery plan generation failed. rc=%s", strrc(prep_rc));
          // 仅由 add_ref 对称：失败时 SqlResult 仍会 close()，此处不得 release，避免双重 release
          return prep_rc;
        }
      }
    }
  }
  if (children_.empty()) {
    LOG_WARN("UpdatePhysicalOperator::open has no child, table=%s", table_ ? table_->name() : "null");
    return RC::SUCCESS;
  }

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("UpdatePhysicalOperator::open failed at children_[0]->open. rc=%s", strrc(rc));
    return rc;
  }

  vector<Record> old_records;
  while (OB_SUCC(rc = children_[0]->next())) {
    Tuple *tuple = children_[0]->current_tuple();
    if (tuple == nullptr) {
      LOG_WARN("UpdatePhysicalOperator::open failed at children_[0]->current_tuple, got null tuple");
      return RC::INTERNAL;
    }
    RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
    Record   &record   = row_tuple->record();

    Record rec_copy;
    rc = rec_copy.copy_data(record.data(), record.len());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy record data. rc=%s", strrc(rc));
      return rc;
    }
    rec_copy.set_rid(record.rid());
    old_records.emplace_back(std::move(rec_copy));
  }

  RC close_rc = children_[0]->close();
  if (OB_FAIL(close_rc)) {
    LOG_WARN("UpdatePhysicalOperator::open failed at children_[0]->close. rc=%s", strrc(close_rc));
    return close_rc;
  }
  child_closed_ = true;

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("child operator error. rc=%s", strrc(rc));
    return rc;
  }

  auto copy_record_with_rid = [](const Record &src, Record &dst) -> RC {
    RC rc = dst.copy_data(src.data(), src.len());
    if (OB_FAIL(rc)) {
      return rc;
    }
    dst.set_rid(src.rid());
    return RC::SUCCESS;
  };

  // Keep statement-local bookkeeping for rollback; do not infer via index math.
  vector<Record> inserted_new_records;
  vector<Record> deleted_old_records;
  for (Record &old_record : old_records) {
    Record new_record;
    rc = build_new_record(old_record, new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at build_new_record. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }

    rc = trx_->delete_record(table_, old_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at trx_->delete_record. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }

    deleted_old_records.emplace_back();
    rc = copy_record_with_rid(old_record, deleted_old_records.back());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy old record after delete. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }

    rc = trx_->insert_record(table_, new_record);
    if (OB_FAIL(rc)) {
      LOG_WARN("UpdatePhysicalOperator::open failed at trx_->insert_record. table=%s old_rid=%s rc=%s",
          table_ ? table_->name() : "null", old_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }

    inserted_new_records.emplace_back();
    rc = copy_record_with_rid(new_record, inserted_new_records.back());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to copy new record after insert. table=%s rid=%s rc=%s",
          table_ ? table_->name() : "null", new_record.rid().to_string().c_str(), strrc(rc));
      goto rollback;
    }
  }

  return RC::SUCCESS;

rollback:
  // Statement-level compensation for explicit transactions:
  // rollback only changes done by this UPDATE statement.
  MvccTrx *mvcc_trx = nullptr;
  if (trx_ != nullptr && trx_->type() == TrxKit::Type::MVCC) {
    mvcc_trx = static_cast<MvccTrx *>(trx_);
  }

  auto rollback_old_record = [this, mvcc_trx](Record &old_record, const char *context) {
    if (mvcc_trx != nullptr) {
      RC rc2 = mvcc_trx->rollback_delete_for_statement(table_, old_record);
      if (OB_FAIL(rc2)) {
        LOG_ERROR("rollback: failed to restore old record in mvcc. context=%s rid=%s rc=%s",
            context, old_record.rid().to_string().c_str(), strrc(rc2));
      }
      return;
    }

    RC rc2 = trx_->insert_record(table_, old_record);
    if (OB_SUCC(rc2)) {
      return;
    }

    LOG_ERROR("rollback: failed to restore old record. context=%s rid=%s rc=%s",
        context, old_record.rid().to_string().c_str(), strrc(rc2));
  };

  for (auto iter = inserted_new_records.rbegin(); iter != inserted_new_records.rend(); ++iter) {
    RC rc2 = trx_->delete_record(table_, *iter);
    if (OB_FAIL(rc2)) {
      LOG_ERROR("rollback: failed to delete new record. rid=%s rc=%s",
          iter->rid().to_string().c_str(), strrc(rc2));
    }
  }

  for (auto iter = deleted_old_records.rbegin(); iter != deleted_old_records.rend(); ++iter) {
    rollback_old_record(*iter, "restore-deleted-old");
  }

  return rc;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  if (table_ != nullptr) {
    table_->release();
  }
  if (!children_.empty() && !child_closed_) {
    RC rc = children_[0]->close();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close child operator. rc=%s", strrc(rc));
    }
  }
  child_closed_ = false;
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  schema.clear();
  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::build_new_record(const Record &old_record, Record &new_record) const
{
  if (table_ == nullptr || field_metas_.empty() || rhs_exprs_ == nullptr
      || rhs_exprs_->size() != field_metas_.size()) {
    return RC::INTERNAL;
  }

  const TableMeta &table_meta = table_->table_meta();
  const int        sys_fields = table_meta.sys_field_num();
  const int        user_fields = table_meta.field_num() - sys_fields;

  unordered_map<string, const Expression *> update_map;
  for (size_t i = 0; i < field_metas_.size(); i++) {
    update_map[field_metas_[i]->name()] = (*rhs_exprs_)[i].get();
  }

  RowTuple tuple;
  tuple.set_schema(table_, table_meta.field_metas());
  tuple.set_record(const_cast<Record *>(&old_record));

  vector<Value> values;
  values.reserve(user_fields);

  for (int i = 0; i < user_fields; i++) {
    const FieldMeta *field = table_meta.field(i + sys_fields);
    if (field == nullptr) {
      return RC::INTERNAL;
    }
    auto it = update_map.find(field->name());
    if (it != update_map.end()) {
      Value cell;
      RC rc = it->second->get_value(tuple, cell);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (cell.is_null()) {
        if (!field->nullable()) {
          LOG_WARN("cannot set NULL on NOT NULL field. table=%s, field=%s", table_->name(), field->name());
          return RC::INVALID_ARGUMENT;
        }
      } else if (field->type() != cell.attr_type()) {
        Value casted;
        rc = Value::cast_to(cell, field->type(), casted);
        if (OB_FAIL(rc)) {
          LOG_WARN("field type mismatch on update. table=%s, field=%s", table_->name(), field->name());
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        cell = std::move(casted);
      }
      values.emplace_back(std::move(cell));
    } else {
      Value cell;
      RC rc = tuple.cell_at(i + sys_fields, cell);
      if (OB_FAIL(rc)) {
        return rc;
      }
      values.emplace_back(std::move(cell));
    }
  }

  return table_->make_record(static_cast<int>(values.size()), values.data(), new_record);
}
