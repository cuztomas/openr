/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkSystemHandler.h"

#include <algorithm>
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
#include <re2/re2.h>
#include <re2/set.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/tests/OpenrThriftServerWrapper.h>

using namespace std;
using namespace openr;

using apache::thrift::FRAGILE;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;
using ::testing::InSequence;

// node-1 connects node-2 via interface iface_2_1 and iface_2_2, node-3 via
// interface iface_3_1
namespace {

re2::RE2::Options regexOpts;

const auto peerSpec_2_1 = thrift::PeerSpec(
    FRAGILE,
    "tcp://[fe80::2%iface_2_1]:10001",
    "tcp://[fe80::2%iface_2_1]:10002",
    false);

const auto peerSpec_2_2 = thrift::PeerSpec(
    FRAGILE,
    "tcp://[fe80::2%iface_2_2]:10001",
    "tcp://[fe80::2%iface_2_2]:10002",
    false);

const auto nb2 = thrift::SparkNeighbor(
    FRAGILE,
    "domain",
    "node-2",
    0, /* hold time */
    "", /* DEPRECATED - public key */
    toBinaryAddress(folly::IPAddress("fe80::2")),
    toBinaryAddress(folly::IPAddress("192.168.0.2")),
    10001,
    10002,
    "" /* ifName */);

const auto nb3 = thrift::SparkNeighbor(
    FRAGILE,
    "domain",
    "node-3",
    0, /* hold time */
    "", /* DEPRECATED - public key */
    toBinaryAddress(folly::IPAddress("fe80::3")),
    toBinaryAddress(folly::IPAddress("192.168.0.3")),
    10001,
    10002,
    "" /* ifName */);

const uint64_t kTimestamp{1000000};
const uint64_t kNodeLabel{0};

const auto adj_2_1 = createThriftAdjacency(
    "node-2",
    "iface_2_1",
    "fe80::2",
    "192.168.0.2",
    1 /* metric */,
    1 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */,
    folly::none);

const auto adj_2_2 = createThriftAdjacency(
    "node-2",
    "iface_2_2",
    "fe80::2",
    "192.168.0.2",
    1 /* metric */,
    2 /* label */,
    false /* overload-bit */,
    0 /* rtt */,
    kTimestamp /* timestamp */,
    Constants::kDefaultAdjWeight /* weight */,
    "" /* otherIfName */,
    folly::none);

const auto staticPrefix1 = toIpPrefix("fc00:face:b00c::/64");
const auto staticPrefix2 = toIpPrefix("fc00:cafe:babe::/64");

thrift::SparkNeighborEvent
createNeighborEvent(
    thrift::SparkNeighborEventType eventType,
    const std::string& ifName,
    const thrift::SparkNeighbor& neighbor,
    int64_t rttUs,
    int32_t label) {
  return createSparkNeighborEvent(
      eventType, ifName, neighbor, rttUs, label, false, folly::none);
}

thrift::AdjacencyDatabase
createAdjDatabase(
    const std::string& thisNodeName,
    const std::vector<thrift::Adjacency>& adjacencies,
    int32_t nodeLabel) {
  return createAdjDb(thisNodeName, adjacencies, nodeLabel);
}

void
printAdjDb(const thrift::AdjacencyDatabase& adjDb) {
  LOG(INFO) << "Node: " << adjDb.thisNodeName
            << ", Overloaded: " << adjDb.isOverloaded
            << ", Label: " << adjDb.nodeLabel;
  for (auto const& adj : adjDb.adjacencies) {
    LOG(INFO) << "  " << adj.otherNodeName << "@" << adj.ifName
              << ", metric: " << adj.metric << ", label: " << adj.adjLabel
              << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt
              << ", ts: " << adj.timestamp << ", " << toString(adj.nextHopV4)
              << ", " << toString(adj.nextHopV6);
  }
}

const std::string kTestVethNamePrefix = "vethLMTest";
const std::vector<uint64_t> kTestVethIfIndex = {1240, 1241, 1242};
const std::string kConfigStorePath = "/tmp/lm_ut_config_store.bin";
} // namespace

class LinkMonitorTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // Cleanup any existing config file on disk
    system(folly::sformat("rm -rf {}", kConfigStorePath).c_str());

    mockNlHandler = std::make_shared<MockNetlinkSystemHandler>(
        context, "inproc://platform-pub-url");

    server = make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockNlHandler);

    systemThriftThread.start(server);
    port = systemThriftThread.getAddress()->getPort();

    // spin up a config store
    configStore = std::make_unique<PersistentStore>(
        "1",
        kConfigStorePath,
        context,
        Constants::kPersistentStoreInitialBackoff,
        Constants::kPersistentStoreMaxBackoff,
        true /* dryrun */);
    kConfigStoreUrl = configStore->inprocCmdUrl;

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(
        context,
        "test_store1",
        std::chrono::seconds(1) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    // create prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        "node-1",
        PersistentStoreUrl{kConfigStoreUrl},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        false /* create IP prefix keys */,
        false,
        std::chrono::seconds(0),
        Constants::kKvStoreDbTtl,
        context);
    prefixManagerThread = std::make_unique<std::thread>([this] {
      LOG(INFO) << "prefix manager starting";
      prefixManager->run();
      LOG(INFO) << "prefix manager stopped";
    });

    // spark reports neighbor up
    EXPECT_NO_THROW(
        sparkReport.bind(fbzmq::SocketUrl{"inproc://spark-report"}).value());

    // spark responses to if events
    EXPECT_NO_THROW(
        sparkIfDbResp.bind(fbzmq::SocketUrl{"inproc://spark-req"}).value());

    regexOpts.set_case_sensitive(false);
    std::string regexErr;
    auto includeRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
    includeRegexList->Add("iface.*", &regexErr);
    includeRegexList->Compile();

    std::unique_ptr<re2::RE2::Set> excludeRegexList;

    auto redistRegexList =
        std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
    redistRegexList->Add("loopback", &regexErr);
    redistRegexList->Compile();

    // start a link monitor
    linkMonitor = make_shared<LinkMonitor>(
        context,
        "node-1",
        port, /* thrift service port */
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList), // redistribute interface name
        std::vector<thrift::IpPrefix>{staticPrefix1, staticPrefix2},
        false /* useRttMetric */,
        false /* enable perf measurement */,
        true /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type mpls */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{"adj:"},
        SparkCmdUrl{"inproc://spark-req"},
        SparkReportUrl{"inproc://spark-report"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        PersistentStoreUrl{kConfigStoreUrl},
        false,
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url"},
        std::chrono::seconds(1),
        // link flap backoffs, set low to keep UT runtime low
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);

    linkMonitorThread = std::make_unique<std::thread>([this]() {
      folly::setThreadName("LinkMonitor");
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitor->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitor->waitUntilRunning();

    // spin up an openrThriftServer
    openrThriftServerWrapper_ = std::make_shared<OpenrThriftServerWrapper>(
        "node-1",
        MonitorSubmitUrl{"inproc://monitor-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        context);
    openrThriftServerWrapper_->addModuleType(
        thrift::OpenrModuleType::LINK_MONITOR, linkMonitor);
    openrThriftServerWrapper_->run();
  }

  void
  TearDown() override {
    LOG(INFO) << "LinkMonitor test/basic operations is done";

    LOG(INFO) << "Stopping openr-ctrl thrift server";
    openrThriftServerWrapper_->stop();
    LOG(INFO) << "Openr-ctrl thrift server got stopped";

    LOG(INFO) << "Stopping the LinkMonitor thread";
    linkMonitor->stop();
    linkMonitorThread->join();
    linkMonitor.reset();
    LOG(INFO) << "LinkMonitor thread got stopped";

    LOG(INFO) << "Closing sockets";
    sparkReport.close();
    sparkIfDbResp.close();

    LOG(INFO) << "Stopping prefix manager thread";
    prefixManager->stop();
    prefixManagerThread->join();

    // Erase data from config store
    LOG(INFO) << "Erasing data from config store";
    PersistentStoreClient configStoreClient{PersistentStoreUrl{kConfigStoreUrl},
                                            context};
    configStoreClient.erase("link-monitor-config");

    // stop config store
    LOG(INFO) << "Stopping config store";
    configStore->stop();
    configStoreThread->join();

    // stop the kvStore
    LOG(INFO) << "Stopping KvStoreWrapper";
    kvStoreWrapper->stop();
    LOG(INFO) << "KvStoreWrapper got stopped";

    // stop mocked nl platform
    LOG(INFO) << "Stopping mocked thrift handlers";
    mockNlHandler->stop();
    systemThriftThread.stop();
    LOG(INFO) << "Mocked thrift handlers got stopped";
  }

  // emulate spark keeping receiving InterfaceDb until no more udpates
  // and update sparkIfDb for every update received
  // return number of updates received
  int
  recvAndReplyIfUpdate(std::chrono::seconds timeout = std::chrono::seconds(2)) {
    int numUpdateRecv = 0;
    while (true) {
      auto ifDb = sparkIfDbResp.recvThriftObj<thrift::InterfaceDatabase>(
          serializer, std::chrono::milliseconds{timeout});
      if (ifDb.hasError()) {
        return numUpdateRecv;
      }
      sendIfDbResp(true);
      sparkIfDb = std::move(ifDb.value().interfaces);
      ++numUpdateRecv;
    }
  }

  // collate Interfaces into a map so UT can run some checks
  using CollatedIfData = struct {
    int isUpCount{0};
    int isDownCount{0};
    int v4AddrsMaxCount{0};
    int v4AddrsMinCount{0};
    int v6LinkLocalAddrsMaxCount{0};
    int v6LinkLocalAddrsMinCount{0};
  };
  using CollatedIfUpdates = std::map<string, CollatedIfData>;

  CollatedIfUpdates
  collateIfUpdates(
      const std::map<std::string, thrift::InterfaceInfo>& interfaces) {
    CollatedIfUpdates res;
    for (const auto& kv : interfaces) {
      const auto& ifName = kv.first;
      if (kv.second.isUp) {
        res[ifName].isUpCount++;
      } else {
        res[ifName].isDownCount++;
      }
      int v4AddrsCount = 0;
      int v6LinkLocalAddrsCount = 0;
      for (const auto& network : kv.second.networks) {
        const auto& ipNetwork = toIPNetwork(network);
        if (ipNetwork.first.isV4()) {
          v4AddrsCount++;
        } else if (ipNetwork.first.isV6() && ipNetwork.first.isLinkLocal()) {
          v6LinkLocalAddrsCount++;
        }
      }

      res[ifName].v4AddrsMaxCount =
          max(v4AddrsCount, res[ifName].v4AddrsMaxCount);

      res[ifName].v4AddrsMinCount =
          min(v4AddrsCount, res[ifName].v4AddrsMinCount);

      res[ifName].v6LinkLocalAddrsMaxCount =
          max(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMaxCount);

      res[ifName].v6LinkLocalAddrsMinCount =
          min(v6LinkLocalAddrsCount, res[ifName].v6LinkLocalAddrsMinCount);
    }
    return res;
  }

  // emulate spark sending a response for IfDb
  void
  sendIfDbResp(bool isSuccess) {
    thrift::SparkIfDbUpdateResult result;
    result.isSuccess = isSuccess;
    sparkIfDbResp.sendThriftObj(result, serializer);
  }

  folly::Optional<thrift::Value>
  getPublicationValueForKey(
      std::string const& key,
      std::chrono::seconds timeout = std::chrono::seconds(10)) {
    VLOG(1) << "Waiting to receive publication for key " << key;
    auto pub = kvStoreWrapper->recvPublication(timeout);

    VLOG(1) << "Received publication with keys";
    for (auto const& kv : pub.keyVals) {
      VLOG(1) << "  " << kv.first;
    }

    auto kv = pub.keyVals.find(key);
    if (kv == pub.keyVals.end() or !kv->second.value) {
      return folly::none;
    }

    return kv->second;
  }

  // recv publicatons from kv store until we get what we were
  // expecting for a given key
  void
  checkNextAdjPub(std::string const& key) {
    CHECK(!expectedAdjDbs.empty());

    printAdjDb(expectedAdjDbs.front());

    while (true) {
      folly::Optional<thrift::Value> value;
      try {
        value = getPublicationValueForKey(key);
        if (not value.hasValue()) {
          continue;
        }
      } catch (std::exception const& e) {
        LOG(ERROR) << "Exception: " << folly::exceptionStr(e);
        EXPECT_TRUE(false);
        return;
      }

      auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
          value->value.value(), serializer);
      printAdjDb(adjDb);

      // we can not know what the nodeLabel will be
      adjDb.nodeLabel = kNodeLabel;
      // nor the timestamp, so we override with our predefinded const values
      for (auto& adj : adjDb.adjacencies) {
        adj.timestamp = kTimestamp;
      }
      auto adjDbstr = fbzmq::util::writeThriftObjStr(adjDb, serializer);
      auto expectedAdjDbstr =
          fbzmq::util::writeThriftObjStr(expectedAdjDbs.front(), serializer);
      if (adjDbstr == expectedAdjDbstr) {
        expectedAdjDbs.pop();
        return;
      }
    }
  }

  // kvstore shall reveive cmd to add/del peers
  void
  checkPeerDump(std::string const& nodeName, thrift::PeerSpec peerSpec) {
    auto const peers = kvStoreWrapper->getPeers();
    EXPECT_EQ(peers.count(nodeName), 1);
    if (!peers.count(nodeName)) {
      return;
    }
    EXPECT_EQ(peers.at(nodeName).pubUrl, peerSpec.pubUrl);
    EXPECT_EQ(peers.at(nodeName).cmdUrl, peerSpec.cmdUrl);
  }

  std::unordered_set<thrift::IpPrefix>
  getNextPrefixDb(std::string const& key) {
    while (true) {
      auto value = getPublicationValueForKey(key, std::chrono::seconds(3));
      if (not value.hasValue()) {
        continue;
      }

      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value->value.value(), serializer);
      std::unordered_set<thrift::IpPrefix> prefixes;
      for (auto const& prefixEntry : prefixDb.prefixEntries) {
        prefixes.insert(prefixEntry.prefix);
      }
      return prefixes;
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread systemThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> sparkReport{context};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> sparkIfDbResp{context};

  unique_ptr<PersistentStore> configStore;
  unique_ptr<std::thread> configStoreThread;
  std::string kConfigStoreUrl;

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<LinkMonitor> linkMonitor;
  std::unique_ptr<std::thread> linkMonitorThread;

  std::unique_ptr<PrefixManager> prefixManager;
  std::unique_ptr<std::thread> prefixManagerThread;

  std::shared_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<MockNetlinkSystemHandler> mockNlHandler;

  std::queue<thrift::AdjacencyDatabase> expectedAdjDbs;
  std::map<std::string, thrift::InterfaceInfo> sparkIfDb;

  // thriftServer to talk to LinkMonitor
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
};

// Start LinkMonitor and ensure empty adjacency database and prefixes are
// received upon initial hold-timeout expiry
TEST_F(LinkMonitorTestFixture, NoNeighborEvent) {
  // Verify that we receive empty adjacency database
  expectedAdjDbs.push(createAdjDatabase("node-1", {}, kNodeLabel));
  checkNextAdjPub("adj:node-1");
}

// receive neighbor up/down events from "spark"
// form peer connections and inform KvStore of adjacencies
TEST_F(LinkMonitorTestFixture, BasicOperation) {
  const int linkMetric = 123;
  const int adjMetric = 100;
  std::string clientId = Constants::kSparkReportClientId.toString();

  {
    InSequence dummy;

    {
      // create an interface
      mockNlHandler->sendLinkEvent("iface_2_1", 100, true);
      recvAndReplyIfUpdate();
      thrift::InterfaceDatabase intfDb(
          FRAGILE,
          "node-1",
          {
              {
                  "iface_2_1",
                  thrift::InterfaceInfo(
                      FRAGILE,
                      true, // isUp
                      100, // ifIndex
                      {}, // v4Addrs: TO BE DEPRECATED SOON
                      {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
                      {} // networks
                      ),
              },
          },
          thrift::PerfEvents());
      intfDb.perfEvents = folly::none;
    }

    {
      // expect neighbor up first
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit set
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;
      adj_2_1_modified.isOverloaded = true;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link overloaded bit unset
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value unset
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect node overload bit set
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect link metric value override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // set adjacency metric, this should override the link metric
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = adjMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // unset adjacency metric, this will remove the override
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // link-monitor and config-store is restarted but state will be
      // retained. expect neighbor up with overrides
      auto adj_2_1_modified = adj_2_1;
      adj_2_1_modified.metric = linkMetric;

      auto adjDb = createAdjDatabase("node-1", {adj_2_1_modified}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }

    {
      // expect neighbor down
      auto adjDb = createAdjDatabase("node-1", {}, kNodeLabel);
      adjDb.isOverloaded = true;
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    EXPECT_NO_THROW(sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value()));
    LOG(INFO) << "Testing neighbor UP event!";
    checkNextAdjPub("adj:node-1");
  }

  // testing for set/unset overload bit and custom metric values
  // 1. set overload bit
  // 2. set custom metric on link: No adjacency advertisement
  // 3. set link overload
  // 4. unset overload bit
  // 5. unset link overload bit - custom metric value should be in effect
  // 6. unset custom metric on link
  // 7: set overload bit
  // 8: custom metric on link
  // 9. Set overload bit and link metric value
  // 10. set and unset adjacency metric
  {
    auto openrCtrlHanlder = openrThriftServerWrapper_->getOpenrCtrlHandler();
    const std::string interfaceName = "iface_2_1";
    const std::string nodeName = "node-2";

    LOG(INFO) << "Testing set node overload command!";
    auto ret = openrCtrlHanlder->semifuture_setNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    auto res = openrCtrlHanlder->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(res->isOverloaded);
    EXPECT_EQ(1, res->interfaceDetails.size());
    EXPECT_FALSE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_FALSE(
        res->interfaceDetails.at(interfaceName).metricOverride.hasValue());

    LOG(INFO) << "Testing set link metric command!";
    ret = openrCtrlHanlder
              ->semifuture_setInterfaceMetric(
                  std::make_unique<std::string>(interfaceName), linkMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link overload command!";
    ret = openrCtrlHanlder
              ->semifuture_setInterfaceOverload(
                  std::make_unique<std::string>(interfaceName))
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = openrCtrlHanlder->semifuture_getInterfaces().get();
    ASSERT_NE(nullptr, res);
    EXPECT_TRUE(res->isOverloaded);
    EXPECT_TRUE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails.at(interfaceName).metricOverride.value());

    LOG(INFO) << "Testing unset node overload command!";
    ret = openrCtrlHanlder->semifuture_unsetNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
    res = openrCtrlHanlder->semifuture_getInterfaces().get();
    EXPECT_FALSE(res->isOverloaded);
    EXPECT_TRUE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_EQ(
        linkMetric,
        res->interfaceDetails.at(interfaceName).metricOverride.value());

    LOG(INFO) << "Testing unset link overload command!";
    ret = openrCtrlHanlder
              ->semifuture_unsetInterfaceOverload(
                  std::make_unique<std::string>(interfaceName))
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset link metric command!";
    ret = openrCtrlHanlder
              ->semifuture_unsetInterfaceMetric(
                  std::make_unique<std::string>(interfaceName))
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    res = openrCtrlHanlder->semifuture_getInterfaces().get();
    EXPECT_FALSE(res->isOverloaded);
    EXPECT_FALSE(res->interfaceDetails.at(interfaceName).isOverloaded);
    EXPECT_FALSE(
        res->interfaceDetails.at(interfaceName).metricOverride.hasValue());

    LOG(INFO) << "Testing set node overload command( AGAIN )!";
    ret = openrCtrlHanlder->semifuture_setNodeOverload().get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set link metric command( AGAIN )!";
    ret = openrCtrlHanlder
              ->semifuture_setInterfaceMetric(
                  std::make_unique<std::string>(interfaceName), linkMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing set adj metric command!";
    ret = openrCtrlHanlder
              ->semifuture_setAdjacencyMetric(
                  std::make_unique<std::string>(interfaceName),
                  std::make_unique<std::string>(nodeName),
                  adjMetric)
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");

    LOG(INFO) << "Testing unset adj metric command!";
    ret = openrCtrlHanlder
              ->semifuture_unsetAdjacencyMetric(
                  std::make_unique<std::string>(interfaceName),
                  std::make_unique<std::string>(nodeName))
              .get();
    EXPECT_TRUE(folly::Unit() == ret);
    checkNextAdjPub("adj:node-1");
  }

  // 11. neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }

  // mock "restarting" link monitor with existing config store

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // stop linkMonitor and openr-ctrl-server
  LOG(INFO) << "Mock restarting link monitor!";
  openrThriftServerWrapper_->stop();

  linkMonitor->stop();
  linkMonitorThread->join();
  linkMonitor.reset();

  linkMonitor = make_shared<LinkMonitor>(
      context,
      "node-1",
      port, // platform pub port
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      std::move(includeRegexList),
      std::move(excludeRegexList),
      // redistribute interface names
      std::move(redistRegexList),
      std::vector<thrift::IpPrefix>{}, // static prefixes
      false /* useRttMetric */,
      false /* enable perf measurement */,
      false /* enable v4 */,
      true /* enable segment routing */,
      false /* prefix type MPLS */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      AdjacencyDbMarker{"adj:"},
      SparkCmdUrl{"inproc://spark-req2"},
      SparkReportUrl{"inproc://spark-report2"},
      MonitorSubmitUrl{"inproc://monitor-rep2"},
      PersistentStoreUrl{kConfigStoreUrl}, /* same config store */
      false,
      PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
      PlatformPublisherUrl{"inproc://platform-pub-url2"},
      LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url2"},
      std::chrono::seconds(1),
      // link flap backoffs, set low to keep UT runtime low
      std::chrono::milliseconds(1),
      std::chrono::milliseconds(8),
      Constants::kKvStoreDbTtl);

  linkMonitorThread = std::make_unique<std::thread>([this]() {
    LOG(INFO) << "LinkMonitor thread starting";
    linkMonitor->run();
    LOG(INFO) << "LinkMonitor thread finishing";
  });
  linkMonitor->waitUntilRunning();

  openrThriftServerWrapper_->addModuleType(
      thrift::OpenrModuleType::LINK_MONITOR, linkMonitor);
  openrThriftServerWrapper_->run();

  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> sparkReport{
      context,
      fbzmq::IdentityString{"spark_server_id"},
      folly::none,
      fbzmq::NonblockingFlag{true}};
  EXPECT_NO_THROW(
      sparkReport.bind(fbzmq::SocketUrl{"inproc://spark-report2"}).value());

  // 12. neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
    LOG(INFO) << "Testing neighbor up event!";
    checkNextAdjPub("adj:node-1");
  }

  // 13. neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
    LOG(INFO) << "Testing neighbor down event!";
    checkNextAdjPub("adj:node-1");
  }
}

// Test throttling
TEST_F(LinkMonitorTestFixture, Throttle) {
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }
  std::string clientId = Constants::kSparkReportClientId.toString();

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  // before throttled function kicks in

  // another neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  // neighbor 3 down immediately
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_3_1",
        nb3,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  checkNextAdjPub("adj:node-1");
}

// parallel adjacencies between two nodes via different interfaces
TEST_F(LinkMonitorTestFixture, ParallelAdj) {
  std::string clientId = Constants::kSparkReportClientId.toString();
  {
    InSequence dummy;

    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor up on another interface
    // still use iface_2_1 because it's the "min" and will not call addPeers

    {
      // note: adj_2_2 is hashed ahead of adj_2_1
      auto adjDb = createAdjDatabase("node-1", {adj_2_2, adj_2_1}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }

    // neighbor down
    {
      auto adjDb = createAdjDatabase("node-1", {adj_2_2}, kNodeLabel);
      expectedAdjDbs.push(std::move(adjDb));
    }
  }

  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor down
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_DOWN,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  checkNextAdjPub("adj:node-1");
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_2.otherNodeName, peerSpec_2_2);
}

// Verify neighbor-restarting event (including parallel case)
TEST_F(LinkMonitorTestFixture, NeighborRestart) {
  std::string clientId = Constants::kSparkReportClientId.toString();

  /* Single link case */
  // neighbor up
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restart
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());

  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  /* Parallel case */
  // neighbor up on another interface
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_UP,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        2 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }

  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting on iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_2.otherNodeName, peerSpec_2_2);

  // neighbor restarted
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTED,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting iface_2_2
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_2",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  checkPeerDump(adj_2_1.otherNodeName, peerSpec_2_1);

  // neighbor restarting iface_2_1
  {
    auto neighborEvent = createNeighborEvent(
        thrift::SparkNeighborEventType::NEIGHBOR_RESTARTING,
        "iface_2_1",
        nb2,
        100 /* rtt-us */,
        1 /* label */);
    sparkReport.sendMultiple(
        fbzmq::Message::from(clientId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(neighborEvent, serializer).value());
  }
  // wait for this peer change to propogate
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  // peers should be gone
  EXPECT_TRUE(kvStoreWrapper->getPeers().empty());
}

TEST_F(LinkMonitorTestFixture, DampenLinkFlaps) {
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  // we want much higher backoffs for this test, so lets spin up a different LM
  openrThriftServerWrapper_->stop();

  linkMonitor->stop();
  linkMonitorThread->join();
  linkMonitor.reset();

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Add("iface.*", &regexErr);
  includeRegexList->Compile();

  std::unique_ptr<re2::RE2::Set> excludeRegexList;

  auto redistRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  redistRegexList->Add("loopback", &regexErr);
  redistRegexList->Compile();

  linkMonitor = make_shared<LinkMonitor>(
      context,
      "node-1",
      port, // platform pub port
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      std::move(includeRegexList),
      std::move(excludeRegexList),
      std::move(redistRegexList), // redistribute interface name
      std::vector<thrift::IpPrefix>{staticPrefix1, staticPrefix2},
      false /* useRttMetric */,
      false /* enable perf measurement */,
      true /* enable v4 */,
      true /* enable segment routing */,
      false /* prefix type MPLS */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      AdjacencyDbMarker{"adj:"},
      SparkCmdUrl{"inproc://spark-req"},
      SparkReportUrl{"inproc://spark-report"},
      MonitorSubmitUrl{"inproc://monitor-rep"},
      PersistentStoreUrl{kConfigStoreUrl},
      false,
      PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
      PlatformPublisherUrl{"inproc://platform-pub-url"},
      LinkMonitorGlobalPubUrl{"inproc://link-monitor-pub-url2"},
      std::chrono::seconds(1),
      // link flap backoffs, set high backoffs for this test
      std::chrono::milliseconds(4000),
      std::chrono::milliseconds(8000),
      Constants::kKvStoreDbTtl);

  linkMonitorThread = std::make_unique<std::thread>([this]() {
    LOG(INFO) << "LinkMonitor thread starting";
    linkMonitor->run();
    LOG(INFO) << "LinkMonitor thread finishing";
  });
  linkMonitor->waitUntilRunning();

  openrThriftServerWrapper_->addModuleType(
      thrift::OpenrModuleType::LINK_MONITOR, linkMonitor);
  openrThriftServerWrapper_->run();

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }

  VLOG(2) << "*** start link flaps ***";

  // Bringing up the interface
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);

  // at this point, both interface should have no backoff
  auto openrCtrlHandler = openrThriftServerWrapper_->getOpenrCtrlHandler();
  auto links = openrCtrlHandler->semifuture_getInterfaces().get();
  EXPECT_EQ(2, links->interfaceDetails.size());
  for (const auto& ifName : ifNames) {
    EXPECT_FALSE(
        links->interfaceDetails.at(ifName).linkFlapBackOffMs.hasValue());
  }

  VLOG(2) << "*** bring down 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);

  // at this point, both interface should have backoff=~1s
  // (1s of debounce + 2s of recvAndReplyIfUpdate)
  {
    // we expect all interfaces are down at this point because backoff hasn't
    // been cleared up yet
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);
    auto links1 = openrCtrlHandler->semifuture_getInterfaces().get();
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links1->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_GE(
          links1->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 0);
      EXPECT_LE(
          links1->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 4000);
    }
  }

  // expect spark to receive updates max in 4 seconds since first flap. We
  // already spent 2s in recvAndReplyIfUpdate before.
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  {
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      // Both report UP twice (link up and addr add)
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      // We get the link local notification
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
  VLOG(2) << "*** end link flaps ***";

  // Bringing down the interfaces
  VLOG(2) << "*** bring down 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  // at this point, both interface should have backoff=3sec

  {
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);
    auto links2 = openrCtrlHandler->semifuture_getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links2->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_GE(
          links2->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 4000);
      EXPECT_LE(
          links2->interfaceDetails.at(ifName).linkFlapBackOffMs.value(), 8000);
    }
  }

  // Bringing up the interfaces
  VLOG(2) << "*** bring up 2 interfaces ***";
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);

  // at this point, both interface should have backoff back to init value

  {
    // expect sparkIfDb to have two interfaces UP
    // Make sure to wait long enough to clear out backoff timers
    recvAndReplyIfUpdate(std::chrono::seconds(8));
    auto res = collateIfUpdates(sparkIfDb);
    auto links3 = openrCtrlHandler->semifuture_getInterfaces().get();

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    EXPECT_EQ(2, links3->interfaceDetails.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
      EXPECT_FALSE(
          links3->interfaceDetails.at(ifName).linkFlapBackOffMs.hasValue());
    }
  }
}

// Test Interface events to Spark
TEST_F(LinkMonitorTestFixture, verifyLinkEventSubscription) {
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate a link up event publication
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });
}

TEST_F(LinkMonitorTestFixture, verifyAddrEventSubscription) {
  const std::string linkX = kTestVethNamePrefix + "X";
  const std::string linkY = kTestVethNamePrefix + "Y";
  const std::set<std::string> ifNames = {linkX, linkY};

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);

  // Both interfaces report as down on creation
  // We receive 2 IfUpUpdates in spark for each interface
  // Both with status as false (DOWN)
  // We let spark return success for each
  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate add address event: v4 while interfaces are down. No addr events
  // should be reported.
  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", true /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      true /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      true /* is up */);

  // Emulate add address event: v6 while interfaces are in UP state. Both
  // v4 and v6 addresses should be reported.
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", true /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", true /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(1, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v4
  mockNlHandler->sendAddrEvent(linkX, "10.0.0.1/31", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "10.0.0.2/31", false /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(1, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate delete address event: v6
  mockNlHandler->sendAddrEvent(linkX, "fe80::1/128", false /* is valid */);
  mockNlHandler->sendAddrEvent(linkY, "fe80::2/128", false /* is valid */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 2 interfaces
    EXPECT_EQ(2, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(1, res.at(ifName).isUpCount);
      EXPECT_EQ(0, res.at(ifName).isDownCount);

      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);

      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  });

  // Emulate address and link events coming in out of order
  const std::string linkZ = kTestVethNamePrefix + "Z";

  // Addr event comes in first
  mockNlHandler->sendAddrEvent(linkZ, "fe80::3/128", true /* is valid */);
  // Link event comes in later
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      true /* is up */);

  EXPECT_NO_THROW({
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    EXPECT_EQ(1, res.at(linkZ).isUpCount);
    EXPECT_EQ(0, res.at(linkZ).isDownCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v4AddrsMinCount);
    EXPECT_EQ(1, res.at(linkZ).v6LinkLocalAddrsMaxCount);
    EXPECT_EQ(0, res.at(linkZ).v6LinkLocalAddrsMinCount);
  });

  // Link goes down
  mockNlHandler->sendLinkEvent(
      linkX /* link name */,
      kTestVethIfIndex[0] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkY /* link name */,
      kTestVethIfIndex[1] /* ifIndex */,
      false /* is up */);
  mockNlHandler->sendLinkEvent(
      linkZ /* link name */,
      kTestVethIfIndex[2] /* ifIndex */,
      false /* is up */);
  {
    // Both interfaces report as down on creation
    // expect sparkIfDb to have two interfaces DOWN
    recvAndReplyIfUpdate();
    auto res = collateIfUpdates(sparkIfDb);

    // messages for 3 interfaces
    EXPECT_EQ(3, res.size());
    for (const auto& ifName : ifNames) {
      EXPECT_EQ(0, res.at(ifName).isUpCount);
      EXPECT_EQ(1, res.at(ifName).isDownCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v4AddrsMinCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMaxCount);
      EXPECT_EQ(0, res.at(ifName).v6LinkLocalAddrsMinCount);
    }
  }
}

// Test getting unique nodeLabels
TEST_F(LinkMonitorTestFixture, NodeLabelAlloc) {
  size_t kNumNodesToTest = 10;

  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(regexOpts, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(kTestVethNamePrefix + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  // spin up kNumNodesToTest - 1 new link monitors. 1 is spun up in setup()
  std::vector<std::unique_ptr<LinkMonitor>> linkMonitors;
  std::vector<std::unique_ptr<std::thread>> linkMonitorThreads;
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    auto lm = std::make_unique<LinkMonitor>(
        context,
        folly::sformat("lm{}", i + 1),
        0, // platform pub port
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::move(includeRegexList),
        std::move(excludeRegexList),
        std::move(redistRegexList),
        std::vector<thrift::IpPrefix>(),
        false /* useRttMetric */,
        false /* enable perf measurement */,
        false /* enable v4 */,
        true /* enable segment routing */,
        false /* prefix type MPLS */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        AdjacencyDbMarker{"adj:"},
        SparkCmdUrl{"inproc://spark-req"},
        SparkReportUrl{"inproc://spark-report"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        PersistentStoreUrl{kConfigStoreUrl},
        false,
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl},
        PlatformPublisherUrl{"inproc://platform-pub-url"},
        LinkMonitorGlobalPubUrl{
            folly::sformat("inproc://link-monitor-pub-url{}", i + 1)},
        std::chrono::seconds(1),
        std::chrono::milliseconds(1),
        std::chrono::milliseconds(8),
        Constants::kKvStoreDbTtl);
    linkMonitors.emplace_back(std::move(lm));

    auto lmThread = std::make_unique<std::thread>([&linkMonitors]() {
      LOG(INFO) << "LinkMonitor thread starting";
      linkMonitors.back()->run();
      LOG(INFO) << "LinkMonitor thread finishing";
    });
    linkMonitorThreads.emplace_back(std::move(lmThread));
    linkMonitors.back()->waitUntilRunning();
  }

  // map of nodeId to value allocated
  std::unordered_map<std::string, int32_t> nodeLabels;

  // recv kv store publications until we have valid labels from each node
  while (nodeLabels.size() < kNumNodesToTest) {
    auto pub = kvStoreWrapper->recvPublication(std::chrono::seconds(10));
    for (auto const& kv : pub.keyVals) {
      if (kv.first.find("adj:") == 0 and kv.second.value) {
        auto adjDb = fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
            kv.second.value.value(), serializer);
        nodeLabels[adjDb.thisNodeName] = adjDb.nodeLabel;
        if (adjDb.nodeLabel == 0) {
          nodeLabels.erase(adjDb.thisNodeName);
        }
      }
    }
  }

  // ensure that we have unique values
  std::set<int32_t> labelSet;
  for (auto const& kv : nodeLabels) {
    labelSet.insert(kv.second);
  }

  EXPECT_EQ(kNumNodesToTest, labelSet.size());
  // cleanup
  for (size_t i = 0; i < kNumNodesToTest - 1; i++) {
    linkMonitors[i]->stop();
    linkMonitorThreads[i]->join();
  }
}

/**
 * Unit-test to test advertisement of static and loopback prefixes
 * - verify initial prefix-db is set to static prefixes
 * - add addresses via addrEvent and verify from KvStore prefix-db
 * - remove address via addrEvent and verify from KvStore prefix-db
 * - announce network instead of address via addrEvent and verify it doesn't
 *   change anything
 * - set link to down state and verify that it removes all associated addresses
 */
TEST_F(LinkMonitorTestFixture, StaticLoopbackPrefixAdvertisement) {
  // Verify that initial DB has static prefix entries
  std::unordered_set<thrift::IpPrefix> prefixes;
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing initial prefix database";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
  }

  //
  // Send link up event
  //
  mockNlHandler->sendLinkEvent("loopback", 101, true);

  //
  // Advertise some dummy and wrong prefixes
  //

  // push some invalid loopback addresses
  mockNlHandler->sendAddrEvent("loopback", "fe80::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "fe80::2/64", true);

  // push some valid loopback addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/128", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", true);

  // push some valid interface addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", true);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", true);

  // Get interface-db and reply to spark
  recvAndReplyIfUpdate();

  // verify
  prefixes.clear();
  while (prefixes.size() != 7) {
    LOG(INFO) << "Testing address advertisements";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 7) {
      LOG(INFO) << "Looking for 7 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:cafe:babe::1/128")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.127.240.1/32")));

    EXPECT_EQ(1, prefixes.count(toIpPrefix("10.128.241.0/24")));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::/64")));
  }

  //
  // Withdraw prefix and see it is being withdrawn
  //

  // withdraw some addresses
  mockNlHandler->sendAddrEvent("loopback", "10.127.240.1/32", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:cafe:babe::1/128", false);

  // withdraw addresses with subnet
  mockNlHandler->sendAddrEvent("loopback", "10.128.241.1/24", false);
  mockNlHandler->sendAddrEvent("loopback", "2803:6080:4958:b403::1/64", false);

  // Get interface-db and reply to spark
  recvAndReplyIfUpdate();

  // verify
  prefixes.clear();
  while (prefixes.size() != 3) {
    LOG(INFO) << "Testing address withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 3) {
      LOG(INFO) << "Looking for 3 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
    EXPECT_EQ(1, prefixes.count(toIpPrefix("2803:6080:4958:b403::1/128")));
  }

  //
  // Send link down event
  //

  mockNlHandler->sendLinkEvent("loopback", 101, false);
  recvAndReplyIfUpdate();

  //
  // Verify all addresses are withdrawn on link down event
  //
  prefixes.clear();
  while (prefixes.size() != 2) {
    LOG(INFO) << "Testing prefix withdraws";
    prefixes = getNextPrefixDb("prefix:node-1");
    if (prefixes.size() != 2) {
      LOG(INFO) << "Looking for 2 prefixes got " << prefixes.size();
      continue;
    }
    EXPECT_EQ(1, prefixes.count(staticPrefix1));
    EXPECT_EQ(1, prefixes.count(staticPrefix2));
  }
}

TEST(LinkMonitor, getPeersFromAdjacencies) {
  std::unordered_map<AdjacencyKey, AdjacencyValue> adjacencies;
  std::unordered_map<std::string, thrift::PeerSpec> peers;

  const auto peerSpec0 = thrift::PeerSpec(
      FRAGILE,
      "tcp://[fe80::2%iface0]:10001",
      "tcp://[fe80::2%iface0]:10002",
      false);
  const auto peerSpec1 = thrift::PeerSpec(
      FRAGILE,
      "tcp://[fe80::2%iface1]:10001",
      "tcp://[fe80::2%iface1]:10002",
      false);
  const auto peerSpec2 = thrift::PeerSpec(
      FRAGILE,
      "tcp://[fe80::2%iface2]:10001",
      "tcp://[fe80::2%iface2]:10002",
      false);
  const auto peerSpec3 = thrift::PeerSpec(
      FRAGILE,
      "tcp://[fe80::2%iface3]:10001",
      "tcp://[fe80::2%iface3]:10002",
      false);

  // Get peer spec
  adjacencies[{"node1", "iface1"}] = {peerSpec1, thrift::Adjacency()};
  adjacencies[{"node2", "iface2"}] = {peerSpec2, thrift::Adjacency()};
  peers["node1"] = peerSpec1;
  peers["node2"] = peerSpec2;
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node2, iface3} to adjacencies and see no changes peers
  adjacencies[{"node2", "iface3"}] = {peerSpec3, thrift::Adjacency()};
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Add {node1, iface0} to adjacencies and see node1 changes to peerSpec0
  adjacencies[{"node1", "iface0"}] = {peerSpec0, thrift::Adjacency()};
  peers["node1"] = peerSpec0;
  EXPECT_EQ(4, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface2} from adjacencies and see node2 changes to peerSpec3
  adjacencies.erase({"node2", "iface2"});
  peers["node2"] = peerSpec3;
  EXPECT_EQ(3, adjacencies.size());
  EXPECT_EQ(2, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Remove {node2, iface3} from adjacencies and see node2 no longer exists
  adjacencies.erase({"node2", "iface3"});
  peers.erase("node2");
  EXPECT_EQ(2, adjacencies.size());
  EXPECT_EQ(1, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));

  // Test for empty adjacencies
  adjacencies.clear();
  peers.clear();
  EXPECT_EQ(0, adjacencies.size());
  EXPECT_EQ(0, peers.size());
  EXPECT_EQ(peers, LinkMonitor::getPeersFromAdjacencies(adjacencies));
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
