// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/pgsql_operation.h"

#include <boost/optional/optional_io.hpp>

#include "yb/common/ql_storage_interface.h"

#include "yb/docdb/doc_pgsql_scanspec.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/docdb_util.h"

#include "yb/util/trace.h"

DECLARE_bool(trace_docdb_calls);

namespace yb {
namespace docdb {

namespace {

CHECKED_STATUS CreateProjection(const Schema& schema,
                                const PgsqlColumnRefsPB& column_refs,
                                Schema* projection) {
  // Create projection of non-primary key columns. Primary key columns are implicitly read by DocDB.
  // It will also sort the columns before scanning.
  vector<ColumnId> column_ids;
  column_ids.reserve(column_refs.ids_size());
  for (int32_t id : column_refs.ids()) {
    const ColumnId column_id(id);
    if (!schema.is_key_column(column_id)) {
      column_ids.emplace_back(column_id);
    }
  }
  return schema.CreateProjectionByIdsIgnoreMissing(column_ids, projection);
}

} // namespace

//--------------------------------------------------------------------------------------------------

Status PgsqlWriteOperation::Init(PgsqlWriteRequestPB* request, PgsqlResponsePB* response) {
  // Initialize operation inputs.
  request_.Swap(request);
  response_ = response;

  // Init DocDB keys using partition and range values.
  // - Collect partition and range values into hashed_components and range_components.
  // - Setup the keys.
  if (request_.has_ybctid_column_value()) {
    CHECK(request_.ybctid_column_value().has_value() &&
          request_.ybctid_column_value().value().has_binary_value())
      << "ERROR: Unexpected value for ybctid column";
    const string& ybctid_value = request_.ybctid_column_value().value().binary_value();
    Slice key_value(ybctid_value.data(), ybctid_value.size());

    // The following code assumes that ybctid is the key of exactly one row, so the hash_doc_key_
    // is set to NULL. If this assumption is no longer true, hash_doc_key_ should be assigned with
    // appropriate values.
    range_doc_key_.emplace();
    RETURN_NOT_OK(range_doc_key_->DecodeFrom(key_value));
    encoded_range_doc_key_ = range_doc_key_->EncodeAsRefCntPrefix();
  } else {
    vector<PrimitiveValue> hashed_components;
    RETURN_NOT_OK(InitKeyColumnPrimitiveValues(request_.partition_column_values(),
                                               schema_,
                                               0,
                                               &hashed_components));

    // We only need the hash key if the range key is not specified.
    if (request_.range_column_values().size() == 0) {
      hashed_doc_key_.emplace(schema_, request_.hash_code(), hashed_components);
      encoded_hashed_doc_key_ = hashed_doc_key_->EncodeAsRefCntPrefix();
    }

    vector<PrimitiveValue> range_components;
    RETURN_NOT_OK(InitKeyColumnPrimitiveValues(request_.range_column_values(),
                                               schema_,
                                               schema_.num_hash_key_columns(),
                                               &range_components));
    if (hashed_components.empty()) {
      range_doc_key_.emplace(schema_, range_components);
    } else {
      range_doc_key_.emplace(schema_, request_.hash_code(), hashed_components, range_components);
    }
    encoded_range_doc_key_ = range_doc_key_->EncodeAsRefCntPrefix();
  }

  return Status::OK();
}

Status PgsqlWriteOperation::Apply(const DocOperationApplyData& data) {
  VLOG(4) << "Write, read time: " << data.read_time << ", txn: " << txn_op_context_;

  switch (request_.stmt_type()) {
    case PgsqlWriteRequestPB::PGSQL_INSERT:
      return ApplyInsert(data);

    case PgsqlWriteRequestPB::PGSQL_UPDATE:
      return ApplyUpdate(data);

    case PgsqlWriteRequestPB::PGSQL_DELETE:
      return ApplyDelete(data);
  }
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyInsert(const DocOperationApplyData& data) {
  QLTableRow::SharedPtr table_row = std::make_shared<QLTableRow>();
  RETURN_NOT_OK(ReadColumns(data, table_row));
  if (!table_row->IsEmpty()) {
    // Primary key or unique index value found.
    return STATUS(QLError, "Duplicate key found in primary key or unique index");
  }

  const MonoDelta ttl = Value::kMaxTtl;
  const UserTimeMicros user_timestamp = Value::kInvalidUserTimestamp;

  // Add the appropriate liveness column.
  if (encoded_range_doc_key_) {
    const DocPath sub_path(encoded_range_doc_key_.as_slice(),
                           PrimitiveValue::SystemColumnId(SystemColumnIds::kLivenessColumn));
    const auto value = Value(PrimitiveValue(), ttl, user_timestamp);
    RETURN_NOT_OK(data.doc_write_batch->SetPrimitive(
        sub_path, value, data.read_time, data.deadline, request_.stmt_id()));
  }

  for (const auto& column_value : request_.column_values()) {
    // Get the column.
    if (!column_value.has_column_id()) {
      return STATUS_FORMAT(InvalidArgument, "column id missing: $0",
                           column_value.DebugString());
    }
    const ColumnId column_id(column_value.column_id());
    auto column = schema_.column_by_id(column_id);
    RETURN_NOT_OK(column);

    // Check column-write operator.
    CHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert)
      << "Illegal write instruction";

    // Evaluate column value.
    QLValue expr_result;
    RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, &expr_result));
    const SubDocument sub_doc =
        SubDocument::FromQLValuePB(expr_result.value(), column->sorting_type());

    // Inserting into specified column.
    DocPath sub_path(encoded_range_doc_key_.as_slice(), PrimitiveValue(column_id));
    RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
        sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id(), ttl, user_timestamp));
  }

  RETURN_NOT_OK(PopulateResultSet(table_row));

  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyUpdate(const DocOperationApplyData& data) {
  QLTableRow::SharedPtr table_row = std::make_shared<QLTableRow>();
  RETURN_NOT_OK(ReadColumns(data, table_row));
  // skipped is set to false if this operation produces some data to write.
  bool skipped = true;

  if (request_.has_ybctid_column_value()) {
    for (const auto& column_value : request_.column_new_values()) {
      // Get the column.
      if (!column_value.has_column_id()) {
        return STATUS_FORMAT(InvalidArgument, "column id missing: $0",
                             column_value.DebugString());
      }
      const ColumnId column_id(column_value.column_id());
      auto column = schema_.column_by_id(column_id);
      RETURN_NOT_OK(column);

      // Check column-write operator.
      CHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert)
        << "Illegal write instruction";

      // Evaluate column value.
      QLValue expr_result;
      RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, &expr_result));

      // Compare with existing value.
      QLValue old_value;
      RETURN_NOT_OK(EvalColumnRef(column_value.column_id(), table_row, &old_value));

      // Inserting into specified column.
      if (expr_result != old_value) {
        const SubDocument sub_doc =
          SubDocument::FromQLValuePB(expr_result.value(), column->sorting_type());
        DocPath sub_path(encoded_range_doc_key_.as_slice(), PrimitiveValue(column_id));
        RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
            sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id()));
        skipped = false;
      }
    }
  } else {
    // This UPDATE is calling PGGATE directly without going thru PosgreSQL layer.
    // Keep it here as we might need it.


    // Very limited support for where expressions. Only used for updates to the sequences data
    // table.
    bool is_match = true;
    if (request_.has_where_expr()) {
      QLValue match;
      RETURN_NOT_OK(EvalExpr(request_.where_expr(), table_row, &match));
      is_match = match.bool_value();
    }

    if (is_match) {
      for (const auto &column_value : request_.column_new_values()) {
        // Get the column.
        if (!column_value.has_column_id()) {
          return STATUS_FORMAT(InvalidArgument, "column id missing: $0",
                               column_value.DebugString());
        }
        const ColumnId column_id(column_value.column_id());
        auto column = schema_.column_by_id(column_id);
        RETURN_NOT_OK(column);

        // Check column-write operator.
        CHECK(GetTSWriteInstruction(column_value.expr()) == bfpg::TSOpcode::kScalarInsert)
        << "Illegal write instruction";

        // Evaluate column value.
        QLValue expr_result;
        RETURN_NOT_OK(EvalExpr(column_value.expr(), table_row, &expr_result));

        const SubDocument sub_doc =
            SubDocument::FromQLValuePB(expr_result.value(), column->sorting_type());

        // Inserting into specified column.
        DocPath sub_path(encoded_range_doc_key_.as_slice(), PrimitiveValue(column_id));
        RETURN_NOT_OK(data.doc_write_batch->InsertSubDocument(
            sub_path, sub_doc, data.read_time, data.deadline, request_.stmt_id()));
        skipped = false;
      }
    }
  }

  // Returning the values before the update.
  RETURN_NOT_OK(PopulateResultSet(table_row));

  if (skipped) {
    response_->set_skipped(true);
  }
  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ApplyDelete(const DocOperationApplyData& data) {
  QLTableRow::SharedPtr table_row = std::make_shared<QLTableRow>();
  RETURN_NOT_OK(ReadColumns(data, table_row));

  // TODO(neil) Add support for WHERE clause.
  CHECK(request_.column_values_size() == 0) << "WHERE clause condition is not yet fully supported";

  // Otherwise, delete the referenced row (all columns).
  RETURN_NOT_OK(data.doc_write_batch->DeleteSubDoc(DocPath(
      encoded_range_doc_key_.as_slice()), data.read_time, data.deadline));

  RETURN_NOT_OK(PopulateResultSet(table_row));

  response_->set_status(PgsqlResponsePB::PGSQL_STATUS_OK);
  return Status::OK();
}

Status PgsqlWriteOperation::ReadColumns(const DocOperationApplyData& data,
                                        const QLTableRow::SharedPtr& table_row) {
  // Filter the columns using primary key.
  if (range_doc_key_) {
    Schema projection;
    RETURN_NOT_OK(CreateProjection(schema_, request_.column_refs(), &projection));
    DocPgsqlScanSpec spec(projection, request_.stmt_id(), *range_doc_key_);
    DocRowwiseIterator iterator(projection,
                                schema_,
                                txn_op_context_,
                                data.doc_write_batch->doc_db(),
                                data.deadline,
                                data.read_time);
    RETURN_NOT_OK(iterator.Init(spec));
    if (iterator.HasNext()) {
      RETURN_NOT_OK(iterator.NextRow(table_row.get()));
    } else {
      table_row->Clear();
    }
    data.restart_read_ht->MakeAtLeast(iterator.RestartReadHt());
  }

  return Status::OK();
}

Status PgsqlWriteOperation::PopulateResultSet(const QLTableRow::SharedPtr& table_row) {
  PgsqlRSRow* rsrow = resultset_.AllocateRSRow(request_.targets().size());
  int rscol_index = 0;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    if (expr.has_column_id()) {
      if (expr.column_id() == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
        rsrow->rscol(rscol_index)->set_binary_value(encoded_range_doc_key_.data(),
                                                    encoded_range_doc_key_.size());
      } else {
        RETURN_NOT_OK(EvalExpr(expr, table_row, rsrow->rscol(rscol_index)));
      }
    }
    rscol_index++;
  }
  return Status::OK();
}

Status PgsqlWriteOperation::GetDocPaths(
    GetDocPathsMode mode, DocPathsToLock *paths, IsolationLevel *level) const {
  if (encoded_hashed_doc_key_) {
    paths->push_back(encoded_hashed_doc_key_);
  }
  if (encoded_range_doc_key_) {
    paths->push_back(encoded_range_doc_key_);
  }
  // When this write operation requires a read, it requires a read snapshot so paths will be locked
  // in snapshot isolation for consistency. Otherwise, pure writes will happen in serializable
  // isolation so that they will serialize but do not conflict with one another.
  //
  // Currently, only keys that are being written are locked, no lock is taken on read at the
  // snapshot isolation level.
  *level = RequireReadSnapshot() ? IsolationLevel::SNAPSHOT_ISOLATION
                                 : IsolationLevel::SERIALIZABLE_ISOLATION;
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgsqlReadOperation::Execute(const common::YQLStorageIf& ql_storage,
                                   CoarseTimePoint deadline,
                                   const ReadHybridTime& read_time,
                                   const Schema& schema,
                                   const Schema *index_schema,
                                   PgsqlResultSet *resultset,
                                   HybridTime *restart_read_ht) {
  VLOG(4) << "Read, read time: " << read_time << ", txn: " << txn_op_context_;

  size_t row_count_limit = std::numeric_limits<std::size_t>::max();
  if (request_.has_limit()) {
    if (request_.limit() == 0) {
      return Status::OK();
    }
    row_count_limit = request_.limit();
  }

  // Create the projection of regular columns selected by the row block plus any referenced in
  // the WHERE condition. When DocRowwiseIterator::NextRow() populates the value map, it uses this
  // projection only to scan sub-documents. The query schema is used to select only referenced
  // columns and key columns.
  Schema projection;
  Schema index_projection;
  common::YQLRowwiseIteratorIf *iter;

  RETURN_NOT_OK(CreateProjection(schema, request_.column_refs(), &projection));
  RETURN_NOT_OK(ql_storage.GetIterator(request_, projection, schema, txn_op_context_,
                                       deadline, read_time, &table_iter_));

  ColumnId ybbasectid_id;
  if (request_.has_index_request()) {
    const PgsqlReadRequestPB& index_request = request_.index_request();
    RETURN_NOT_OK(CreateProjection(*index_schema, index_request.column_refs(), &index_projection));
    RETURN_NOT_OK(ql_storage.GetIterator(index_request, index_projection, *index_schema,
                                         txn_op_context_, deadline, read_time, &index_iter_));
    iter = index_iter_.get();
    const size_t idx = index_schema->find_column("ybbasectid");
    if (idx == Schema::kColumnNotFound) {
      return STATUS(Corruption, "Column ybbasectid not found in index");
    }
    ybbasectid_id = index_schema->column_id(idx);
  } else {
    iter = table_iter_.get();
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Initialized iterator");
  }

  // Fetching data.
  int match_count = 0;
  QLTableRow::SharedPtr row = std::make_shared<QLTableRow>();
  while (resultset->rsrow_count() < row_count_limit && iter->HasNext()) {
    // The filtering process runs in the following order.
    // <hash_code><hash_components><range_components><regular_column_id> -> value;
    row->Clear();

    if (request_.has_index_request()) {
      QLValue row_key;
      RETURN_NOT_OK(iter->NextRow(row.get()));
      RETURN_NOT_OK(row->GetValue(ybbasectid_id, &row_key));
      RETURN_NOT_OK(table_iter_->Seek(row_key.binary_value()));
      if (!table_iter_->HasNext() ||
          VERIFY_RESULT(table_iter_->GetRowKey()) != row_key.binary_value()) {
        DocKey doc_key;
        RETURN_NOT_OK(doc_key.DecodeFrom(Slice(row_key.binary_value())));
        LOG(WARNING) << "Row key " << doc_key << " missing in indexed table";
        continue;
      }
      row->Clear();
      RETURN_NOT_OK(table_iter_->NextRow(projection, row.get()));
    } else {
      RETURN_NOT_OK(iter->NextRow(projection, row.get()));
    }

    // Match the row with the where condition before adding to the row block.
    bool is_match = true;
    if (request_.has_where_expr()) {
      QLValue match;
      RETURN_NOT_OK(EvalExpr(request_.where_expr(), row, &match));
      is_match = match.bool_value();
    }
    if (is_match) {
      match_count++;
      if (request_.is_aggregate()) {
        RETURN_NOT_OK(EvalAggregate(row));
      } else {
        RETURN_NOT_OK(PopulateResultSet(row, resultset));
      }
    }
  }

  if (request_.is_aggregate() && match_count > 0) {
    RETURN_NOT_OK(PopulateAggregate(row, resultset));
  }

  if (FLAGS_trace_docdb_calls) {
    TRACE("Fetched $0 rows.", resultset->rsrow_count());
  }
  *restart_read_ht = iter->RestartReadHt();

  if (resultset->rsrow_count() >= row_count_limit && !request_.is_aggregate()) {
    RETURN_NOT_OK(iter->SetPagingStateIfNecessary(request_, &response_));
  }

  return Status::OK();
}

Status PgsqlReadOperation::PopulateResultSet(const QLTableRow::SharedPtr& table_row,
                                             PgsqlResultSet *resultset) {
  PgsqlRSRow *rsrow = resultset->AllocateRSRow(request_.targets().size());
  int rscol_index = 0;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, rsrow->rscol(rscol_index)));
    rscol_index++;
  }
  return Status::OK();
}

Status PgsqlReadOperation::GetTupleId(QLValue *result) const {
  // Get row key and save to QLValue.
  // TODO(neil) Check if we need to append a table_id and other info to TupleID. For example, we
  // might need info to make sure the TupleId by itself is a valid reference to a specific row of
  // a valid table.
  result->set_binary_value(VERIFY_RESULT(table_iter_->GetRowKey()));
  return Status::OK();
}

Status PgsqlReadOperation::EvalAggregate(const QLTableRow::SharedPtr& table_row) {
  if (aggr_result_.empty()) {
    int column_count = request_.targets().size();
    aggr_result_.resize(column_count);
  }

  int aggr_index = 0;
  for (const PgsqlExpressionPB& expr : request_.targets()) {
    RETURN_NOT_OK(EvalExpr(expr, table_row, &aggr_result_[aggr_index]));
    aggr_index++;
  }
  return Status::OK();
}

Status PgsqlReadOperation::PopulateAggregate(const QLTableRow::SharedPtr& table_row,
                                             PgsqlResultSet *resultset) {
  int column_count = request_.targets().size();
  PgsqlRSRow *rsrow = resultset->AllocateRSRow(column_count);
  for (int rscol_index = 0; rscol_index < column_count; rscol_index++) {
    *rsrow->rscol(rscol_index) = aggr_result_[rscol_index];
  }
  return Status::OK();
}

Status PgsqlReadOperation::GetIntents(const Schema& schema, KeyValueWriteBatchPB* out) {
  auto pair = out->mutable_read_pairs()->Add();

  if (request_.partition_column_values().empty()) {
    // Empty components mean that we don't have primary key at all, but request
    // could still contain hash_code as part of tablet routing.
    // So we should ignore it.
    pair->set_key(std::string(1, ValueTypeAsChar::kGroupEnd));
  } else {
    std::vector<PrimitiveValue> hashed_components;
    RETURN_NOT_OK(InitKeyColumnPrimitiveValues(
        request_.partition_column_values(), schema, 0 /* start_idx */, &hashed_components));

    DocKey doc_key(request_.hash_code(), hashed_components);
    pair->set_key(doc_key.Encode().data());
  }

  pair->set_value(std::string(1, ValueTypeAsChar::kNull));
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb
