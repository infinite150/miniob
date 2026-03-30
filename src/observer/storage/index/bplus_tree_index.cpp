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
// Created by wangyunlai.wyl on 2021/5/19.
//

#include "storage/index/bplus_tree_index.h"
#include "common/lang/span.h"
#include "common/log/log.h"
#include "session/session.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "storage/db/db.h"
#include "storage/trx/trx.h"

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

/// 唯一索引：SQL NULL 不参与唯一性；索引列任一为 NULL 时须按 (key,RID) 区分
static bool unique_index_key_contains_null(Table *table, const vector<FieldMeta> &idx_fields, const char *record)
{
  if (table == nullptr || record == nullptr || idx_fields.empty()) {
    return false;
  }
  const TableMeta &tm    = table->table_meta();
  const int        boff  = tm.null_bitmap_offset();
  const auto      *bytes = reinterpret_cast<const unsigned char *>(record + boff);
  for (const FieldMeta &fm : idx_fields) {
    for (int j = 0; j < tm.field_num(); j++) {
      const FieldMeta *f = tm.field(j);
      if (f != nullptr && 0 == strcmp(f->name(), fm.name())) {
        if (((bytes[j / 8] >> (j % 8)) & 1U) != 0) {
          return true;
        }
        break;
      }
    }
  }
  return false;
}

static bool extract_mvcc_trx_id_from_new_record(Table *table, const char *new_record, int32_t &trx_id)
{
  if (table == nullptr || new_record == nullptr) {
    return false;
  }
  const auto trx_fields = table->table_meta().trx_fields();
  if (trx_fields.size() < 2) {
    return false;
  }
  const int32_t begin_xid = *reinterpret_cast<const int32_t *>(new_record + trx_fields[0].offset());
  if (begin_xid >= 0) {
    return false;
  }
  trx_id = -begin_xid;
  return true;
}

static bool is_deleted_by_this_trx(Table *table, const Record &old_record, int32_t trx_id)
{
  if (table == nullptr || trx_id <= 0 || old_record.data() == nullptr) {
    return false;
  }
  const auto trx_fields = table->table_meta().trx_fields();
  if (trx_fields.size() < 2) {
    return false;
  }
  const int32_t end_xid = *reinterpret_cast<const int32_t *>(old_record.data() + trx_fields[1].offset());
  return end_xid == -trx_id;
}

/// 唯一键冲突消解需要当前事务号：优先从新行 begin_xid 解析；失败时用 Session 上的 MVCC 事务（多连接脚本场景）。
static bool resolve_trx_id_for_mvcc_dup_fixup(Table *table, const char *new_record, int32_t &trx_id)
{
  if (extract_mvcc_trx_id_from_new_record(table, new_record, trx_id)) {
    return true;
  }
  Session *session = Session::current_session();
  Trx     *trx     = session != nullptr ? session->current_trx() : nullptr;
  if (trx != nullptr && trx->type() == TrxKit::Type::MVCC) {
    trx_id = trx->id();
    return true;
  }
  return false;
}

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, field_meta.type(), field_meta.len(),
      -1, -1, index_meta.unique());
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }
  if (field_metas.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  Index::init(index_meta, field_metas);

  vector<std::pair<AttrType, int>> fields;
  for (const FieldMeta *fm : field_metas) {
    fields.push_back({fm->type(), fm->len()});
  }

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = bpm.create_file(file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index file. file_name:%s, rc:%s", file_name, strrc(rc));
    return rc;
  }

  DiskBufferPool *bp = nullptr;
  rc = bpm.open_file(table->db()->log_handler(), file_name, bp);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index file. file_name:%s, rc:%s", file_name, strrc(rc));
    return rc;
  }

  rc = index_handler_.create(table->db()->log_handler(), *bp, span<const std::pair<AttrType, int>>(fields.data(), fields.size()),
      -1, -1, index_meta.unique());
  if (RC::SUCCESS != rc) {
    bpm.close_file(file_name);
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create multi-field index, file_name:%s, index:%s", file_name, index_meta.name());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_meta);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const vector<const FieldMeta *> &field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been inited before. file_name:%s, index:%s",
        file_name, index_meta.name());
    return RC::RECORD_OPENNED;
  }
  if (field_metas.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, rc:%s",
        file_name, index_meta.name(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open multi-field index, file_name:%s, index:%s", file_name, index_meta.name());
  return RC::SUCCESS;
}

RC BplusTreeIndex::close()
{
  if (inited_) {
    LOG_INFO("Begin to close index, index:%s, field:%s", index_meta_.name(), index_meta_.field());
    index_handler_.close();
    inited_ = false;
  }
  LOG_INFO("Successfully close index.");
  return RC::SUCCESS;
}

RC BplusTreeIndex::insert_entry(const char *record, const RID *rid)
{
  const bool null_in_key =
      index_meta_.unique() && unique_index_key_contains_null(table_, field_metas_, record);
  RC rc = RC::SUCCESS;
  if (field_metas_.size() == 1) {
    rc = index_handler_.insert_entry(record + field_metas_[0].offset(), rid, null_in_key);
    if (rc != RC::RECORD_DUPLICATE_KEY || !index_meta_.unique() || null_in_key) {
      return rc;
    }

    int32_t current_trx_id = 0;
    if (!resolve_trx_id_for_mvcc_dup_fixup(table_, record, current_trx_id)) {
      return rc;
    }

    unique_ptr<IndexScanner> scanner(
        create_scanner(record + field_metas_[0].offset(), field_metas_[0].len(), true,
            record + field_metas_[0].offset(), field_metas_[0].len(), true));
    if (scanner == nullptr) {
      return rc;
    }

    bool        has_conflict = false;
    vector<RID> invisible_rids;
    RID         dup_rid;
    while (OB_SUCC(scanner->next_entry(&dup_rid))) {
      Record dup_record;
      RC     grc = table_->get_record(dup_rid, dup_record);
      if (OB_FAIL(grc)) {
        continue;
      }

      if (is_deleted_by_this_trx(table_, dup_record, current_trx_id)) {
        invisible_rids.emplace_back(dup_rid);
      } else {
        has_conflict = true;
        break;
      }
    }

    if (has_conflict) {
      LOG_WARN("unique index duplicate during mvcc update. index=%s table=%s rc=%s",
          index_meta_.name(), table_ ? table_->name() : "null", strrc(rc));
      return rc;
    }
    scanner.reset();
    for (const RID &invisible_rid : invisible_rids) {
      // UPDATE(delete+insert) in MVCC may leave old version occupying unique key in index.
      // If it's already invisible to current trx, remove that stale index entry and retry.
      RC drc = index_handler_.delete_entry(record + field_metas_[0].offset(), &invisible_rid, false);
      if (drc != RC::SUCCESS && drc != RC::RECORD_NOT_EXIST) {
        return rc;
      }
    }
    return index_handler_.insert_entry(record + field_metas_[0].offset(), rid, null_in_key);
  }
  char key_buf[256];
  int  offset = 0;
  for (const FieldMeta &fm : field_metas_) {
    if (offset + fm.len() > (int)sizeof(key_buf)) {
      LOG_WARN("composite key too long");
      return RC::INTERNAL;
    }
    memcpy(key_buf + offset, record + fm.offset(), fm.len());
    offset += fm.len();
  }
  rc = index_handler_.insert_entry(key_buf, rid, null_in_key);
  if (rc != RC::RECORD_DUPLICATE_KEY || !index_meta_.unique() || null_in_key) {
    return rc;
  }

  int32_t current_trx_id = 0;
  if (!resolve_trx_id_for_mvcc_dup_fixup(table_, record, current_trx_id)) {
    return rc;
  }

  unique_ptr<IndexScanner> scanner(create_scanner(key_buf, offset, true, key_buf, offset, true));
  if (scanner == nullptr) {
    return rc;
  }

  bool        has_conflict = false;
  vector<RID> invisible_rids;
  RID         dup_rid;
  while (OB_SUCC(scanner->next_entry(&dup_rid))) {
    Record dup_record;
    RC     grc = table_->get_record(dup_rid, dup_record);
    if (OB_FAIL(grc)) {
      continue;
    }

    if (is_deleted_by_this_trx(table_, dup_record, current_trx_id)) {
      invisible_rids.emplace_back(dup_rid);
    } else {
      has_conflict = true;
      break;
    }
  }

  if (has_conflict) {
    LOG_WARN("unique index duplicate during mvcc update. index=%s table=%s rc=%s",
        index_meta_.name(), table_ ? table_->name() : "null", strrc(rc));
    return rc;
  }
  scanner.reset();
  for (const RID &invisible_rid : invisible_rids) {
    RC drc = index_handler_.delete_entry(key_buf, &invisible_rid, false);
    if (drc != RC::SUCCESS && drc != RC::RECORD_NOT_EXIST) {
      return rc;
    }
  }
  return index_handler_.insert_entry(key_buf, rid, null_in_key);
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  const bool null_in_key =
      index_meta_.unique() && unique_index_key_contains_null(table_, field_metas_, record);
  if (field_metas_.size() == 1) {
    return index_handler_.delete_entry(record + field_metas_[0].offset(), rid, null_in_key);
  }
  char key_buf[256];
  int  offset = 0;
  for (const FieldMeta &fm : field_metas_) {
    if (offset + fm.len() > (int)sizeof(key_buf)) {
      LOG_WARN("composite key too long");
      return RC::INTERNAL;
    }
    memcpy(key_buf + offset, record + fm.offset(), fm.len());
    offset += fm.len();
  }
  return index_handler_.delete_entry(key_buf, rid, null_in_key);
}

IndexScanner *BplusTreeIndex::create_scanner(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  BplusTreeIndexScanner *index_scanner = new BplusTreeIndexScanner(index_handler_);
  RC rc = index_scanner->open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open index scanner. rc=%d:%s", rc, strrc(rc));
    delete index_scanner;
    return nullptr;
  }
  return index_scanner;
}

RC BplusTreeIndex::sync() { return index_handler_.sync(); }

////////////////////////////////////////////////////////////////////////////////
BplusTreeIndexScanner::BplusTreeIndexScanner(BplusTreeHandler &tree_handler) : tree_scanner_(tree_handler) {}

BplusTreeIndexScanner::~BplusTreeIndexScanner() noexcept { tree_scanner_.close(); }

RC BplusTreeIndexScanner::open(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  return tree_scanner_.open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
}

RC BplusTreeIndexScanner::next_entry(RID *rid) { return tree_scanner_.next_entry(*rid); }

RC BplusTreeIndexScanner::destroy()
{
  delete this;
  return RC::SUCCESS;
}
