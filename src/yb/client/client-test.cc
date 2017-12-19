// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <glog/stl_logging.h>

#include "yb/client/callbacks.h"
#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/client-test-util.h"
#include "yb/client/meta_cache.h"
#include "yb/client/row_result.h"
#include "yb/client/scanner-internal.h"
#include "yb/client/table_handle.h"
#include "yb/client/value.h"
#include "yb/client/yb_op.h"
#include "yb/common/partial_row.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master-test-util.h"
#include "yb/master/master.proxy.h"
#include "yb/master/mini_master.h"
#include "yb/master/ts_descriptor.h"
#include "yb/rpc/messenger.h"
#include "yb/server/metadata.h"
#include "yb/server/hybrid_clock.h"
#include "yb/yql/cql/ql/util/statement_result.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/scanners.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/metrics.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"
#include "yb/util/stopwatch.h"
#include "yb/util/test_util.h"
#include "yb/util/thread.h"
#include "yb/util/tostring.h"

DECLARE_bool(enable_data_block_fsync);
DECLARE_bool(log_inject_latency);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_int32(log_inject_latency_ms_mean);
DECLARE_int32(log_inject_latency_ms_stddev);
DECLARE_int32(master_inject_latency_on_tablet_lookups_ms);
DECLARE_int32(max_create_tablets_per_ts);
DECLARE_int32(scanner_gc_check_interval_us);
DECLARE_int32(scanner_inject_latency_on_each_batch_ms);
DECLARE_int32(scanner_max_batch_size_bytes);
DECLARE_int32(scanner_ttl_ms);
DECLARE_int32(tablet_server_svc_queue_length);

DEFINE_int32(test_scan_num_rows, 1000, "Number of rows to insert and scan");

METRIC_DECLARE_counter(rpcs_queue_overflow);

using namespace std::literals; // NOLINT
using namespace std::placeholders;

namespace yb {
namespace client {

using std::string;
using std::set;
using std::vector;

using base::subtle::Atomic32;
using base::subtle::NoBarrier_AtomicIncrement;
using base::subtle::NoBarrier_Load;
using base::subtle::NoBarrier_Store;
using master::CatalogManager;
using master::GetTableLocationsRequestPB;
using master::GetTableLocationsResponsePB;
using master::TabletLocationsPB;
using master::TSInfoPB;
using std::shared_ptr;
using tablet::TabletPeer;
using tserver::MiniTabletServer;

constexpr int32_t kNoBound = kint32max;
constexpr int kNumTablets = 2;

class ClientTest: public YBMiniClusterTestBase<MiniCluster> {
 public:
  ClientTest() {
    YBSchemaBuilder b;
    b.AddColumn("key")->Type(INT32)->NotNull()->HashPrimaryKey();
    b.AddColumn("int_val")->Type(INT32)->NotNull();
    b.AddColumn("string_val")->Type(STRING)->Nullable();
    b.AddColumn("non_null_with_default")->Type(INT32)->NotNull()
        ->Default(YBValue::FromInt(12345));
    CHECK_OK(b.Build(&schema_));

    FLAGS_enable_data_block_fsync = false; // Keep unit tests fast.
  }

  void SetUp() override {
    YBMiniClusterTestBase::SetUp();

    // Reduce the TS<->Master heartbeat interval
    FLAGS_heartbeat_interval_ms = 10;
    FLAGS_scanner_gc_check_interval_us = 50 * 1000; // 50 milliseconds.

    // Start minicluster and wait for tablet servers to connect to master.
    cluster_.reset(new MiniCluster(env_.get(), MiniClusterOptions()));
    ASSERT_OK(cluster_->Start());

    // Connect to the cluster.
    ASSERT_OK(YBClientBuilder()
        .add_master_server_addr(yb::ToString(cluster_->mini_master()->bound_rpc_addr()))
        .Build(&client_));

    // Create a keyspace;
    ASSERT_OK(client_->CreateNamespace(kKeyspaceName));

    ASSERT_NO_FATALS(CreateTable(kTableName, 1, kNumTablets, &client_table_));
    ASSERT_NO_FATALS(CreateTable(kTable2Name, 1, 1, &client_table2_));
  }

  void DoTearDown() override {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }
    YBMiniClusterTestBase::DoTearDown();
  }

 protected:

  static const string kKeyspaceName;
  static const YBTableName kTableName;
  static const YBTableName kTable2Name;

  string GetFirstTabletId(YBTable* table) {
    GetTableLocationsRequestPB req;
    GetTableLocationsResponsePB resp;
    table->name().SetIntoTableIdentifierPB(req.mutable_table());
    CHECK_OK(cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(
        &req, &resp));
    CHECK_GT(resp.tablet_locations_size(), 0);
    return resp.tablet_locations(0).tablet_id();
  }

  void CheckNoRpcOverflow() {
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* server = cluster_->mini_tablet_server(i);
      if (server->is_started()) {
        ASSERT_EQ(0, server->server()->rpc_server()->
            service_pool("yb.tserver.TabletServerService")->
            RpcsQueueOverflowMetric()->value());
      }
    }
  }

  YBSessionPtr CreateSession(YBClient* client = nullptr) {
    if (client == nullptr) {
      client = client_.get();
    }
    std::shared_ptr<YBSession> session = client->NewSession();
    EXPECT_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
    session->SetTimeout(10s);
    return session;
  }

  // Inserts 'num_rows' test rows using 'client'
  void InsertTestRows(YBClient* client, const TableHandle& table, int num_rows, int first_row = 0) {
    auto session = CreateSession(client);
    for (int i = first_row; i < num_rows + first_row; i++) {
      ASSERT_OK(session->Apply(BuildTestRow(table, i)));
    }
    FlushSessionOrDie(session);
        ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  // Inserts 'num_rows' using the default client.
  void InsertTestRows(const TableHandle& table, int num_rows, int first_row = 0) {
    InsertTestRows(client_.get(), table, num_rows, first_row);
  }

  void UpdateTestRows(const TableHandle& table, int lo, int hi) {
    auto session = CreateSession();
    for (int i = lo; i < hi; i++) {
      ASSERT_OK(session->Apply(UpdateTestRow(table, i)));
    }
    FlushSessionOrDie(session);
        ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  void DeleteTestRows(const TableHandle& table, int lo, int hi) {
    auto session = CreateSession();
    for (int i = lo; i < hi; i++) {
      ASSERT_OK(session->Apply(DeleteTestRow(table, i)));
    }
    FlushSessionOrDie(session);
        ASSERT_NO_FATALS(CheckNoRpcOverflow());
  }

  shared_ptr<YBqlWriteOp> BuildTestRow(const TableHandle& table, int index) {
    auto insert = table.NewInsertOp();
    auto req = insert->mutable_request();
    QLAddInt32HashValue(req, index);
    const auto& columns = table.schema().columns();
    table.AddInt32ColumnValue(req, columns[1].name(), index * 2);
    table.AddStringColumnValue(req, columns[2].name(), StringPrintf("hello %d", index));
    table.AddInt32ColumnValue(req, columns[3].name(), index * 3);
    return insert;
  }

  shared_ptr<YBqlWriteOp> UpdateTestRow(const TableHandle& table, int index) {
    auto update = table.NewUpdateOp();
    auto req = update->mutable_request();
    QLAddInt32HashValue(req, index);
    const auto& columns = table.schema().columns();
    table.AddInt32ColumnValue(req, columns[1].name(), index * 2 + 1);
    table.AddStringColumnValue(req, columns[2].name(), StringPrintf("hello again %d", index));
    return update;
  }

  shared_ptr<YBqlWriteOp> DeleteTestRow(const TableHandle& table, int index) {
    auto del = table.NewDeleteOp();
    QLAddInt32HashValue(del->mutable_request(), index);
    return del;
  }

  void DoTestScanWithoutPredicates() {
    YBScanner scanner(client_table_.get());
    ASSERT_OK(scanner.SetProjectedColumns({"key"}));
    LOG_TIMING(INFO, "Scanning with no predicates") {
      ASSERT_OK(scanner.Open());

      ASSERT_TRUE(scanner.HasMoreRows());
      YBScanBatch batch;
      uint64_t sum = 0;
      while (scanner.HasMoreRows()) {
        ASSERT_OK(scanner.NextBatch(&batch));
        for (const YBScanBatch::RowPtr& row : batch) {
          int32_t value;
          ASSERT_OK(row.GetInt32(0, &value));
          sum += value;
        }
      }
      // The sum should be the sum of the arithmetic series from
      // 0..FLAGS_test_scan_num_rows-1
      uint64_t expected = implicit_cast<uint64_t>(FLAGS_test_scan_num_rows) *
          (0 + (FLAGS_test_scan_num_rows - 1)) / 2;
      ASSERT_EQ(expected, sum);
    }
  }

  void DoTestScanWithStringPredicate() {
    YBScanner scanner(client_table_.get());
    ASSERT_OK(scanner.AddConjunctPredicate(
        client_table_.table()->NewComparisonPredicate("string_val", YBPredicate::GREATER_EQUAL,
            YBValue::CopyString("hello 2"))));
    ASSERT_OK(scanner.AddConjunctPredicate(
        client_table_.table()->NewComparisonPredicate("string_val", YBPredicate::LESS_EQUAL,
            YBValue::CopyString("hello 3"))));

    LOG_TIMING(INFO, "Scanning with string predicate") {
      ASSERT_OK(scanner.Open());

      ASSERT_TRUE(scanner.HasMoreRows());
      YBScanBatch batch;
      while (scanner.HasMoreRows()) {
        ASSERT_OK(scanner.NextBatch(&batch));
        for (const YBScanBatch::RowPtr& row : batch) {
          Slice s;
          ASSERT_OK(row.GetString(2, &s));
          if (!s.starts_with("hello 2") && !s.starts_with("hello 3")) {
            FAIL() << row.ToString();
          }
        }
      }
    }
  }

  void DoTestScanWithKeyPredicate() {
    auto op = client_table_.NewReadOp();
    auto req = op->mutable_request();

    auto* const condition = req->mutable_where_expr()->mutable_condition();
    condition->set_op(QL_OP_AND);
    client_table_.AddInt32Condition(condition, "key", QL_OP_GREATER_THAN_EQUAL, 5);
    client_table_.AddInt32Condition(condition, "key", QL_OP_LESS_THAN_EQUAL, 10);
    client_table_.AddColumns({"key"}, req);
    auto session = client_->NewSession();
    session->SetTimeout(60s);
    ASSERT_OK(session->Apply(op));
    ASSERT_OK(session->Flush());
    ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    for (const auto& row : rowblock->rows()) {
      int32_t key = row.column(0).int32_value();
      ASSERT_GE(key, 5);
      ASSERT_LE(key, 10);
    }
  }

  // Creates a table with 'num_replicas', split into tablets based on 'split_rows'
  // (or single tablet if 'split_rows' is empty).
  void CreateTable(const YBTableName& table_name_orig,
                   int num_replicas,
                   int num_tablets,
                   TableHandle* table) {
    // The implementation allows table name without a keyspace.
    YBTableName table_name(table_name_orig.has_namespace() ?
        table_name_orig.namespace_name() : kKeyspaceName, table_name_orig.table_name());

    bool added_replicas = false;
    // Add more tablet servers to satisfy all replicas, if necessary.
    while (cluster_->num_tablet_servers() < num_replicas) {
      ASSERT_OK(cluster_->AddTabletServer());
      added_replicas = true;
    }

    if (added_replicas) {
      ASSERT_OK(cluster_->WaitForTabletServerCount(num_replicas));
    }

    ASSERT_OK(table->Create(table_name, num_tablets, schema_, client_.get(), num_replicas));
  }

  // Kills a tablet server.
  // Boolean flags control whether to restart the tserver, and if so, whether to wait for it to
  // finish bootstrapping.
  Status KillTServerImpl(const string& uuid, const bool restart, const bool wait_started) {
    bool ts_found = false;
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      if (ts->server()->instance_pb().permanent_uuid() == uuid) {
        if (restart) {
          LOG(INFO) << "Restarting TS at " << ts->bound_rpc_addr();
              RETURN_NOT_OK(ts->Restart());
          if (wait_started) {
            LOG(INFO) << "Waiting for TS " << ts->bound_rpc_addr() << " to finish bootstrapping";
                RETURN_NOT_OK(ts->WaitStarted());
          }
        } else {
          LOG(INFO) << "Killing TS " << uuid << " at " << ts->bound_rpc_addr();
          ts->Shutdown();
        }
        ts_found = true;
        break;
      }
    }
    if (!ts_found) {
      return STATUS(InvalidArgument, strings::Substitute("Could not find tablet server $1", uuid));
    }

    return Status::OK();
  }

  Status RestartTServerAndWait(const string& uuid) {
    return KillTServerImpl(uuid, true, true);
  }

  Status RestartTServerAsync(const string& uuid) {
    return KillTServerImpl(uuid, true, false);
  }

  Status KillTServer(const string& uuid) {
    return KillTServerImpl(uuid, false, false);
  }

  void DoApplyWithoutFlushTest(int sleep_micros);

  enum WhichServerToKill {
    DEAD_MASTER,
    DEAD_TSERVER
  };
  void DoTestWriteWithDeadServer(WhichServerToKill which);

  YBSchema schema_;

  gscoped_ptr<MiniCluster> cluster_;
  shared_ptr<YBClient> client_;
  TableHandle client_table_;
  TableHandle client_table2_;
};


const string ClientTest::kKeyspaceName("my_keyspace");
const YBTableName ClientTest::kTableName(kKeyspaceName, "client-testtb");
const YBTableName ClientTest::kTable2Name(kKeyspaceName, "client-testtb2");

namespace {

TableFilter MakeFilter(int32_t lower_bound, int32_t upper_bound, std::string column = "key") {
  if (lower_bound != kNoBound) {
    if (upper_bound != kNoBound) {
      return FilterBetween(lower_bound, Inclusive::kTrue, upper_bound, Inclusive::kTrue,
                           std::move(column));
    } else {
      return FilterGreater(lower_bound, Inclusive::kTrue, std::move(column));
    }
  }
  if (upper_bound != kNoBound) {
    return FilterLess(upper_bound, Inclusive::kTrue, std::move(column));
  }
  return TableFilter();
}

int CountRowsFromClient(const TableHandle& table, YBConsistencyLevel consistency,
                        int32_t lower_bound, int32_t upper_bound) {
  TableIteratorOptions options;
  options.consistency = consistency;
  options.columns = {"key"};
  options.filter = MakeFilter(lower_bound, upper_bound);
  return boost::size(TableRange(table, options));
}

int CountRowsFromClient(const TableHandle& table, int32_t lower_bound, int32_t upper_bound) {
  return CountRowsFromClient(table, YBConsistencyLevel::STRONG, lower_bound, upper_bound);
}

int CountRowsFromClient(const TableHandle& table) {
  return CountRowsFromClient(table, kNoBound, kNoBound);
}

// Count the rows of a table, checking that the operation succeeds.
//
// Must be public to use as a thread closure.
void CheckRowCount(const TableHandle& table) {
  CountRowsFromClient(table);
}

} // namespace

TEST_F(ClientTest, TestListTables) {
  vector<YBTableName> tables;
  ASSERT_OK(client_->ListTables(&tables));
  std::sort(tables.begin(), tables.end(), [](const YBTableName& n1, const YBTableName& n2) {
    return n1.ToString() < n2.ToString();
  });
  ASSERT_EQ(2 + cluster_->leader_mini_master()->NumSystemTables(), tables.size());
  ASSERT_EQ(kTableName, tables[0]) << "Tables:" << ToString(tables);
  ASSERT_EQ(kTable2Name, tables[1]) << "Tables:" << ToString(tables);
  tables.clear();
  ASSERT_OK(client_->ListTables(&tables, "testtb2"));
  ASSERT_EQ(1, tables.size());
  ASSERT_EQ(kTable2Name, tables[0]) << "Tables:" << ToString(tables);
}

TEST_F(ClientTest, TestListTabletServers) {
  std::vector<std::unique_ptr<YBTabletServer>> tss;
  ASSERT_OK(client_->ListTabletServers(&tss));
  ASSERT_EQ(1, tss.size());
  ASSERT_EQ(cluster_->mini_tablet_server(0)->server()->instance_pb().permanent_uuid(),
            tss[0]->uuid());
  ASSERT_EQ(cluster_->mini_tablet_server(0)->server()->first_rpc_address().address().to_string(),
            tss[0]->hostname());
}

TEST_F(ClientTest, TestBadTable) {
  shared_ptr<YBTable> t;
  Status s = client_->OpenTable(YBTableName(kKeyspaceName, "xxx-does-not-exist"), &t);
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(false), "Not found: The table does not exist");
}

// Test that, if the master is down, we experience a network error talking
// to it (no "find the new leader master" since there's only one master).
TEST_F(ClientTest, TestMasterDown) {
  DontVerifyClusterBeforeNextTearDown();
  cluster_->mini_master()->Shutdown();
  shared_ptr<YBTable> t;
  client_->data_->default_admin_operation_timeout_ = MonoDelta::FromSeconds(1);
  Status s = client_->OpenTable(YBTableName(kKeyspaceName, "other-tablet"), &t);
  ASSERT_TRUE(s.IsNetworkError());
}

// TODO scan with predicates is not supported.
TEST_F(ClientTest, TestScan) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  ASSERT_EQ(FLAGS_test_scan_num_rows, CountRowsFromClient(client_table_));

  // Scan after insert
  DoTestScanWithoutPredicates();
  DoTestScanWithStringPredicate();
  DoTestScanWithKeyPredicate();

  // Scan after update
  UpdateTestRows(client_table_, 0, FLAGS_test_scan_num_rows);
  DoTestScanWithKeyPredicate();

  // Scan after delete half
  DeleteTestRows(client_table_, 0, FLAGS_test_scan_num_rows / 2);
  DoTestScanWithKeyPredicate();

  // Scan after delete all
  DeleteTestRows(client_table_, FLAGS_test_scan_num_rows / 2 + 1, FLAGS_test_scan_num_rows);
  DoTestScanWithKeyPredicate();

  // Scan after re-insert
  InsertTestRows(client_table_, 1);
  DoTestScanWithKeyPredicate();
}

TEST_F(ClientTest, TestScanAtSnapshot) {
  int half_the_rows = FLAGS_test_scan_num_rows / 2;

  // Insert half the rows
  ASSERT_NO_FATALS(InsertTestRows(client_table_, half_the_rows));

  // get the time from the server and transform to micros disregarding any
  // logical values (we shouldn't have any with a single server anyway);
  int64_t ts = server::HybridClock::GetPhysicalValueMicros(
      cluster_->mini_tablet_server(0)->server()->clock()->Now());

  // Insert the second half of the rows
  ASSERT_NO_FATALS(InsertTestRows(client_table_, half_the_rows, half_the_rows));

  YBScanner scanner(client_table_.get());
  ASSERT_OK(scanner.Open());
  uint64_t count = 0;

  // Do a "normal", READ_LATEST scan
  YBScanBatch batch;
  while (scanner.HasMoreRows()) {
    ASSERT_OK(scanner.NextBatch(&batch));
    count += batch.NumRows();
  }

  ASSERT_EQ(FLAGS_test_scan_num_rows, count);

  // Now close the scanner and perform a scan at 'ts'
  scanner.Close();
  ASSERT_OK(scanner.SetReadMode(YBScanner::READ_AT_SNAPSHOT));
  ASSERT_OK(scanner.SetSnapshotMicros(ts));
  ASSERT_OK(scanner.Open());

  count = 0;
  while (scanner.HasMoreRows()) {
    ASSERT_OK(scanner.NextBatch(&batch));
    count += batch.NumRows();
  }

  ASSERT_EQ(half_the_rows, count);
}

// Test scanning at a hybrid_time in the future compared to the
// local clock. If we are within the clock error, this should wait.
// If we are far in the future, we should get an error.
TEST_F(ClientTest, TestScanAtFutureHybridTime) {
  YBScanner scanner(client_table_.get());
  ASSERT_OK(scanner.SetReadMode(YBScanner::READ_AT_SNAPSHOT));

  // Try to perform a scan at NowLatest(). This is in the future,
  // but the server should wait until it's in the past.
  int64_t ts = server::HybridClock::GetPhysicalValueMicros(
      cluster_->mini_tablet_server(0)->server()->clock()->NowLatest());
  ASSERT_OK(scanner.SetSnapshotMicros(ts));
  ASSERT_OK(scanner.Open());
  scanner.Close();

  // Try to perform a scan far in the future (60s -- higher than max clock error).
  // This should return an error.
  ts += 60 * 1000000;
  ASSERT_OK(scanner.SetSnapshotMicros(ts));
  Status s = scanner.Open();
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  ASSERT_STR_CONTAINS(s.ToString(), "in the future.");
}

void CheckCounts(const TableHandle& table, const std::vector<int>& expected) {
  std::vector<std::pair<int, int>> bounds = {
    { kNoBound, kNoBound },
    { kNoBound, 15 },
    { 27, kNoBound },
    { 0, 15 },
    { 0, 10 },
    { 0, 20 },
    { 0, 30 },
    { 14, 30 },
    { 30, 30 },
    { 50, kNoBound },
  };
  ASSERT_EQ(bounds.size(), expected.size());
  for (size_t i = 0; i != bounds.size(); ++i) {
    ASSERT_EQ(expected[i], CountRowsFromClient(table, bounds[i].first, bounds[i].second));
  }
  // Run through various scans.
}

TEST_F(ClientTest, TestScanMultiTablet) {
  // 5 tablets, each with 10 rows worth of space.
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName("TestScanMultiTablet"), 1, 5, &table));

  // Insert rows with keys 12, 13, 15, 17, 22, 23, 25, 27...47 into each
  // tablet, except the first which is empty.
    auto session = CreateSession();
  for (int i = 1; i < 5; i++) {
    ASSERT_OK(session->Apply(BuildTestRow(table, 2 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 3 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 5 + (i * 10))));
    ASSERT_OK(session->Apply(BuildTestRow(table, 7 + (i * 10))));
  }
  FlushSessionOrDie(session);

  // Run through various scans.
  CheckCounts(table, { 16, 3, 9, 3, 0, 4, 8, 6, 0, 0 });

  // Update every other row
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(UpdateTestRow(table, 2 + i * 10)));
    ASSERT_OK(session->Apply(UpdateTestRow(table, 5 + i * 10)));
  }
  FlushSessionOrDie(session);

  // Check all counts the same (make sure updates don't change # of rows)
  CheckCounts(table, { 16, 3, 9, 3, 0, 4, 8, 6, 0, 0 });

  // Delete half the rows
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(DeleteTestRow(table, 5 + i*10)));
    ASSERT_OK(session->Apply(DeleteTestRow(table, 7 + i*10)));
  }
  FlushSessionOrDie(session);

  // Check counts changed accordingly
  CheckCounts(table, { 8, 2, 4, 2, 0, 2, 4, 2, 0, 0 });

  // Delete rest of rows
  for (int i = 1; i < 5; ++i) {
    ASSERT_OK(session->Apply(DeleteTestRow(table, 2 + i*10)));
    ASSERT_OK(session->Apply(DeleteTestRow(table, 3 + i*10)));
  }
  FlushSessionOrDie(session);

  // Check counts changed accordingly
  CheckCounts(table, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
}

TEST_F(ClientTest, TestScanEmptyTable) {
  YBScanner scanner(client_table_.get());
  ASSERT_OK(scanner.SetProjectedColumns(vector<string>()));
  ASSERT_OK(scanner.Open());

  // There are two tablets in the table, both empty. Until we scan to
  // the last tablet, HasMoreRows will return true (because it doesn't
  // know whether there's data in subsequent tablets).
  ASSERT_TRUE(scanner.HasMoreRows());
  YBScanBatch batch;
  ASSERT_OK(scanner.NextBatch(&batch));
  ASSERT_EQ(0, batch.NumRows());
  ASSERT_FALSE(scanner.HasMoreRows());
}

// Test scanning with an empty projection. This should yield an empty
// row block with the proper number of rows filled in. Impala issues
// scans like this in order to implement COUNT(*).
TEST_F(ClientTest, TestScanEmptyProjection) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));
  YBScanner scanner(client_table_.get());
  ASSERT_OK(scanner.SetProjectedColumns(vector<string>()));
  ASSERT_EQ(scanner.GetProjectionSchema().num_columns(), 0);
  LOG_TIMING(INFO, "Scanning with no projected columns") {
    ASSERT_OK(scanner.Open());

    ASSERT_TRUE(scanner.HasMoreRows());
    YBScanBatch batch;
    uint64_t count = 0;
    while (scanner.HasMoreRows()) {
      ASSERT_OK(scanner.NextBatch(&batch));
      count += batch.NumRows();
    }
    ASSERT_EQ(FLAGS_test_scan_num_rows, count);
  }
}

TEST_F(ClientTest, TestProjectInvalidColumn) {
  YBScanner scanner(client_table_.get());
  Status s = scanner.SetProjectedColumns({ "column-doesnt-exist" });
  ASSERT_EQ("Not found: Column: \"column-doesnt-exist\" was not found in the table schema.",
            s.ToString(/* no file/line */ false));

  // Test trying to use a projection where a column is used multiple times.
  // TODO: consider fixing this to support returning the column multiple
  // times, even though it's not very useful.
  s = scanner.SetProjectedColumns({ "key", "key" });
  ASSERT_EQ("Invalid argument: Duplicate column name: key", s.ToString(/* no file/line */ false));
}

// Test a scan where we have a predicate on a key column that is not
// in the projection.
TEST_F(ClientTest, TestScanPredicateKeyColNotProjected) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  size_t nrows = 0;
  TableIteratorOptions options;
  options.columns = {"key", "int_val"};
  options.filter = MakeFilter(5, 10);
  for (const auto& row : TableRange(client_table_, options)) {
    int32_t key = row.column(0).int32_value();
    int32_t val = row.column(1).int32_value();
    ASSERT_EQ(key * 2, val);

    ++nrows;
  }

  ASSERT_EQ(6, nrows);
}

// Test a scan where we have a predicate on a non-key column that is
// not in the projection.
TEST_F(ClientTest, TestScanPredicateNonKeyColNotProjected) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  size_t nrows = 0;
  TableIteratorOptions options;
  options.columns = {"key", "int_val"};
  options.filter = MakeFilter(10, 20, "int_val");
  TableRange range(client_table_, options);
  for (const auto& row : range) {
    int32_t key = row.column(0).int32_value();
    int32_t val = row.column(1).int32_value();
    ASSERT_EQ(key * 2, val);

    ++nrows;
  }

  ASSERT_EQ(nrows, 6);
}

// Test adding various sorts of invalid binary predicates.
TEST_F(ClientTest, TestInvalidPredicates) {
  YBScanner scanner(client_table_.get());

  // Predicate on a column that does not exist.
  Status s = scanner.AddConjunctPredicate(
      client_table_->NewComparisonPredicate("this-does-not-exist",
                                            YBPredicate::EQUAL, YBValue::FromInt(5)));
  EXPECT_EQ("Not found: column not found: this-does-not-exist",
            s.ToString(/* no file/line */ false));

  // Int predicate on a string column.
  s = scanner.AddConjunctPredicate(
      client_table_->NewComparisonPredicate("string_val",
                                            YBPredicate::EQUAL, YBValue::FromInt(5)));
  EXPECT_EQ("Invalid argument: non-string value for string column string_val",
            s.ToString(false));

  // String predicate on an int column.
  s = scanner.AddConjunctPredicate(
      client_table_->NewComparisonPredicate("int_val",
                                            YBPredicate::EQUAL, YBValue::CopyString("x")));
  EXPECT_EQ("Invalid argument: non-int value for int column int_val",
            s.ToString(false));

  // Out-of-range int predicate on an int column.
  s = scanner.AddConjunctPredicate(
      client_table_->NewComparisonPredicate(
          "int_val",
          YBPredicate::EQUAL,
          YBValue::FromInt(static_cast<int64_t>(MathLimits<int32_t>::kMax) + 10)));
  EXPECT_EQ("Invalid argument: value 2147483657 out of range for "
            "32-bit signed integer column 'int_val'", s.ToString(false));
}


// Check that the tserver proxy is reset on close, even for empty tables.
TEST_F(ClientTest, TestScanCloseProxy) {
  const YBTableName kEmptyTable("TestScanCloseProxy");
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kEmptyTable, 3, kNumTablets, &table));

  {
    // Open and close an empty scanner.
    YBScanner scanner(table.get());
    ASSERT_OK(scanner.Open());
    scanner.Close();
    CHECK_EQ(0, scanner.data_->proxy_.use_count()) << "Proxy was not reset!";
  }

  // Insert some test rows.
  ASSERT_NO_FATALS(InsertTestRows(table, FLAGS_test_scan_num_rows));
  {
    // Open and close a scanner with rows.
    YBScanner scanner(table.get());
    ASSERT_OK(scanner.Open());
    scanner.Close();
    CHECK_EQ(0, scanner.data_->proxy_.use_count()) << "Proxy was not reset!";
  }
}

namespace internal {

static void ReadBatchToStrings(YBScanner* scanner, vector<string>* rows) {
  YBScanBatch batch;
  ASSERT_OK(scanner->NextBatch(&batch));
  for (int i = 0; i < batch.NumRows(); i++) {
    rows->push_back(batch.Row(i).ToString());
  }
}

static void DoScanWithCallback(
    YBTable* table, const vector<string>& expected_rows,
    const std::function<Status(const string&)>& cb) {
  // Initialize fault-tolerant snapshot scanner.
  YBScanner scanner(table);
  ASSERT_OK(scanner.SetFaultTolerant());
  // Set a small batch size so it reads in multiple batches.
  ASSERT_OK(scanner.SetBatchSizeBytes(1));

  ASSERT_OK(scanner.Open());
  vector<string> rows;

  // Do a first scan to get us started.
  {
    LOG(INFO) << "Setting up scanner.";
    ASSERT_TRUE(scanner.HasMoreRows());
    ASSERT_NO_FATALS(ReadBatchToStrings(&scanner, &rows));
    ASSERT_GT(rows.size(), 0);
    ASSERT_TRUE(scanner.HasMoreRows());
  }

  // Call the callback on the tserver serving the scan.
  LOG(INFO) << "Calling callback.";
  {
    YBTabletServer* kts_ptr;
    ASSERT_OK(scanner.GetCurrentServer(&kts_ptr));
    gscoped_ptr<YBTabletServer> kts(kts_ptr);
    ASSERT_OK(cb(kts->uuid()));
  }

  // Check that we can still read the next batch.
  LOG(INFO) << "Checking that we can still read the next batch.";
  ASSERT_TRUE(scanner.HasMoreRows());
  ASSERT_OK(scanner.SetBatchSizeBytes(1024*1024));
  while (scanner.HasMoreRows()) {
    ASSERT_NO_FATALS(ReadBatchToStrings(&scanner, &rows));
  }
  scanner.Close();

  // Verify results from the scan.
  LOG(INFO) << "Verifying results from scan.";
  std::sort(rows.begin(), rows.end());

  auto bad = 0;
  for (int i = 0; i < rows.size(); i++) {
    if (expected_rows[i] != rows[i]) {
      if (++bad <= 10) {
        EXPECT_EQ(expected_rows[i], rows[i]);
      }
    }
  }
  ASSERT_EQ(expected_rows.size(), rows.size());
}

}  // namespace internal

// Test that ordered snapshot scans can be resumed in the case of different tablet server failures.
TEST_F(ClientTest, TestScanFaultTolerance) {
  // Create test table and insert test rows.
  const YBTableName kScanTable("TestScanFaultTolerance");
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kScanTable, 3, 1, &table));
  ASSERT_NO_FATALS(InsertTestRows(table, FLAGS_test_scan_num_rows));

  // Do an initial scan to determine the expected rows for later verification.
  vector<string> expected_rows = ScanTableToStrings(table.get());
  std::sort(expected_rows.begin(), expected_rows.end());

  for (int with_flush = 0; with_flush <= 1; with_flush++) {
    SCOPED_TRACE((with_flush == 1) ? "with flush" : "without flush");
    // The second time through, flush to ensure that we test both against MRS and
    // disk.
    if (with_flush) {
      string tablet_id = GetFirstTabletId(table.get());
      for (int i = 0; i < 3; i++) {
        scoped_refptr<TabletPeer> tablet_peer;
        ASSERT_TRUE(cluster_->mini_tablet_server(i)->server()->tablet_manager()->LookupTablet(
                tablet_id, &tablet_peer));
        ASSERT_OK(tablet_peer->tablet()->Flush(tablet::FlushMode::kSync));
      }
    }

    // Test a few different recoverable server-side error conditions.
    // Since these are recoverable, the scan will succeed when retried elsewhere.

    // Restarting and waiting should result in a SCANNER_EXPIRED error.
    LOG(INFO) << "Doing a scan while restarting a tserver and waiting for it to come up...";
    ASSERT_NO_FATALS(internal::DoScanWithCallback(
        table.get(), expected_rows,
        std::bind(&ClientTest_TestScanFaultTolerance_Test::RestartTServerAndWait, this, _1)));

    // Restarting and not waiting means the tserver is hopefully bootstrapping, leading to
    // a TABLET_NOT_RUNNING error.
    LOG(INFO) << "Doing a scan while restarting a tserver...";
    ASSERT_NO_FATALS(internal::DoScanWithCallback(
        table.get(), expected_rows,
        std::bind(&ClientTest_TestScanFaultTolerance_Test::RestartTServerAsync, this, _1)));
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      ASSERT_OK(ts->WaitStarted());
    }

    // Killing the tserver should lead to an RPC timeout.
    LOG(INFO) << "Doing a scan while killing a tserver...";
    ASSERT_NO_FATALS(internal::DoScanWithCallback(
        table.get(), expected_rows,
        std::bind(&ClientTest_TestScanFaultTolerance_Test::KillTServer, this, _1)));

    // Restart the server that we killed.
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      if (!ts->is_started()) {
        ASSERT_OK(ts->Start());
        ASSERT_OK(ts->WaitStarted());
      }
    }
  }
}

TEST_F(ClientTest, TestGetTabletServerBlacklist) {
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName("blacklist"), 3, kNumTablets, &table));
  InsertTestRows(table, 1, 0);

  // Look up the tablet and its replicas into the metadata cache.
  // We have to loop since some replicas may have been created slowly.
  scoped_refptr<internal::RemoteTablet> rt;
  while (true) {
    Synchronizer sync;
    client_->data_->meta_cache_->LookupTabletByKey(table.get(), "", MonoTime::Max(), &rt,
                                                  sync.AsStatusCallback());
    ASSERT_OK(sync.Wait());
    ASSERT_TRUE(rt.get() != nullptr);
    vector<internal::RemoteTabletServer*> tservers;
    rt->GetRemoteTabletServers(&tservers);
    if (tservers.size() == 3) {
      break;
    }
    rt->MarkStale();
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  // Get the Leader.
  internal::RemoteTabletServer *rts;
  set<string> blacklist;
  vector<internal::RemoteTabletServer*> candidates;
  vector<internal::RemoteTabletServer*> tservers;
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::LEADER_ONLY,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  // Blacklist the leader, should not work.
  blacklist.insert(rts->permanent_uuid());
  {
    Status s = client_->data_->GetTabletServer(client_.get(), rt,
                                               YBClient::LEADER_ONLY,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }
  // Keep blacklisting replicas until we run out.
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::CLOSEST_REPLICA,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  blacklist.insert(rts->permanent_uuid());
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(), rt,
                                            YBClient::FIRST_REPLICA,
                                            blacklist, &candidates, &rts));
  tservers.push_back(rts);
  blacklist.insert(rts->permanent_uuid());

  // Make sure none of the three modes work when all nodes are blacklisted.
  vector<YBClient::ReplicaSelection> selections;
  selections.push_back(YBClient::LEADER_ONLY);
  selections.push_back(YBClient::CLOSEST_REPLICA);
  selections.push_back(YBClient::FIRST_REPLICA);
  for (YBClient::ReplicaSelection selection : selections) {
    Status s = client_->data_->GetTabletServer(client_.get(), rt, selection,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }

  // Make sure none of the modes work when all nodes are dead.
  for (internal::RemoteTabletServer* rt : tservers) {
    client_->data_->meta_cache_->MarkTSFailed(rt, STATUS(NetworkError, "test"));
  }
  blacklist.clear();
  for (YBClient::ReplicaSelection selection : selections) {
    Status s = client_->data_->GetTabletServer(client_.get(), rt,
                                               selection,
                                               blacklist, &candidates, &rts);
    ASSERT_TRUE(s.IsServiceUnavailable());
  }
}

TEST_F(ClientTest, TestScanWithEncodedRangePredicate) {
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(YBTableName("split-table"),
                               1, /* replicas */
                               kNumTablets,
                               &table));

  ASSERT_NO_FATALS(InsertTestRows(table, 100));

  TableRange all_range(table, {});
  auto all_rows = ScanToStrings(all_range);
  ASSERT_EQ(100, all_rows.size());

  // Test a double-sided range within first tablet
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(5, Inclusive::kTrue, 8, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(8 - 5, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[7], rows.back());
  }

  // Test a double-sided range spanning tablets
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(5, Inclusive::kTrue, 15, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(15 - 5, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[14], rows.back());
  }

  // Test a double-sided range within second tablet
  {
    TableIteratorOptions options;
    options.filter = FilterBetween(15, Inclusive::kTrue, 20, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(20 - 15, rows.size());
    EXPECT_EQ(all_rows[15], rows.front());
    EXPECT_EQ(all_rows[19], rows.back());
  }

  // Test a lower-bound only range.
  {
    TableIteratorOptions options;
    options.filter = FilterGreater(5, Inclusive::kTrue);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(95, rows.size());
    EXPECT_EQ(all_rows[5], rows.front());
    EXPECT_EQ(all_rows[99], rows.back());
  }

  // Test an upper-bound only range in first tablet.
  {
    TableIteratorOptions options;
    options.filter = FilterLess(5, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(5, rows.size());
    EXPECT_EQ(all_rows[0], rows.front());
    EXPECT_EQ(all_rows[4], rows.back());
  }

  // Test an upper-bound only range in second tablet.
  {
    TableIteratorOptions options;
    options.filter = FilterLess(15, Inclusive::kFalse);
    auto rows = ScanToStrings(TableRange(table, options));
    ASSERT_EQ(15, rows.size());
    EXPECT_EQ(all_rows[0], rows.front());
    EXPECT_EQ(all_rows[14], rows.back());
  }

}

static void AssertScannersDisappear(const tserver::ScannerManager* manager) {
  // The Close call is async, so we may have to loop a bit until we see it disappear.
  // This loops for ~10sec. Typically it succeeds in only a few milliseconds.
  int i = 0;
  for (i = 0; i < 500; i++) {
    if (manager->CountActiveScanners() == 0) {
      LOG(INFO) << "Successfully saw scanner close on iteration " << i;
      return;
    }
    // Sleep 2ms on first few times through, then longer on later iterations.
    SleepFor(MonoDelta::FromMilliseconds(i < 10 ? 2 : 20));
  }
  FAIL() << "Waited too long for the scanner to close";
}

namespace {

int64_t SumResults(const YBScanBatch& batch) {
  int64_t sum = 0;
  for (const YBScanBatch::RowPtr& row : batch) {
    int32_t val;
    CHECK_OK(row.GetInt32(0, &val));
    sum += val;
  }
  return sum;
}

} // anonymous namespace

TEST_F(ClientTest, TestScannerKeepAlive) {
  google::FlagSaver saver;

#ifndef __APPLE__
  std::chrono::milliseconds kScannerTtl = 100ms;
#else
  // MAC OS X could just don't notify waiting socket that there is some data recieved for 100+ ms.
  std::chrono::milliseconds kScannerTtl = 400ms;
#endif

  ASSERT_NO_FATALS(InsertTestRows(client_table_, 1000));
  // Set the scanner ttl really low
  ANNOTATE_BENIGN_RACE(&FLAGS_scanner_ttl_ms, "Set at runtime, for tests.");
  FLAGS_scanner_ttl_ms = kScannerTtl.count();

  // Start a scan but don't get the whole data back
  YBScanner scanner(client_table_.get());
  // This will make sure we have to do multiple NextBatch calls to the second tablet.
  ASSERT_OK(scanner.SetBatchSizeBytes(100));
  ASSERT_OK(scanner.Open());

  YBScanBatch batch;
  int64_t sum = 0;

  ASSERT_TRUE(scanner.HasMoreRows());
  ASSERT_OK(scanner.NextBatch(&batch));

  // We should get only 3 rows back (from the first tablet).
  ASSERT_EQ(batch.NumRows(), 3);
  sum += SumResults(batch);

  ASSERT_TRUE(scanner.HasMoreRows());

  // We're in between tablets but even if there isn't a live scanner the client should
  // still return OK to the keep alive call.
  ASSERT_OK(scanner.KeepAlive());

  // Start scanning the second tablet, but break as soon as we have some data so that
  // we have a live remote scanner on the second tablet.
  while (scanner.HasMoreRows()) {
    ASSERT_OK(scanner.NextBatch(&batch));
    if (batch.NumRows() > 0) break;
  }
  sum += SumResults(batch);
  ASSERT_TRUE(scanner.HasMoreRows());

  // Now loop while keeping the scanner alive. Each time we loop we sleep 1/2 a scanner
  // ttl interval (the garbage collector is running each 50 msecs too.).
  for (int i = 0; i < 5; i++) {
    std::this_thread::sleep_for(kScannerTtl / 2);
    ASSERT_OK(scanner.KeepAlive());
  }

  // Get a second batch before sleeping/keeping alive some more. This is test for a bug
  // where we would only actually perform a KeepAlive() rpc after the first request and
  // not on subsequent ones.
  while (scanner.HasMoreRows()) {
    ASSERT_OK(scanner.NextBatch(&batch));
    if (batch.NumRows() > 0) break;
  }

  ASSERT_TRUE(scanner.HasMoreRows());
  for (int i = 0; i < 5; i++) {
    std::this_thread::sleep_for(kScannerTtl / 2);
    ASSERT_OK(scanner.KeepAlive());
  }
  sum += SumResults(batch);

  // Loop to get the remaining rows.
  while (scanner.HasMoreRows()) {
    ASSERT_OK(scanner.NextBatch(&batch));
    sum += SumResults(batch);
  }
  ASSERT_FALSE(scanner.HasMoreRows());
  ASSERT_EQ(sum, 499500);
}

// Test cleanup of scanners on the server side when closed.
TEST_F(ClientTest, TestCloseScanner) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));

  const tserver::ScannerManager* manager =
    cluster_->mini_tablet_server(0)->server()->scanner_manager();
  // Open the scanner, make sure it gets closed right away
  {
    SCOPED_TRACE("Implicit close");
    YBScanner scanner(client_table_.get());
    ASSERT_OK(scanner.Open());
    ASSERT_EQ(0, manager->CountActiveScanners());
    scanner.Close();
    AssertScannersDisappear(manager);
  }

  // Open the scanner, make sure we see 1 registered scanner.
  {
    SCOPED_TRACE("Explicit close");
    YBScanner scanner(client_table_.get());
    ASSERT_OK(scanner.SetBatchSizeBytes(0)); // won't return data on open
    ASSERT_OK(scanner.Open());
    ASSERT_EQ(1, manager->CountActiveScanners());
    scanner.Close();
    AssertScannersDisappear(manager);
  }

  {
    SCOPED_TRACE("Close when out of scope");
    {
      YBScanner scanner(client_table_.get());
      ASSERT_OK(scanner.SetBatchSizeBytes(0));
      ASSERT_OK(scanner.Open());
      ASSERT_EQ(1, manager->CountActiveScanners());
    }
    // Above scanner went out of scope, so the destructor should close asynchronously.
    AssertScannersDisappear(manager);
  }
}

TEST_F(ClientTest, TestScanTimeout) {
  // If we set the RPC timeout to be 0, we'll time out in the GetTableLocations
  // code path and not even discover where the tablet is hosted.
  {
    client_->data_->default_rpc_timeout_ = MonoDelta::FromSeconds(0);
    YBScanner scanner(client_table_.get());
    Status s = scanner.Open();
    EXPECT_TRUE(s.IsTimedOut()) << s.ToString();
    EXPECT_FALSE(scanner.data_->remote_) << "should not have located any tablet";
    client_->data_->default_rpc_timeout_ = MonoDelta::FromSeconds(5);
  }

  // Warm the cache so that the subsequent timeout occurs within the scan,
  // not the lookup.
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));

  // The "overall operation" timed out; no replicas failed.
  {
    YBScanner scanner(client_table_.get());
    ASSERT_OK(scanner.SetTimeoutMillis(0));
    ASSERT_TRUE(scanner.Open().IsTimedOut());
    YB_ASSERT_TRUE(scanner.data_->remote_) << "We should have located a tablet";;
    ASSERT_EQ(0, scanner.data_->remote_->GetNumFailedReplicas());
  }

  // Insert some more rows so that the scan takes multiple batches, instead of
  // fetching all the data on the 'Open()' call.
  client_->data_->default_rpc_timeout_ = MonoDelta::FromSeconds(5);
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 1000, 1));
  {
    google::FlagSaver saver;
    FLAGS_scanner_max_batch_size_bytes = 100;
    YBScanner scanner(client_table_.get());

    // Set the single-RPC timeout low. Since we only have a single replica of this
    // table, we'll ignore this timeout for the actual scan calls, and use the
    // scanner timeout instead.
    FLAGS_scanner_inject_latency_on_each_batch_ms = 50;
    client_->data_->default_rpc_timeout_ = MonoDelta::FromMilliseconds(1);
    ASSERT_OK(scanner.SetTimeoutMillis(5000));

    // Should successfully scan.
    ASSERT_OK(scanner.Open());
    ASSERT_TRUE(scanner.HasMoreRows());
    while (scanner.HasMoreRows()) {
      YBScanBatch batch;
      ASSERT_OK(scanner.NextBatch(&batch));
    }
  }
}

static std::unique_ptr<YBError> GetSingleErrorFromSession(YBSession* session) {
  CHECK_EQ(1, session->CountPendingErrors());
  CollectedErrors errors = session->GetPendingErrors();
  CHECK_EQ(1, errors.size());
  std::unique_ptr<YBError> result = std::move(errors.front());
  return result;
}

// Simplest case of inserting through the client API: a single row
// with manual batching.
// TODO Actually we need to check that hash columns present during insert. But it is not done yet.
TEST_F(ClientTest, DISABLED_TestInsertSingleRowManualBatch) {
  auto session = CreateSession();
  ASSERT_FALSE(session->HasPendingOperations());

  auto insert = client_table_.NewInsertOp();
  // Try inserting without specifying a key: should fail.
  client_table_.AddInt32ColumnValue(insert->mutable_request(), "int_val", 54321);
  client_table_.AddStringColumnValue(insert->mutable_request(), "string_val", "hello world");
  Status s = session->Apply(insert);
  ASSERT_OK(s);
  ASSERT_OK(session->Flush());
  ASSERT_EQ(QLResponsePB::YQL_STATUS_RUNTIME_ERROR, insert->response().status());

  // Retry
  QLAddInt32HashValue(insert->mutable_request(), 12345);
  ASSERT_OK(session->Apply(insert));
  ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";

  FlushSessionOrDie(session, { insert });
}

namespace {

CHECKED_STATUS ApplyInsertToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key,
                                    int int_val,
                                    const char* string_val,
                                    std::shared_ptr<YBqlOp>* op = nullptr) {
  auto insert = table.NewInsertOp();
  QLAddInt32HashValue(insert->mutable_request(), row_key);
  table.AddInt32ColumnValue(insert->mutable_request(), "int_val", int_val);
  table.AddStringColumnValue(insert->mutable_request(), "string_val", string_val);
  if (op) {
    *op = insert;
  }
  return session->Apply(insert);
}

CHECKED_STATUS ApplyUpdateToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key,
                                    int int_val) {
  auto update = table.NewUpdateOp();
  QLAddInt32HashValue(update->mutable_request(), row_key);
  table.AddInt32ColumnValue(update->mutable_request(), "int_val", int_val);
  return session->Apply(update);
}

CHECKED_STATUS ApplyDeleteToSession(YBSession* session,
                                    const TableHandle& table,
                                    int row_key) {
  auto del = table.NewDeleteOp();
  QLAddInt32HashValue(del->mutable_request(), row_key);
  return session->Apply(del);
}

} // namespace

TEST_F(ClientTest, TestWriteTimeout) {
  auto session = CreateSession();

  // First time out the lookup on the master side.
  {
    google::FlagSaver saver;
    FLAGS_master_inject_latency_on_tablet_lookups_ms = 110;
    session->SetTimeout(100ms);
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
    Status s = session->Flush();
    ASSERT_TRUE(s.IsIOError()) << "unexpected status: " << s.ToString();
    auto error = GetSingleErrorFromSession(session.get());
    ASSERT_TRUE(error->status().IsTimedOut()) << error->status().ToString();
    ASSERT_STR_CONTAINS(error->status().ToString(),
        strings::Substitute("GetTableLocations($0, hash_code: 4624, 1) failed: "
            "timed out after deadline expired", client_table_->name().ToString()));
  }

  // Next time out the actual write on the tablet server.
  {
    google::FlagSaver saver;
    FLAGS_log_inject_latency = true;
    FLAGS_log_inject_latency_ms_mean = 110;
    FLAGS_log_inject_latency_ms_stddev = 0;

    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
    Status s = session->Flush();
    ASSERT_TRUE(s.IsIOError());
    auto error = GetSingleErrorFromSession(session.get());
    ASSERT_TRUE(error->status().IsTimedOut()) << error->status().ToString();
    ASSERT_STR_CONTAINS(error->status().ToString(), "Failed Write");
    ASSERT_STR_CONTAINS(error->status().ToString(), "Write RPC to 127.0.0.1:");
    ASSERT_STR_CONTAINS(error->status().ToString(), "after 1 attempt");
  }
}

// Test which does an async flush and then drops the reference
// to the Session. This should still call the callback.
TEST_F(ClientTest, TestAsyncFlushResponseAfterSessionDropped) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  Synchronizer s;
  YBStatusMemberCallback<Synchronizer> cb(&s, &Synchronizer::StatusCB);
  session->FlushAsync(&cb);
  session.reset();
  ASSERT_OK(s.Wait());

  // Try again, this time should not have an error response (to re-insert the same row).
  s.Reset();
  session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  ASSERT_EQ(1, session->CountBufferedOperations());
  session->FlushAsync(&cb);
  ASSERT_EQ(0, session->CountBufferedOperations());
  session.reset();
  ASSERT_TRUE(s.Wait().ok());
}

TEST_F(ClientTest, TestSessionClose) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  // Closing the session now should return Status::IllegalState since we
  // have a pending operation.
  ASSERT_TRUE(session->Close().IsIllegalState());

  Synchronizer s;
  YBStatusMemberCallback<Synchronizer> cb(&s, &Synchronizer::StatusCB);
  session->FlushAsync(&cb);
  ASSERT_OK(s.Wait());

  ASSERT_OK(session->Close());
}

// Test which sends multiple batches through the same session, each of which
// contains multiple rows spread across multiple tablets.
TEST_F(ClientTest, TestMultipleMultiRowManualBatches) {
  auto session = CreateSession();

  const int kNumBatches = 5;
  const int kRowsPerBatch = 10;

  int row_key = 0;

  for (int batch_num = 0; batch_num < kNumBatches; batch_num++) {
    for (int i = 0; i < kRowsPerBatch; i++) {
      ASSERT_OK(ApplyInsertToSession(
                         session.get(),
                         (row_key % 2 == 0) ? client_table_ : client_table2_,
                         row_key, row_key * 10, "hello world"));
      row_key++;
    }
    ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";
    FlushSessionOrDie(session);
    ASSERT_FALSE(session->HasPendingOperations()) << "Should have no more pending ops after flush";
  }

  const int kNumRowsPerTablet = kNumBatches * kRowsPerBatch / 2;
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table_));
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table2_));

  // Verify the data looks right.
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  std::sort(rows.begin(), rows.end());
  ASSERT_EQ(kNumRowsPerTablet, rows.size());
  ASSERT_EQ("(int32 key=0, int32 int_val=0, string string_val=hello world, "
            "int32 non_null_with_default=12345)"
            , rows[0]);
}

// Test a batch where one of the inserted rows succeeds and duplicates succeed too.
TEST_F(ClientTest, TestBatchWithDuplicates) {
  auto session = CreateSession();

  // Insert a row with key "1"
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);

  // Now make a batch that has key "1" along with
  // key "2" which will succeed. Flushing should not return an error.
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "Attempted dup"));
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 2, 1, "Should succeed"));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());

  // Verify that the other row was successfully inserted
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(2, rows.size());
  std::sort(rows.begin(), rows.end());
  ASSERT_EQ("(int32 key=1, int32 int_val=1, string string_val=Attempted dup, "
            "int32 non_null_with_default=12345)", rows[0]);
  ASSERT_EQ("(int32 key=2, int32 int_val=1, string string_val=Should succeed, "
            "int32 non_null_with_default=12345)", rows[1]);
}

// Test flushing an empty batch (should be a no-op).
TEST_F(ClientTest, TestEmptyBatch) {
  auto session = CreateSession();
  FlushSessionOrDie(session);
}

void ClientTest::DoTestWriteWithDeadServer(WhichServerToKill which) {
  DontVerifyClusterBeforeNextTearDown();
  auto session = CreateSession();
  session->SetTimeout(1s);

  // Shut down the server.
  switch (which) {
    case DEAD_MASTER:
      cluster_->mini_master()->Shutdown();
      break;
    case DEAD_TSERVER:
      cluster_->mini_tablet_server(0)->Shutdown();
      break;
  }

  // Try a write.
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  Status s = session->Flush();
  ASSERT_TRUE(s.IsIOError()) << s.ToString();

  auto error = GetSingleErrorFromSession(session.get());
  switch (which) {
    case DEAD_MASTER:
      // Only one master, so no retry for finding the new leader master.
      ASSERT_TRUE(error->status().IsNetworkError());
      break;
    case DEAD_TSERVER:
      ASSERT_TRUE(error->status().IsTimedOut());
      auto pos = error->status().ToString().find("Connection refused");
      if (pos == std::string::npos) {
        pos = error->status().ToString().find("Broken pipe");
      }
      ASSERT_NE(std::string::npos, pos);
      break;
  }

  ASSERT_STR_CONTAINS(error->failed_op().ToString(), "QL_WRITE");
}

// Test error handling cases where the master is down (tablet resolution fails)
TEST_F(ClientTest, TestWriteWithDeadMaster) {
  client_->data_->default_admin_operation_timeout_ = MonoDelta::FromSeconds(1);
  DoTestWriteWithDeadServer(DEAD_MASTER);
}

// Test error handling when the TS is down (actual write fails its RPC)
TEST_F(ClientTest, TestWriteWithDeadTabletServer) {
  DoTestWriteWithDeadServer(DEAD_TSERVER);
}

void ClientTest::DoApplyWithoutFlushTest(int sleep_micros) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  SleepFor(MonoDelta::FromMicroseconds(sleep_micros));
  session.reset(); // should not crash!

  // Should have no rows.
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());
}


// Applies some updates to the session, and then drops the reference to the
// Session before flushing. Makes sure that the tablet resolution callbacks
// properly deal with the session disappearing underneath.
//
// This test doesn't sleep between applying the operations and dropping the
// reference, in hopes that the reference will be dropped while DNS is still
// in-flight, etc.
TEST_F(ClientTest, TestApplyToSessionWithoutFlushing_OpsInFlight) {
  DoApplyWithoutFlushTest(0);
}

// Same as the above, but sleeps a little bit after applying the operations,
// so that the operations are already in the per-TS-buffer.
TEST_F(ClientTest, TestApplyToSessionWithoutFlushing_OpsBuffered) {
  DoApplyWithoutFlushTest(10000);
}

// Apply a large amount of data without calling Flush(), and ensure
// that we get an error on Apply() rather than sending a too-large
// RPC to the server.
TEST_F(ClientTest, DISABLED_TestApplyTooMuchWithoutFlushing) {

  // Applying a bunch of small rows without a flush should result
  // in an error.
  {
    bool got_expected_error = false;
    auto session = CreateSession();
    for (int i = 0; i < 1000000; i++) {
      Status s = ApplyInsertToSession(session.get(), client_table_, 1, 1, "x");
      if (s.IsIncomplete()) {
        ASSERT_STR_CONTAINS(s.ToString(), "not enough space remaining in buffer");
        got_expected_error = true;
        break;
      } else {
        ASSERT_OK(s);
      }
    }
    ASSERT_TRUE(got_expected_error);
  }

  // Writing a single very large row should also result in an error.
  {
    string huge_string(10 * 1024 * 1024, 'x');

    shared_ptr<YBSession> session = client_->NewSession();
    Status s = ApplyInsertToSession(session.get(), client_table_, 1, 1, huge_string.c_str());
    ASSERT_TRUE(s.IsIncomplete()) << "got unexpected status: " << s.ToString();
  }
}

// Test that update updates and delete deletes with expected use
TEST_F(ClientTest, TestMutationsWork) {
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);

  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  FlushSessionOrDie(session);
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("(int32 key=1, int32 int_val=2, string string_val=original row, "
            "int32 non_null_with_default=12345)", rows[0]);
  rows.clear();

  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());
}

TEST_F(ClientTest, TestMutateDeletedRow) {
  vector<string> rows;
  auto session = CreateSession();
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  FlushSessionOrDie(session);
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());

  // Attempt update deleted row
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());

  // Attempt delete deleted row
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());
}

TEST_F(ClientTest, TestMutateNonexistentRow) {
  vector<string> rows;
  auto session = CreateSession();

  // Attempt update nonexistent row
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  Status s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());

  // Attempt delete nonexistent row
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  s = session->Flush();
  ASSERT_TRUE(s.ok());
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());
}

// Do a write with a bad schema on the client side. This should make the Prepare
// phase of the write fail, which will result in an error on the RPC response.
TEST_F(ClientTest, TestWriteWithBadSchema) {
  // Remove the 'int_val' column.
  // Now the schema on the client is "old"
  gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  ASSERT_OK(table_alterer->DropColumn("int_val")->Alter());

  // Try to do a write with the bad schema.
  auto session = CreateSession();
  std::shared_ptr<YBqlOp> op;
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 12345, 12345, "x", &op));
  ASSERT_OK(session->Flush());
  ASSERT_EQ(QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH, op->response().status());
}

TEST_F(ClientTest, TestBasicAlterOperations) {
  // test that having no steps throws an error
  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    Status s = table_alterer->Alter();
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(), "No alter steps provided");
  }

  // test that adding a non-nullable column with no default value throws an error
  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->AddColumn("key")->Type(INT32)->NotNull();
    Status s = table_alterer->Alter();
    ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
    ASSERT_STR_CONTAINS(s.ToString(), "column `key`: NOT NULL columns must have a default");
  }

  // test that remove key should throws an error
  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    Status s = table_alterer
      ->DropColumn("key")
      ->Alter();
    ASSERT_TRUE(s.IsInvalidArgument());
    ASSERT_STR_CONTAINS(s.ToString(), "cannot remove a key column");
  }

  // test that renaming to an already-existing name throws an error
  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->AlterColumn("int_val")->RenameTo("string_val");
    Status s = table_alterer->Alter();
    ASSERT_TRUE(s.IsAlreadyPresent());
    ASSERT_STR_CONTAINS(s.ToString(), "The column already exists: string_val");
  }

  // Need a tablet peer for the next set of tests.
  string tablet_id = GetFirstTabletId(client_table_.get());
  scoped_refptr<TabletPeer> tablet_peer;
  ASSERT_TRUE(cluster_->mini_tablet_server(0)->server()->tablet_manager()->LookupTablet(
      tablet_id, &tablet_peer));

  {
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    table_alterer->DropColumn("int_val")
      ->AddColumn("new_col")->Type(INT32);
    ASSERT_OK(table_alterer->Alter());
    ASSERT_EQ(1, tablet_peer->tablet()->metadata()->schema_version());
  }

  {
    const YBTableName kRenamedTableName(kKeyspaceName, "RenamedTable");
    gscoped_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
    ASSERT_OK(table_alterer
              ->RenameTo(kRenamedTableName)
              ->Alter());
    ASSERT_EQ(2, tablet_peer->tablet()->metadata()->schema_version());
    ASSERT_EQ(kRenamedTableName.table_name(), tablet_peer->tablet()->metadata()->table_name());

    vector<YBTableName> tables;
    ASSERT_OK(client_->ListTables(&tables));
    ASSERT_TRUE(::util::gtl::contains(tables.begin(), tables.end(), kRenamedTableName));
    ASSERT_FALSE(::util::gtl::contains(tables.begin(), tables.end(), kTableName));
  }
}

TEST_F(ClientTest, TestDeleteTable) {
  // Open the table before deleting it.
  ASSERT_OK(client_table_.Open(kTableName, client_.get()));

  // Insert a few rows, and scan them back. This is to populate the MetaCache.
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(10, rows.size());

  // Remove the table
  // NOTE that it returns when the operation is completed on the master side
  string tablet_id = GetFirstTabletId(client_table_.get());
  ASSERT_OK(client_->DeleteTable(kTableName));
  vector<YBTableName> tables;
  ASSERT_OK(client_->ListTables(&tables));
  ASSERT_FALSE(::util::gtl::contains(tables.begin(), tables.end(), kTableName));

  // Wait until the table is removed from the TS
  int wait_time = 1000;
  bool tablet_found = true;
  for (int i = 0; i < 80 && tablet_found; ++i) {
    scoped_refptr<TabletPeer> tablet_peer;
    tablet_found = cluster_->mini_tablet_server(0)->server()->tablet_manager()->LookupTablet(
                      tablet_id, &tablet_peer);
    SleepFor(MonoDelta::FromMicroseconds(wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000);
  }
  ASSERT_FALSE(tablet_found);

  // Try to open the deleted table
  Status s = client_table_.Open(kTableName, client_.get());
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "The table does not exist");

  // Create a new table with the same name. This is to ensure that the client
  // doesn't cache anything inappropriately by table name (see KUDU-1055).
  ASSERT_NO_FATALS(CreateTable(kTableName, 1, kNumTablets, &client_table_));

  // Should be able to insert successfully into the new table.
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 10));
}

TEST_F(ClientTest, TestGetTableSchema) {
  YBSchema schema;
  PartitionSchema partition_schema;

  // Verify the schema for the current table
  ASSERT_OK(client_->GetTableSchema(kTableName, &schema, &partition_schema));
  ASSERT_TRUE(schema_.Equals(schema));

  // Verify that a get schema request for a missing table throws not found
  Status s = client_->GetTableSchema(YBTableName(kKeyspaceName, "MissingTableName"), &schema,
                                     &partition_schema);
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_STR_CONTAINS(s.ToString(), "The table does not exist");
}

TEST_F(ClientTest, TestStaleLocations) {
  string tablet_id = GetFirstTabletId(client_table2_.get());

  // The Tablet is up and running the location should not be stale
  master::TabletLocationsPB locs_pb;
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));
  ASSERT_FALSE(locs_pb.stale());

  // On Master restart and no tablet report we expect the locations to be stale
  cluster_->mini_tablet_server(0)->Shutdown();
  ASSERT_OK(cluster_->mini_master()->Restart());
  ASSERT_OK(cluster_->mini_master()->master()->WaitUntilCatalogManagerIsLeaderAndReadyForTests());
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));
  ASSERT_TRUE(locs_pb.stale());

  // Restart the TS and Wait for the tablets to be reported to the master.
  ASSERT_OK(cluster_->mini_tablet_server(0)->Start());
  ASSERT_OK(cluster_->WaitForTabletServerCount(1));
  ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                  tablet_id, &locs_pb));

  // It may take a while to bootstrap the tablet and send the location report
  // so spin until we get a non-stale location.
  int wait_time = 1000;
  for (int i = 0; i < 80; ++i) {
    ASSERT_OK(cluster_->mini_master()->master()->catalog_manager()->GetTabletLocations(
                    tablet_id, &locs_pb));
    if (!locs_pb.stale()) {
      break;
    }
    SleepFor(MonoDelta::FromMicroseconds(wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000);
  }
  ASSERT_FALSE(locs_pb.stale());
}

// Test creating and accessing a table which has multiple tablets,
// each of which is replicated.
//
// TODO: this should probably be the default for _all_ of the tests
// in this file. However, some things like alter table are not yet
// working on replicated tables - see KUDU-304
TEST_F(ClientTest, TestReplicatedMultiTabletTable) {
  const YBTableName kReplicatedTable("replicated");
  const int kNumRowsToWrite = 100;
  const int kNumReplicas = 3;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               kNumReplicas,
                               kNumTablets,
                               &table));

  // Should have no rows to begin with.
  ASSERT_EQ(0, CountRowsFromClient(table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // Should now see the data.
  ASSERT_EQ(kNumRowsToWrite, CountRowsFromClient(table));

  // TODO: once leader re-election is in, should somehow force a re-election
  // and ensure that the client handles refreshing the leader.
}

TEST_F(ClientTest, TestReplicatedMultiTabletTableFailover) {
  const YBTableName kReplicatedTable("replicated_failover_on_reads");
  const int kNumRowsToWrite = 100;
  const int kNumReplicas = 3;
  const int kNumTries = 100;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               kNumReplicas,
                               kNumTablets,
                               &table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // Find the leader of the first tablet.
  Synchronizer sync;
  scoped_refptr<internal::RemoteTablet> rt;
  client_->data_->meta_cache_->LookupTabletByKey(table.get(), "",
                                                 MonoTime::Max(),
                                                 &rt, sync.AsStatusCallback());
  ASSERT_OK(sync.Wait());
  internal::RemoteTabletServer *rts = rt->LeaderTServer();

  // Kill the leader of the first tablet.
  ASSERT_OK(KillTServer(rts->permanent_uuid()));

  // We wait until we fail over to the new leader(s).
  int tries = 0;
  for (;;) {
    tries++;
    int num_rows = CountRowsFromClient(table);
    if (num_rows == kNumRowsToWrite) {
      LOG(INFO) << "Found expected number of rows: " << num_rows;
      break;
    } else {
      LOG(INFO) << "Only found " << num_rows << " rows on try "
                << tries << ", retrying";
      ASSERT_LE(tries, kNumTries);
      SleepFor(MonoDelta::FromMilliseconds(10 * tries)); // sleep a bit more with each attempt.
    }
  }
}

// This test that we can keep writing to a tablet when the leader
// tablet dies.
// This currently forces leader promotion through RPC and creates
// a new client afterwards.
// TODO Remove the leader promotion part when we have automated
// leader election.
TEST_F(ClientTest, TestReplicatedTabletWritesWithLeaderElection) {
  const YBTableName kReplicatedTable("replicated_failover_on_writes");
  const int kNumRowsToWrite = 100;
  const int kNumReplicas = 3;

  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReplicatedTable,
                               kNumReplicas,
                               1,
                               &table));

  // Insert some data.
  ASSERT_NO_FATALS(InsertTestRows(table, kNumRowsToWrite));

  // TODO: we have to sleep here to make sure that the leader has time to
  // propagate the writes to the followers. We can remove this once the
  // followers run a leader election on their own and handle advancing
  // the commit index.
  SleepFor(MonoDelta::FromMilliseconds(1500));

  // Find the leader replica
  Synchronizer sync;
  scoped_refptr<internal::RemoteTablet> rt;
  client_->data_->meta_cache_->LookupTabletByKey(table.get(), "",
                                                 MonoTime::Max(),
                                                 &rt, sync.AsStatusCallback());
  ASSERT_OK(sync.Wait());
  internal::RemoteTabletServer *rts;
  set<string> blacklist;
  vector<internal::RemoteTabletServer*> candidates;
  ASSERT_OK(client_->data_->GetTabletServer(client_.get(),
                                            rt,
                                            YBClient::LEADER_ONLY,
                                            blacklist,
                                            &candidates,
                                            &rts));

  string killed_uuid = rts->permanent_uuid();
  // Kill the tserver that is serving the leader tablet.
  ASSERT_OK(KillTServer(killed_uuid));

  // Since we waited before, hopefully all replicas will be up to date
  // and we can just promote another replica.
  rpc::MessengerBuilder bld("client");
  auto client_messenger = bld.Build();
  ASSERT_OK(client_messenger);
  gscoped_ptr<consensus::ConsensusServiceProxy> new_leader_proxy;

  int new_leader_idx = -1;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    MiniTabletServer* ts = cluster_->mini_tablet_server(i);
    if (ts->is_started()) {
      const string& uuid = ts->server()->instance_pb().permanent_uuid();
      if (uuid != killed_uuid) {
        new_leader_idx = i;
        break;
      }
    }
  }
  ASSERT_NE(-1, new_leader_idx);

  MiniTabletServer* new_leader = cluster_->mini_tablet_server(new_leader_idx);
  ASSERT_TRUE(new_leader != nullptr);
  new_leader_proxy.reset(
      new consensus::ConsensusServiceProxy(*client_messenger,
                                           new_leader->bound_rpc_addr()));

  consensus::RunLeaderElectionRequestPB req;
  consensus::RunLeaderElectionResponsePB resp;
  rpc::RpcController controller;

  LOG(INFO) << "Promoting server at index " << new_leader_idx << " listening at "
            << new_leader->bound_rpc_addr() << " ...";
  req.set_dest_uuid(new_leader->server()->fs_manager()->uuid());
  req.set_tablet_id(rt->tablet_id());
  ASSERT_OK(new_leader_proxy->RunLeaderElection(req, &resp, &controller));
  ASSERT_FALSE(resp.has_error()) << "Got error. Response: " << resp.ShortDebugString();

  LOG(INFO) << "Inserting additional rows...";
  ASSERT_NO_FATALS(InsertTestRows(table,
                                  kNumRowsToWrite,
                                  kNumRowsToWrite));

  // TODO: we have to sleep here to make sure that the leader has time to
  // propagate the writes to the followers. We can remove this once the
  // followers run a leader election on their own and handle advancing
  // the commit index.
  SleepFor(MonoDelta::FromMilliseconds(1500));

  LOG(INFO) << "Counting rows...";
  ASSERT_EQ(2 * kNumRowsToWrite, CountRowsFromClient(table,
                                                     YBConsistencyLevel::CONSISTENT_PREFIX,
                                                     kNoBound, kNoBound));
}

namespace {

void CheckCorrectness(YBScanner* scanner, int expected[], int nrows) {
  ASSERT_OK(scanner->Open());
  int readrows = 0;
  YBScanBatch batch;
  if (nrows) {
    ASSERT_TRUE(scanner->HasMoreRows());
  } else {
    ASSERT_FALSE(scanner->HasMoreRows());
  }

  while (scanner->HasMoreRows()) {
    ASSERT_OK(scanner->NextBatch(&batch));
    for (const YBScanBatch::RowPtr& r : batch) {
      int32_t key;
      int32_t val;
      Slice strval;
      ASSERT_OK(r.GetInt32(0, &key));
      ASSERT_OK(r.GetInt32(1, &val));
      ASSERT_OK(r.GetString(2, &strval));
      ASSERT_NE(expected[key], -1) << "Deleted key found in table in table " << key;
      ASSERT_EQ(expected[key], val) << "Incorrect int value for key " <<  key;
      ASSERT_EQ(strval.size(), 0) << "Incorrect string value for key " << key;
      ++readrows;
    }
  }
  ASSERT_EQ(readrows, nrows);
  scanner->Close();
}

} // anonymous namespace

// Randomized mutations accuracy testing
TEST_F(ClientTest, TestRandomWriteOperation) {
  auto session = CreateSession();
  int row[FLAGS_test_scan_num_rows]; // -1 indicates empty
  int nrows;
  YBScanner scanner(client_table_.get());

  // First half-fill
  for (int i = 0; i < FLAGS_test_scan_num_rows/2; ++i) {
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, i, i, ""));
    row[i] = i;
  }
  for (int i = FLAGS_test_scan_num_rows/2; i < FLAGS_test_scan_num_rows; ++i) {
    row[i] = -1;
  }
  nrows = FLAGS_test_scan_num_rows/2;

  // Randomized testing
  LOG(INFO) << "Randomized mutations testing.";
  unsigned int seed = SeedRandom();
  for (int i = 0; i <= 1000; ++i) {
    // Test correctness every so often
    if (i % 50 == 0) {
      LOG(INFO) << "Correctness test " << i;
      FlushSessionOrDie(session);
      ASSERT_NO_FATALS(CheckCorrectness(&scanner, row, nrows));
      LOG(INFO) << "...complete";
    }

    int change = rand_r(&seed) % FLAGS_test_scan_num_rows;
    // Insert if empty
    if (row[change] == -1) {
      ASSERT_OK(ApplyInsertToSession(session.get(),
                                            client_table_,
                                            change,
                                            change,
                                            ""));
      row[change] = change;
      ++nrows;
      VLOG(1) << "Insert " << change;
    } else {
      // Update or delete otherwise
      int update = rand_r(&seed) & 1;
      if (update) {
        ASSERT_OK(ApplyUpdateToSession(session.get(),
                                              client_table_,
                                              change,
                                              ++row[change]));
        VLOG(1) << "Update " << change;
      } else {
        ASSERT_OK(ApplyDeleteToSession(session.get(),
                                              client_table_,
                                              change));
        row[change] = -1;
        --nrows;
        VLOG(1) << "Delete " << change;
      }
    }
  }

  // And one more time for the last batch.
  FlushSessionOrDie(session);
  ASSERT_NO_FATALS(CheckCorrectness(&scanner, row, nrows));
}

// Test whether a batch can handle several mutations in a batch
TEST_F(ClientTest, TestSeveralRowMutatesPerBatch) {
  auto session = CreateSession();

  // Test insert/update
  LOG(INFO) << "Testing insert/update in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, ""));
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 2));
  FlushSessionOrDie(session);
  vector<string> rows;
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("(int32 key=1, int32 int_val=2, string string_val=, "
            "int32 non_null_with_default=12345)", rows[0]);
  rows.clear();


  LOG(INFO) << "Testing insert/delete in same batch, key " << 2 << ".";
  // Test insert/delete
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 2, 1, ""));
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 2));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("(int32 key=1, int32 int_val=2, string string_val=, "
            "int32 non_null_with_default=12345)", rows[0]);
  rows.clear();

  // Test update/delete
  LOG(INFO) << "Testing update/delete in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyUpdateToSession(session.get(), client_table_, 1, 1));
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(0, rows.size());

  // Test delete/insert (insert a row first)
  LOG(INFO) << "Inserting row for delete/insert test, key " << 1 << ".";
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, ""));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("(int32 key=1, int32 int_val=1, string string_val=, "
            "int32 non_null_with_default=12345)", rows[0]);
  rows.clear();
  LOG(INFO) << "Testing delete/insert in same batch, key " << 1 << ".";
  ASSERT_OK(ApplyDeleteToSession(session.get(), client_table_, 1));
  ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, 1, 2, ""));
  FlushSessionOrDie(session);
  ScanTableToStrings(client_table_.get(), &rows);
  ASSERT_EQ(1, rows.size());
  ASSERT_EQ("(int32 key=1, int32 int_val=2, string string_val=, "
            "int32 non_null_with_default=12345)", rows[0]);
            rows.clear();
}

// Tests that master permits are properly released after a whole bunch of
// rows are inserted.
TEST_F(ClientTest, TestMasterLookupPermits) {
  int initial_value = client_->data_->meta_cache_->master_lookup_sem_.GetValue();
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));
  ASSERT_EQ(initial_value,
            client_->data_->meta_cache_->master_lookup_sem_.GetValue());
}

// Define callback for deadlock simulation, as well as various helper methods.
namespace {
class DLSCallback : public YBStatusCallback {
 public:
  explicit DLSCallback(Atomic32* i) : i(i) {
  }

  void Run(const Status& s) override {
    CHECK_OK(s);
    NoBarrier_AtomicIncrement(i, 1);
    delete this;
  }
 private:
  Atomic32* const i;
};

// Returns col1 value of first row.
int32_t ReadFirstRowKeyFirstCol(YBTable* tbl) {
  YBScanner scanner(tbl);

  CHECK_OK(scanner.Open());
  YBScanBatch batch;
  CHECK(scanner.HasMoreRows());
  CHECK_OK(scanner.NextBatch(&batch));
  YBRowResult row = batch.Row(0);
  int32_t val;
  CHECK_OK(row.GetInt32(1, &val));
  return val;
}

// Checks that all rows have value equal to expected, return number of rows.
int CheckRowsEqual(YBTable* tbl, int32_t expected) {
  YBScanner scanner(tbl);
  CHECK_OK(scanner.Open());
  YBScanBatch batch;
  int cnt = 0;
  while (scanner.HasMoreRows()) {
    CHECK_OK(scanner.NextBatch(&batch));
    for (const YBScanBatch::RowPtr& row : batch) {
      // Check that for every key:
      // 1. Column 1 int32_t value == expected
      // 2. Column 2 string value is empty
      // 3. Column 3 int32_t value is default, 12345
      int32_t key;
      int32_t val;
      Slice strval;
      int32_t val2;
      CHECK_OK(row.GetInt32(0, &key));
      CHECK_OK(row.GetInt32(1, &val));
      CHECK_OK(row.GetString(2, &strval));
      CHECK_OK(row.GetInt32(3, &val2));
      CHECK_EQ(expected, val) << "Incorrect int value for key " << key;
      CHECK_EQ(strval.size(), 0) << "Incorrect string value for key " << key;
      CHECK_EQ(12345, val2);
      ++cnt;
    }
  }
  return cnt;
}

// Return a session "loaded" with updates. Sets the session timeout
// to the parameter value. Larger timeouts decrease false positives.
shared_ptr<YBSession> LoadedSession(const shared_ptr<YBClient>& client,
                                    const TableHandle& tbl,
                                    bool fwd, int max, MonoDelta timeout) {
  shared_ptr<YBSession> session = client->NewSession();
  session->SetTimeout(timeout);
  CHECK_OK(session->SetFlushMode(YBSession::MANUAL_FLUSH));
  for (int i = 0; i < max; ++i) {
    int key = fwd ? i : max - i;
    CHECK_OK(ApplyUpdateToSession(session.get(), tbl, key, fwd));
  }
  return session;
}

} // anonymous namespace

// Starts many clients which update a table in parallel.
// Half of the clients update rows in ascending order while the other
// half update rows in descending order.
// This ensures that we don't hit a deadlock in such a situation.
TEST_F(ClientTest, TestDeadlockSimulation) {
  if (!AllowSlowTests()) {
    LOG(WARNING) << "TestDeadlockSimulation disabled since slow.";
    return;
  }

  // Make reverse client who will make batches that update rows
  // in reverse order. Separate client used so rpc calls come in at same time.
  shared_ptr<YBClient> rev_client;
  ASSERT_OK(YBClientBuilder()
                   .add_master_server_addr(ToString(cluster_->mini_master()->bound_rpc_addr()))
                   .Build(&rev_client));
  TableHandle rev_table;
  ASSERT_OK(rev_table.Open(kTableName, client_.get()));

  // Load up some rows
  const int kNumRows = 300;
  const auto kTimeout = 60s;
  auto session = CreateSession();
  for (int i = 0; i < kNumRows; ++i)
    ASSERT_OK(ApplyInsertToSession(session.get(), client_table_, i, i,  ""));
  FlushSessionOrDie(session);

  // Check both clients see rows
  int fwd = CountRowsFromClient(client_table_);
  ASSERT_EQ(kNumRows, fwd);
  int rev = CountRowsFromClient(rev_table);
  ASSERT_EQ(kNumRows, rev);

  // Generate sessions
  const int kNumSessions = 100;
  shared_ptr<YBSession> fwd_sessions[kNumSessions];
  shared_ptr<YBSession> rev_sessions[kNumSessions];
  for (int i = 0; i < kNumSessions; ++i) {
    fwd_sessions[i] = LoadedSession(client_, client_table_, true, kNumRows, kTimeout);
    rev_sessions[i] = LoadedSession(rev_client, rev_table, true, kNumRows, kTimeout);
  }

  // Run async calls - one thread updates sequentially, another in reverse.
  Atomic32 ctr1, ctr2;
  NoBarrier_Store(&ctr1, 0);
  NoBarrier_Store(&ctr2, 0);
  for (int i = 0; i < kNumSessions; ++i) {
    // The callbacks are freed after they are invoked.
    fwd_sessions[i]->FlushAsync(new DLSCallback(&ctr1));
    rev_sessions[i]->FlushAsync(new DLSCallback(&ctr2));
  }

  // Spin while waiting for ops to complete.
  int lctr1, lctr2, prev1 = 0, prev2 = 0;
  do {
    lctr1 = NoBarrier_Load(&ctr1);
    lctr2 = NoBarrier_Load(&ctr2);
    // Display progress in 10% increments.
    if (prev1 == 0 || lctr1 + lctr2 - prev1 - prev2 > kNumSessions / 10) {
      LOG(INFO) << "# updates: " << lctr1 << " fwd, " << lctr2 << " rev";
      prev1 = lctr1;
      prev2 = lctr2;
    }
    SleepFor(MonoDelta::FromMilliseconds(100));
  } while (lctr1 != kNumSessions|| lctr2 != kNumSessions);
  int32_t expected = ReadFirstRowKeyFirstCol(client_table_.get());

  // Check transaction from forward client.
  fwd = CheckRowsEqual(client_table_.get(), expected);
  ASSERT_EQ(fwd, kNumRows);

  // Check from reverse client side.
  rev = CheckRowsEqual(rev_table.get(), expected);
  ASSERT_EQ(rev, kNumRows);
}

TEST_F(ClientTest, TestCreateDuplicateTable) {
  gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_TRUE(table_creator->table_name(kTableName)
              .schema(&schema_)
              .num_replicas(1)
              .Create().IsAlreadyPresent());
}

TEST_F(ClientTest, CreateTableWithoutTservers) {
  DoTearDown();

  YBMiniClusterTestBase::SetUp();

  MiniClusterOptions options;
  options.num_tablet_servers = 0;
  // Start minicluster with only master (to simulate tserver not yet heartbeating).
  cluster_.reset(new MiniCluster(env_.get(), options));
  ASSERT_OK(cluster_->Start());

  // Connect to the cluster.
  ASSERT_OK(YBClientBuilder()
      .add_master_server_addr(yb::ToString(cluster_->mini_master()->bound_rpc_addr()))
      .Build(&client_));

  gscoped_ptr<client::YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(kKeyspaceName, "foobar"))
      .schema(&schema_)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(), "num_tablets should be greater than 0.");
}

TEST_F(ClientTest, TestCreateTableWithTooManyTablets) {
  FLAGS_max_create_tablets_per_ts = 1;

  gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(kKeyspaceName, "foobar"))
      .schema(&schema_)
      .num_tablets(2)
      .num_replicas(3)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(),
                      "The requested number of tablets is over the permitted maximum (1)");
}

TEST_F(ClientTest, TestCreateTableWithTooManyReplicas) {
  gscoped_ptr<YBTableCreator> table_creator(client_->NewTableCreator());
  Status s = table_creator->table_name(YBTableName(kKeyspaceName, "foobar"))
      .schema(&schema_)
      .num_tablets(2)
      .num_replicas(3)
      .Create();
  ASSERT_TRUE(s.IsInvalidArgument());
  ASSERT_STR_CONTAINS(s.ToString(),
                      "Not enough live tablet servers to create a table with the requested "
                      "replication factor 3. 1 tablet servers are alive");
}

TEST_F(ClientTest, TestLatestObservedHybridTime) {
  // Check that a write updates the latest observed hybrid_time.
  uint64_t ht0 = client_->GetLatestObservedHybridTime();
  ASSERT_EQ(ht0, YBClient::kNoHybridTime);
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 1, 0));
  uint64_t ht1 = client_->GetLatestObservedHybridTime();
  ASSERT_NE(ht0, ht1);

  // Check that the hybrid_time of the previous write will be observed by another
  // client performing a snapshot scan at that hybrid_time.
  shared_ptr<YBClient> client;
  shared_ptr<YBTable> table;
  ASSERT_OK(YBClientBuilder()
      .add_master_server_addr(ToString(cluster_->mini_master()->bound_rpc_addr()))
      .Build(&client));
  ASSERT_EQ(client->GetLatestObservedHybridTime(), YBClient::kNoHybridTime);
  ASSERT_OK(client->OpenTable(client_table_->name(), &table));
  YBScanner scanner(table.get());
  ASSERT_OK(scanner.SetReadMode(YBScanner::READ_AT_SNAPSHOT));
  ASSERT_OK(scanner.SetSnapshotRaw(ht1));
  ASSERT_OK(scanner.Open());
  scanner.Close();
  uint64_t ht2 = client->GetLatestObservedHybridTime();
  ASSERT_EQ(ht1, ht2);
}

TEST_F(ClientTest, TestClonePredicates) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, 2, 0));
  gscoped_ptr<YBPredicate> predicate(client_table_->NewComparisonPredicate(
      "key",
      YBPredicate::EQUAL,
      YBValue::FromInt(1)));

  gscoped_ptr<YBScanner> scanner(new YBScanner(client_table_.get()));
  ASSERT_OK(scanner->AddConjunctPredicate(predicate->Clone()));
  ASSERT_OK(scanner->Open());

  int count = 0;
  YBScanBatch batch;
  while (scanner->HasMoreRows()) {
    ASSERT_OK(scanner->NextBatch(&batch));
    count += batch.NumRows();
  }

  ASSERT_EQ(count, 1);

  scanner.reset(new YBScanner(client_table_.get()));
  ASSERT_OK(scanner->AddConjunctPredicate(predicate->Clone()));
  ASSERT_OK(scanner->Open());

  count = 0;
  while (scanner->HasMoreRows()) {
    ASSERT_OK(scanner->NextBatch(&batch));
    count += batch.NumRows();
  }

  ASSERT_EQ(count, 1);
}

// Test that scanners will retry after receiving ERROR_SERVER_TOO_BUSY from an
// overloaded tablet server. Regression test for KUDU-1079.
TEST_F(ClientTest, TestServerTooBusyRetry) {
  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  // Introduce latency in each scan to increase the likelihood of
  // ERROR_SERVER_TOO_BUSY.
  FLAGS_scanner_inject_latency_on_each_batch_ms = 10;

  // Reduce the service queue length of each tablet server in order to increase
  // the likelihood of ERROR_SERVER_TOO_BUSY.
  FLAGS_tablet_server_svc_queue_length = 1;
  for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
    MiniTabletServer* ts = cluster_->mini_tablet_server(i);
    ASSERT_OK(ts->Restart());
    ASSERT_OK(ts->WaitStarted());
  }

  bool stop = false;
  vector<scoped_refptr<yb::Thread> > threads;
  int t = 0;
  while (!stop) {
    scoped_refptr<yb::Thread> thread;
    ASSERT_OK(yb::Thread::Create("test", strings::Substitute("t$0", t++),
                                 &CheckRowCount, std::cref(client_table_), &thread));
    threads.push_back(thread);

    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      scoped_refptr<Counter> counter = METRIC_rpcs_queue_overflow.Instantiate(
          cluster_->mini_tablet_server(i)->server()->metric_entity());
      stop = counter->value() > 0;
    }
  }

  for (const scoped_refptr<yb::Thread>& thread : threads) {
    thread->Join();
  }
}

TEST_F(ClientTest, TestLastErrorEmbeddedInScanTimeoutStatus) {
  // For the random() calls that take place during scan retries.
  SeedRandom();

  ASSERT_NO_FATALS(InsertTestRows(client_table_, FLAGS_test_scan_num_rows));

  {
    // Revert the latency injection flags at the end so the test exits faster.
    google::FlagSaver saver;

    // Restart, but inject latency so that startup is very slow.
    FLAGS_log_inject_latency = true;
    FLAGS_log_inject_latency_ms_mean = 1000;
    FLAGS_log_inject_latency_ms_stddev = 0;
    for (int i = 0; i < cluster_->num_tablet_servers(); i++) {
      MiniTabletServer* ts = cluster_->mini_tablet_server(i);
      ASSERT_OK(ts->Restart());
    }

    // As the tservers are still starting up, the scan will retry until it
    // times out. The actual error should be embedded in the returned status.
    YBScanner scan(client_table_.get());
    ASSERT_OK(scan.SetTimeoutMillis(1000));
    Status s = scan.Open();
    SCOPED_TRACE(s.ToString());
    ASSERT_TRUE(s.IsTimedOut());
    auto message = s.ToString(false);
    ASSERT_STR_CONTAINS(message, "Illegal state (");
    ASSERT_STR_CONTAINS(message, "): Tablet not RUNNING");
  }
}

TEST_F(ClientTest, TestReadFromFollower) {
  // Create table and write some rows.
  const YBTableName kReadFromFollowerTable("TestReadFromFollower");
  TableHandle table;
  ASSERT_NO_FATALS(CreateTable(kReadFromFollowerTable, 3, 1, &table));
  ASSERT_NO_FATALS(InsertTestRows(table, FLAGS_test_scan_num_rows));

  // Find the followers.
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;
  table->name().SetIntoTableIdentifierPB(req.mutable_table());
  CHECK_OK(cluster_->mini_master()->master()->catalog_manager()->GetTableLocations(&req, &resp));
  ASSERT_EQ(1, resp.tablet_locations_size());
  ASSERT_EQ(3, resp.tablet_locations(0).replicas_size());
  const string& tablet_id = resp.tablet_locations(0).tablet_id();

  vector<master::TSInfoPB> followers;
  for (const auto& replica : resp.tablet_locations(0).replicas()) {
    if (replica.role() == consensus::RaftPeerPB_Role_FOLLOWER) {
      followers.push_back(replica.ts_info());
    }
  }
  ASSERT_EQ(cluster_->num_tablet_servers() - 1, followers.size());

  rpc::MessengerBuilder bld("client");
  auto client_messenger = bld.Build();
  ASSERT_OK(client_messenger);
  for (const master::TSInfoPB& ts_info : followers) {
    // Try to read from followers.
    auto endpoint = ParseEndpoint(ts_info.rpc_addresses(0).host(),
                                  ts_info.rpc_addresses(0).port());
    ASSERT_TRUE(endpoint.ok());
    auto tserver_proxy = std::make_unique<tserver::TabletServerServiceProxy>(
        *client_messenger, *endpoint);

    std::unique_ptr<QLRowBlock> rowBlock;
    ASSERT_OK(WaitFor([&]() -> bool {
      // Setup read request.
      tserver::ReadRequestPB req;
      tserver::ReadResponsePB resp;
      rpc::RpcController controller;
      req.set_tablet_id(tablet_id);
      req.set_consistency_level(YBConsistencyLevel::CONSISTENT_PREFIX);
      QLReadRequestPB *ql_read = req.mutable_ql_batch()->Add();
      std::shared_ptr<std::vector<ColumnSchema>> selected_cols =
          std::make_shared<std::vector<ColumnSchema>>(schema_.columns());
      QLRSRowDescPB *rsrow_desc = ql_read->mutable_rsrow_desc();
      for (int i = 0; i < schema_.num_columns(); i++) {
        ql_read->add_selected_exprs()->set_column_id(yb::kFirstColumnId + i);
        ql_read->mutable_column_refs()->add_ids(yb::kFirstColumnId + i);

        QLRSColDescPB *rscol_desc = rsrow_desc->add_rscol_descs();
        rscol_desc->set_name((*selected_cols)[i].name());
        (*selected_cols)[i].type()->ToQLTypePB(rscol_desc->mutable_ql_type());
      }

      EXPECT_OK(tserver_proxy->Read(req, &resp, &controller));

      // Verify response.
      EXPECT_FALSE(resp.has_error());
      EXPECT_EQ(1, resp.ql_batch_size());
      const QLResponsePB &ql_resp = resp.ql_batch(0);
      EXPECT_EQ(QLResponsePB_QLStatus_YQL_STATUS_OK, ql_resp.status());
      EXPECT_TRUE(ql_resp.has_rows_data_sidecar());

      Slice rows_data;
      EXPECT_TRUE(controller.finished());
      EXPECT_OK(controller.GetSidecar(ql_resp.rows_data_sidecar(), &rows_data));
      yb::ql::RowsResult rowsResult(kReadFromFollowerTable, selected_cols, rows_data.ToBuffer());
      rowBlock = rowsResult.GetRowBlock();
      return FLAGS_test_scan_num_rows == rowBlock->row_count();
    }, MonoDelta::FromSeconds(30), "Waiting for replication to followers"));

    std::vector<bool> seen_key(rowBlock->row_count());
    for (int i = 0; i < rowBlock->row_count(); i++) {
      const QLRow& row = rowBlock->row(i);
      auto key = row.column(0).int32_value();
      ASSERT_LT(key, seen_key.size());
      ASSERT_FALSE(seen_key[key]);
      seen_key[key] = true;
      ASSERT_EQ(key * 2, row.column(1).int32_value());
      ASSERT_EQ(StringPrintf("hello %d", key), row.column(2).string_value());
      ASSERT_EQ(key * 3, row.column(3).int32_value());
    }
  }
}

}  // namespace client
}  // namespace yb
