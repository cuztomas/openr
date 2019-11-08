/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <openr/fib/Fib.h>
#include <openr/fib/tests/MockNetlinkFibHandler.h>
#include <openr/fib/tests/PrefixGenerator.h>
#include <openr/tests/OpenrThriftServerWrapper.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thread>

/**
 * Defines a benchmark that allows users to record customized counter during
 * benchmarking and passes a parameter to another one. This is common for
 * benchmarks that need a "problem size" in addition to "number of iterations".
 */
#define BENCHMARK_COUNTERS_PARAM(name, counters, param) \
  BENCHMARK_COUNTERS_NAME_PARAM(name, counters, param, param)

/*
 * Like BENCHMARK_COUNTERS_PARAM(), but allows a custom name to be specified for
 * each parameter, rather than using the parameter value.
 */
#define BENCHMARK_COUNTERS_NAME_PARAM(name, counters, param_name, ...) \
  BENCHMARK_IMPL_COUNTERS(                                             \
      FB_CONCATENATE(name, FB_CONCATENATE(_, param_name)),             \
      FB_STRINGIZE(name) "(" FB_STRINGIZE(param_name) ")",             \
      counters,                                                        \
      iters,                                                           \
      unsigned,                                                        \
      iters) {                                                         \
    name(counters, iters, ##__VA_ARGS__);                              \
  }

using namespace folly;
namespace {
// Virtual interface
const std::string kVethNameY("vethTestY");
// Prefix length of a subnet
static const long kBitMaskLen = 128;
// Updating kDeltaSize routing entries
static const uint32_t kDeltaSize = 10;
// Number of nexthops
const uint8_t kNumOfNexthops = 128;

} // anonymous namespace

namespace openr {

using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

class FibWrapper {
 public:
  FibWrapper() {
    // Register Singleton
    folly::SingletonVault::singleton()->registrationComplete();
    // Create MockNetlinkFibHandler
    mockFibHandler = std::make_shared<MockNetlinkFibHandler>();

    // Start ThriftServer
    server = std::make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler);
    fibThriftThread.start(server);

    // Create sockets
    decisionPub.bind(fbzmq::SocketUrl{"inproc://decision-pub"});

    // Creat Fib module and start fib thread
    port = fibThriftThread.getAddress()->getPort();
    fib = std::make_shared<Fib>(
        "node-1",
        port, // thrift port
        false, // dryrun
        false, // segment route
        false, // orderedFib
        std::chrono::seconds(2),
        false, // waitOnDecision
        DecisionPubUrl{"inproc://decision-pub"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalCmdUrl{"inproc://kvstore-cmd"},
        KvStoreLocalPubUrl{"inproc://kvstore-sub"},
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
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalPubUrl{"inproc://kvstore-sub"},
        context);
    openrThriftServerWrapper_->addModuleType(thrift::OpenrModuleType::FIB, fib);
    openrThriftServerWrapper_->run();
  }

  ~FibWrapper() {
    LOG(INFO) << "Stopping openr-ctrl thrift server";
    openrThriftServerWrapper_->stop();
    LOG(INFO) << "Openr-ctrl thrift server got stopped";

    // This will be invoked before Fib's d-tor
    fib->stop();
    fibThread->join();

    // Close socket
    decisionPub.close();

    // Stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
  }

  thrift::PerfDatabase
  getPerfDb() {
    thrift::PerfDatabase perfDb;
    auto resp = openrThriftServerWrapper_->getOpenrCtrlHandler()
                    ->semifuture_getPerfDb()
                    .get();
    EXPECT_TRUE(resp);

    perfDb = *resp;
    return perfDb;
  }

  void
  accumulatePerfTimes(std::vector<uint64_t>& processTimes) {
    // Get perfDB
    auto perfDB = getPerfDb();
    // If get empty perfDB, just log it
    if (perfDB.eventInfo.size() == 0 or
        perfDB.eventInfo[0].events.size() == 0) {
      LOG(INFO) << "perfDB is emtpy.";
    } else {
      // Accumulate time into processTimes
      // Each time get the latest perf event.
      auto perfDBInfoSize = perfDB.eventInfo.size();
      auto eventInfo = perfDB.eventInfo[perfDBInfoSize - 1];
      for (size_t index = 1; index < eventInfo.events.size(); index++) {
        processTimes[index - 1] +=
            (eventInfo.events[index].unixTs -
             eventInfo.events[index - 1].unixTs);
      }
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> decisionPub{context};

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler;
  PrefixGenerator prefixGenerator;

  // thriftServer to talk to Fib
  std::shared_ptr<OpenrThriftServerWrapper> openrThriftServerWrapper_{nullptr};
};

/**
 * Benchmark for fib
 * 1. Create a fib
 * 2. Generate random IpV6s and routes
 * 3. Send routes to fib
 * 4. Wait until the completion of routes update
 */
static void
BM_Fib(folly::UserCounters& counters, uint32_t iters, unsigned numOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  // Fib starts with clean route database
  auto fibWrapper = std::make_unique<FibWrapper>();

  // Initial syncFib debounce
  fibWrapper->mockFibHandler->waitForSyncFib();

  // Generate random prefixes
  auto prefixes = fibWrapper->prefixGenerator.ipv6PrefixGenerator(
      numOfPrefixes, kBitMaskLen);
  thrift::RouteDatabaseDelta routeDbDelta;
  routeDbDelta.thisNodeName = "node-1";
  for (auto& prefix : prefixes) {
    routeDbDelta.unicastRoutesToUpdate.emplace_back(createUnicastRoute(
        prefix,
        fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
            kNumOfNexthops, kVethNameY)));
  }
  // Send routeDB to Fib and wait for updating completing
  fibWrapper->decisionPub.sendThriftObj(routeDbDelta, fibWrapper->serializer);
  fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

  // Customized time counter
  // processTimes[0] is the time of sending routDB from decision to Fib
  // processTimes[1] is the time of processing DB within Fib
  // processTimes[2] is the time of programming routs with Fib agent server
  std::vector<uint64_t> processTimes{0, 0, 0};
  // Maek sure deltaSize <= numOfPrefixes
  auto deltaSize = kDeltaSize <= numOfPrefixes ? kDeltaSize : numOfPrefixes;
  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    // Update routes by randomly regenerating nextHops for deltaSize prefixes.
    routeDbDelta.unicastRoutesToUpdate.clear();
    for (uint32_t index = 0; index < deltaSize; index++) {
      routeDbDelta.unicastRoutesToUpdate.emplace_back(createUnicastRoute(
          prefixes[index],
          fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
              kNumOfNexthops, kVethNameY)));
    }
    // Add perfevents
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, routeDbDelta.thisNodeName, "FIB_INIT_UPDATE");
    routeDbDelta.perfEvents = perfEvents;

    // Send routeDB to Fib for updates
    fibWrapper->decisionPub.sendThriftObj(routeDbDelta, fibWrapper->serializer);
    fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

    // Get time information from perf event
    fibWrapper->accumulatePerfTimes(processTimes);
  }

  suspender.rehire(); // Stop measuring time again
  // Get average time for each itaration
  for (auto& processTime : processTimes) {
    processTime /= iters == 0 ? 1 : iters;
  }

  // Add customized counters to state.
  counters["route_receive"] = processTimes[0];
  counters["route_install"] = processTimes[2];
}

// The parameter is the number of prefixes sent to fib
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 10);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 1000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 9000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
