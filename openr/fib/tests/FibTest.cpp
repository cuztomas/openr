/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkFibHandler.h"

#include <chrono>
#include <thread>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/NetworkUtil.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

using namespace std;
using namespace openr;

using apache::thrift::FRAGILE;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

const int16_t kFibId{static_cast<int16_t>(thrift::FibClient::OPENR)};

const auto prefix1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto prefix2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto prefix3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto prefix4 = toIpPrefix("::ffff:10.4.4.4/128");

const auto label1{1};
const auto label2{2};
const auto label3{3};

const auto path1_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1);
const auto path1_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2);
const auto path1_2_3 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_3"),
    1);
const auto path1_3_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    2);
const auto path1_3_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2);
const auto path3_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_3_2_1"),
    1);
const auto path3_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_3_2_2"),
    2);
const auto path3_4_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::4")),
    std::string("iface_3_4_1"),
    2);
const auto path3_4_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::4")),
    std::string("iface_3_4_2"),
    2);

const auto mpls_path1_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 2),
    true /* useNonShortestPath */);
const auto mpls_path1_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 2),
    true /* useNonShortestPath */);

bool
checkEqualRoutes(thrift::RouteDatabase lhs, thrift::RouteDatabase rhs) {
  if (lhs.unicastRoutes.size() != rhs.unicastRoutes.size()) {
    return false;
  }
  std::unordered_map<thrift::IpPrefix, std::set<thrift::NextHopThrift>>
      lhsRoutes;
  std::unordered_map<thrift::IpPrefix, std::set<thrift::NextHopThrift>>
      rhsRoutes;
  for (auto const& route : lhs.unicastRoutes) {
    lhsRoutes.emplace(
        route.dest,
        std::set<thrift::NextHopThrift>(
            route.nextHops.begin(), route.nextHops.end()));
  }
  for (auto const& route : rhs.unicastRoutes) {
    rhsRoutes.emplace(
        route.dest,
        std::set<thrift::NextHopThrift>(
            route.nextHops.begin(), route.nextHops.end()));
  }

  for (auto const& kv : lhsRoutes) {
    if (rhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (rhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }

  for (auto const& kv : rhsRoutes) {
    if (lhsRoutes.count(kv.first) == 0) {
      return false;
    }
    if (lhsRoutes.at(kv.first) != kv.second) {
      return false;
    }
  }

  return true;
}

class FibTestFixture : public ::testing::Test {
 public:
  explicit FibTestFixture(bool waitOnDecision = false)
      : waitOnDecision_(waitOnDecision) {}
  void
  SetUp() override {
    mockFibHandler = std::make_shared<MockNetlinkFibHandler>();

    server = make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler);

    fibThriftThread.start(server);
    port = fibThriftThread.getAddress()->getPort();

    EXPECT_NO_THROW(
        decisionPub.bind(fbzmq::SocketUrl{"inproc://decision-pub"}).value());
    EXPECT_NO_THROW(
        decisionRep.bind(fbzmq::SocketUrl{"inproc://decision-cmd"}).value());
    EXPECT_NO_THROW(lmPub.bind(fbzmq::SocketUrl{"inproc://lm-pub"}).value());

    fib = std::make_shared<Fib>(
        "node-1",
        port, /* thrift port */
        false, /* dryrun */
        true, /* segment route */
        false, /* orderedFib */
        std::chrono::seconds(2),
        waitOnDecision_,
        DecisionPubUrl{"inproc://decision-pub"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalCmdUrl{"inproc://kvstore-cmd"},
        KvStoreLocalPubUrl{"inproc://kvstore-pub"},
        context);

    fibThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib->waitUntilRunning();

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        "node-1",
        MonitorSubmitUrl{"inproc://monitor-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        context);
    openrThriftServerWrapper_->addModuleType(thrift::OpenrModuleType::FIB, fib);
    openrThriftServerWrapper_->run();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping openr-ctrl thrift server";
    openrThriftServerWrapper_->stop();
    LOG(INFO) << "Openr-ctrl thrift server got stopped";

    // this will be invoked before Fib's d-tor
    LOG(INFO) << "Stopping the Fib thread";
    fib->stop();
    fibThread->join();

    decisionPub.close();
    decisionRep.close();
    lmPub.close();

    // stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
    LOG(INFO) << "Mock fib platform is stopped";
  }

  thrift::RouteDatabase
  getRouteDb() {
    auto resp = openrThriftServerWrapper_->getOpenrCtrlHandler()
                    ->semifuture_getRouteDb()
                    .get();
    EXPECT_TRUE(resp);
    return std::move(*resp);
  }

  std::vector<thrift::UnicastRoute>
  getUnicastRoutesFiltered(std::unique_ptr<std::vector<std::string>> prefixes) {
    auto resp = openrThriftServerWrapper_->getOpenrCtrlHandler()
                    ->semifuture_getUnicastRoutesFiltered(std::move(prefixes))
                    .get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  std::vector<thrift::UnicastRoute>
  getUnicastRoutes() {
    auto resp = openrThriftServerWrapper_->getOpenrCtrlHandler()
                    ->semifuture_getUnicastRoutes()
                    .get();
    EXPECT_TRUE(resp);
    return *resp;
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> decisionPub{context};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> decisionRep{context};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> lmPub{context};

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler;

 private:
  // thriftServer to talk to Fib
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};

  bool waitOnDecision_{false};
};

TEST_F(FibTestFixture, processRouteDb) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  // Mimic decision pub sock publishing RouteDatabaseDelta
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = "node-1";
  routeDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  routeDbDelta.unicastRoutesToUpdate.emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  int64_t countAdd = mockFibHandler->getAddRoutesCount();
  // add routes
  mockFibHandler->waitForUpdateUnicastRoutes();

  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 0);

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  EXPECT_TRUE(checkEqualRoutes(routeDb, getRouteDb()));

  // Update routes
  routeDbDelta.unicastRoutesToUpdate.clear();
  countAdd = mockFibHandler->getAddRoutesCount();
  int64_t countDel = mockFibHandler->getDelRoutesCount();
  routeDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2}));
  routeDbDelta.unicastRoutesToUpdate.emplace_back(
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2}));
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  // syncFib debounce
  mockFibHandler->waitForUpdateUnicastRoutes();
  EXPECT_GT(mockFibHandler->getAddRoutesCount(), countAdd);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), countDel);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_TRUE(checkEqualRoutes(routeDb, getRouteDb()));

  // Update routes by removing some nextHop
  countAdd = mockFibHandler->getAddRoutesCount();
  routeDb.unicastRoutes.clear();
  routeDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix2, {path1_2_2, path1_2_3}));
  routeDb.unicastRoutes.emplace_back(createUnicastRoute(prefix3, {path1_3_2}));

  routeDbDelta.unicastRoutesToUpdate.clear();
  routeDbDelta.unicastRoutesToUpdate.emplace_back(
      createUnicastRoute(prefix2, {path1_2_2, path1_2_3}));
  routeDbDelta.unicastRoutesToUpdate.emplace_back(
      createUnicastRoute(prefix3, {path1_3_2}));
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();
  // syncFib debounce
  mockFibHandler->waitForUpdateUnicastRoutes();
  EXPECT_GT(mockFibHandler->getAddRoutesCount(), countAdd);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), countDel);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_TRUE(checkEqualRoutes(routeDb, getRouteDb()));
}

TEST_F(FibTestFixture, processInterfaceDb) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  // Mimic interface initially coming up
  thrift::InterfaceDatabase intfDb(
      FRAGILE,
      "node-1",
      {
          {
              path1_2_1.address.ifName.value(),
              thrift::InterfaceInfo(
                  FRAGILE,
                  true, // isUp
                  121, // ifIndex
                  {}, // v4Addrs: TO BE DEPRECATED SOON
                  {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                  {} // networks
                  ),
          },
          {
              path1_2_2.address.ifName.value(),
              thrift::InterfaceInfo(
                  FRAGILE,
                  true, // isUp
                  122, // ifIndex
                  {}, // v4Addrs: TO BE DEPRECATED SOON
                  {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                  {} // networks
                  ),
          },
      },
      thrift::PerfEvents());
  intfDb.perfEvents = folly::none;
  lmPub.sendThriftObj(intfDb, serializer).value();

  // Mimic decision pub sock publishing RouteDatabaseDelta
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}),
      createUnicastRoute(prefix1, {path1_2_1})};
  routeDbDelta.mplsRoutesToUpdate = {
      createMplsRoute(label2, {mpls_path1_2_1, mpls_path1_2_2}),
      createMplsRoute(label1, {mpls_path1_2_1})};
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();

  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 2);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 2);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);

  // Mimic interface going down
  thrift::InterfaceDatabase intfChange_1(
      FRAGILE,
      "node-1",
      {
          {
              path1_2_1.address.ifName.value(),
              thrift::InterfaceInfo(
                  FRAGILE,
                  false, // isUp
                  121, // ifIndex
                  {}, // v4Addrs: TO BE DEPRECATED SOON
                  {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                  {} // networks
                  ),
          },
      },
      thrift::PerfEvents());
  intfChange_1.perfEvents = folly::none;
  lmPub.sendThriftObj(intfChange_1, serializer).value();

  mockFibHandler->waitForDeleteUnicastRoutes();
  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForDeleteMplsRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 1);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);

  //
  // Send new route for prefix2 (see it gets updated right through)
  //
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix1, {path1_2_2})};
  routeDbDelta.mplsRoutesToUpdate = {createMplsRoute(label1, {mpls_path1_2_2})};
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 1);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 1);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);

  // Mimic interface going down
  // the route entry associated with the prefix shall be removed this time
  thrift::InterfaceDatabase intfChange_2(
      FRAGILE,
      "node-1",
      {
          {
              path1_2_2.address.ifName.value(),
              thrift::InterfaceInfo(
                  FRAGILE,
                  false, // isUp
                  122, // ifIndex
                  {}, // v4Addrs: TO BE DEPRECATED SOON
                  {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                  {} // networks
                  ),
          },
      },
      thrift::PerfEvents());
  intfChange_2.perfEvents = folly::none;
  lmPub.sendThriftObj(intfChange_2, serializer).value();

  mockFibHandler->waitForDeleteUnicastRoutes();
  mockFibHandler->waitForDeleteMplsRoutes();
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 3);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  //
  // Bring up both of these interfaces and verify that route appears back
  //
  lmPub.sendThriftObj(intfDb, serializer).value();

  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 6);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 6);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 3);
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
}

TEST_F(FibTestFixture, basicAddAndDelete) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  // Mimic decision pub sock publishing RouteDatabaseDelta
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix1, {path1_2_1, path1_2_2}),
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2})};
  routeDbDelta.mplsRoutesToUpdate = {
      createMplsRoute(label1, {mpls_path1_2_1, mpls_path1_2_2}),
      createMplsRoute(label2, {mpls_path1_2_2}),
      createMplsRoute(label3, {mpls_path1_2_1})};
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  // wait
  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();

  // verify routes
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 2);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 0);

  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 3);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 0);

  // delete one route
  routeDbDelta.unicastRoutesToUpdate.clear();
  routeDbDelta.mplsRoutesToUpdate.clear();
  routeDbDelta.unicastRoutesToDelete = {prefix3};
  routeDbDelta.mplsRoutesToDelete = {label1, label3};
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  mockFibHandler->waitForDeleteUnicastRoutes();
  mockFibHandler->waitForDeleteMplsRoutes();

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  EXPECT_EQ(routes.at(0).dest, prefix1);
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 2);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 1);

  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 1);
  EXPECT_EQ(mplsRoutes.at(0).topLabel, label2);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 2);

  // add back that route
  routeDbDelta.unicastRoutesToDelete.clear();
  routeDbDelta.mplsRoutesToDelete.clear();
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2})};
  routeDbDelta.mplsRoutesToUpdate = {
      createMplsRoute(label1, {mpls_path1_2_1, mpls_path1_2_2})};
  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->waitForUpdateMplsRoutes();

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 3);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 1);

  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 4);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 2);
}

TEST_F(FibTestFixture, fibRestart) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Mimic decision pub sock publishing RouteDatabaseDelta
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix1, {path1_2_1, path1_2_2})};
  routeDbDelta.mplsRoutesToUpdate = {
      createMplsRoute(label1, {mpls_path1_2_1, mpls_path1_2_2}),
      createMplsRoute(label2, {mpls_path1_2_2})};

  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);

  // Restart
  mockFibHandler->restart();

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 2);
}

class FibTestFixtureWaitOnDecision : public FibTestFixture {
 public:
  FibTestFixtureWaitOnDecision() : FibTestFixture(true) {}
};

TEST_F(FibTestFixtureWaitOnDecision, WaitOnDecision) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  std::vector<thrift::MplsRoute> mplsRoutes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);
  mockFibHandler->getMplsRouteTableByClient(mplsRoutes, kFibId);
  EXPECT_EQ(mplsRoutes.size(), 0);

  // Mimic decision pub sock publishing RouteDatabaseDelta
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  routeDbDelta.unicastRoutesToUpdate = {
      createUnicastRoute(prefix1, {path1_2_1, path1_2_2})};
  routeDbDelta.mplsRoutesToUpdate = {
      createMplsRoute(label1, {mpls_path1_2_1, mpls_path1_2_2}),
      createMplsRoute(label2, {mpls_path1_2_2})};

  decisionPub.sendThriftObj(routeDbDelta, serializer).value();

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();
  mockFibHandler->waitForSyncMplsFib();

  // ensure no other calls occured
  EXPECT_EQ(mockFibHandler->getFibSyncCount(), 1);
  EXPECT_EQ(mockFibHandler->getAddRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler->getDelRoutesCount(), 0);

  EXPECT_EQ(mockFibHandler->getFibMplsSyncCount(), 2);
  EXPECT_EQ(mockFibHandler->getAddMplsRoutesCount(), 0);
  EXPECT_EQ(mockFibHandler->getDelMplsRoutesCount(), 0);
}

TEST_F(FibTestFixture, getUnicastRoutesFilteredTest) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // initial syncFib debounce
  mockFibHandler->waitForSyncFib();

  const auto prefix1 = toIpPrefix("192.168.20.16/28");
  const auto prefix2 = toIpPrefix("192.168.0.0/16");
  const auto prefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto prefix4 = toIpPrefix("fd00::48:2:0/126");

  const auto route1 = createUnicastRoute(prefix1, {});
  const auto route2 = createUnicastRoute(prefix2, {});
  const auto route3 = createUnicastRoute(prefix3, {});
  const auto route4 = createUnicastRoute(prefix4, {});

  // add routes to DB and update DB
  thrift::RouteDatabaseDelta routeDb;
  routeDb.thisNodeName = "node-1";
  routeDb.unicastRoutesToUpdate.emplace_back(route1);
  routeDb.unicastRoutesToUpdate.emplace_back(route2);
  routeDb.unicastRoutesToUpdate.emplace_back(route3);
  routeDb.unicastRoutesToUpdate.emplace_back(route4);
  decisionPub.sendThriftObj(routeDb, serializer).value();
  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 4);

  // input filter prefix list
  auto filter =
      std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>({
          "192.168.20.16/28", // match prefix1
          "192.168.20.19", // match prefix1
          "192.168.0.0", // match prefix2
          "192.168.0.0/18", // match prefix2
          "10.46.8.0", // no match
          "fd00::48:2:0/127", // match prefix4
          "fd00::48:2:0/125" // no match
      }));

  // expected routesDB after filtering - delete duplicate entries
  thrift::RouteDatabase expectedDb;
  expectedDb.thisNodeName = "node-1";
  expectedDb.unicastRoutes.emplace_back(route1);
  expectedDb.unicastRoutes.emplace_back(route2);
  expectedDb.unicastRoutes.emplace_back(route4);
  // check if match correctly
  thrift::RouteDatabase responseDb;
  const auto& responseRoutes = getUnicastRoutesFiltered(std::move(filter));
  responseDb.unicastRoutes = responseRoutes;
  EXPECT_TRUE(checkEqualRoutes(expectedDb, responseDb));

  // check when get empty input - return all unicast routes
  thrift::RouteDatabase allRouteDb;
  allRouteDb.unicastRoutes.emplace_back(route1);
  allRouteDb.unicastRoutes.emplace_back(route2);
  allRouteDb.unicastRoutes.emplace_back(route3);
  allRouteDb.unicastRoutes.emplace_back(route4);
  auto emptyParamRet =
      std::unique_ptr<std::vector<std::string>>(new std::vector<std::string>());
  const auto& allRoutes = getUnicastRoutesFiltered(std::move(emptyParamRet));
  thrift::RouteDatabase allRoutesRespDb;
  allRoutesRespDb.unicastRoutes = allRoutes;
  EXPECT_TRUE(checkEqualRoutes(allRouteDb, allRoutesRespDb));

  // check getUnicastRoutes() API - return all unicast routes
  const auto& allRoute = getUnicastRoutes();
  thrift::RouteDatabase allRoutesApiDb;
  allRoutesApiDb.unicastRoutes = allRoute;
  EXPECT_TRUE(checkEqualRoutes(allRouteDb, allRoutesApiDb));

  // check when no result found
  auto notFoundFilter = std::unique_ptr<std::vector<std::string>>(
      new std::vector<std::string>({"10.46.8.0", "10.46.8.0/24"}));
  const auto& notFoundResp =
      getUnicastRoutesFiltered(std::move(notFoundFilter));
  EXPECT_EQ(notFoundResp.size(), 0);
}

TEST_F(FibTestFixture, longestPrefixMatchTest) {
  std::unordered_map<thrift::IpPrefix, thrift::UnicastRoute> unicastRoutes;
  const auto& dbPrefix1 = toIpPrefix("192.168.0.0/16");
  const auto& dbPrefix2 = toIpPrefix("192.168.0.0/20");
  const auto& dbPrefix3 = toIpPrefix("192.168.0.0/24");
  const auto& dbPrefix4 = toIpPrefix("192.168.20.16/28");
  unicastRoutes[dbPrefix1] = createUnicastRoute(dbPrefix1, {});
  unicastRoutes[dbPrefix2] = createUnicastRoute(dbPrefix2, {});
  unicastRoutes[dbPrefix3] = createUnicastRoute(dbPrefix3, {});
  unicastRoutes[dbPrefix4] = createUnicastRoute(dbPrefix4, {});

  const auto inputPrefix1 =
      folly::IPAddress::tryCreateNetwork("192.168.20.19").value();
  const auto inputPrefix2 =
      folly::IPAddress::tryCreateNetwork("192.168.20.16/28").value();
  const auto inputPrefix3 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0").value();
  const auto inputPrefix4 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/14").value();
  const auto inputPrefix5 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/18").value();
  const auto inputPrefix6 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/22").value();
  const auto inputPrefix7 =
      folly::IPAddress::tryCreateNetwork("192.168.0.0/26").value();

  // input 192.168.20.19 matched 192.168.20.16/28
  const auto& result1 = Fib::longestPrefixMatch(inputPrefix1, unicastRoutes);
  EXPECT_TRUE(result1.has_value());
  EXPECT_EQ(result1.value(), dbPrefix4);

  // input 192.168.20.16/28 matched 192.168.20.16/28
  const auto& result2 = Fib::longestPrefixMatch(inputPrefix2, unicastRoutes);
  EXPECT_TRUE(result2.has_value());
  EXPECT_EQ(result2.value(), dbPrefix4);

  // input 192.168.0.0 matched 192.168.0.0/24
  const auto& result3 = Fib::longestPrefixMatch(inputPrefix3, unicastRoutes);
  EXPECT_TRUE(result3.has_value());
  EXPECT_EQ(result3.value(), dbPrefix3);
  //
  // input 192.168.0.0/14 has no match
  const auto& result4 = Fib::longestPrefixMatch(inputPrefix4, unicastRoutes);
  EXPECT_TRUE(not result4.has_value());

  // input 192.168.0.0/18 matched 192.168.0.0/16
  const auto& result5 = Fib::longestPrefixMatch(inputPrefix5, unicastRoutes);
  EXPECT_TRUE(result5.has_value());
  EXPECT_EQ(result5.value(), dbPrefix1);

  // input 192.168.0.0/22 matched 192.168.0.0/20
  const auto& result6 = Fib::longestPrefixMatch(inputPrefix6, unicastRoutes);
  EXPECT_TRUE(result6.has_value());
  EXPECT_EQ(result6.value(), dbPrefix2);

  // input 192.168.0.0/26 matched 192.168.0.0/24
  const auto& result7 = Fib::longestPrefixMatch(inputPrefix7, unicastRoutes);
  EXPECT_TRUE(result7.has_value());
  EXPECT_EQ(result7.value(), dbPrefix3);
}

TEST_F(FibTestFixture, doNotInstall) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  const auto prefix1 = toIpPrefix("192.168.20.16/28");
  const auto prefix2 = toIpPrefix("192.168.0.0/16");
  const auto prefix3 = toIpPrefix("fd00::48:2:0/128");
  const auto prefix4 = toIpPrefix("fd00::48:2:0/126");

  auto route1 = createUnicastRoute(prefix1, {});
  auto route2 = createUnicastRoute(prefix2, {});
  auto route3 = createUnicastRoute(prefix3, {});
  auto route4 = createUnicastRoute(prefix4, {});

  route1.doNotInstall = true;
  route3.doNotInstall = true;

  // add routes to DB and update DB
  {
    thrift::RouteDatabaseDelta routeDb;
    routeDb.thisNodeName = "node-1";
    routeDb.unicastRoutesToUpdate.emplace_back(route1);
    routeDb.unicastRoutesToUpdate.emplace_back(route2);
    decisionPub.sendThriftObj(routeDb, serializer).value();
  }
  mockFibHandler->waitForSyncFib();
  mockFibHandler->getRouteTableByClient(routes, kFibId);

  // only 1 route is installable
  EXPECT_EQ(routes.size(), 1);

  // add routes to DB and update DB
  {
    thrift::RouteDatabaseDelta routeDb;
    routeDb.thisNodeName = "node-1";
    routeDb.unicastRoutesToUpdate.emplace_back(route3);
    routeDb.unicastRoutesToUpdate.emplace_back(route4);
    decisionPub.sendThriftObj(routeDb, serializer).value();
  }

  mockFibHandler->waitForUpdateUnicastRoutes();
  mockFibHandler->getRouteTableByClient(routes, kFibId);

  // now 2 routes are installable
  EXPECT_EQ(routes.size(), 2);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
