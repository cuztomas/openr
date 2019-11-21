/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <set>
#include <string>
#include <unordered_set>

#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#if FOLLY_USE_SYMBOLIZER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif
#include <gflags/gflags.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/LinkState.h>
#include <openr/decision/PrefixState.h>

using namespace std;

using apache::thrift::FRAGILE;
using apache::thrift::TEnumTraits;

using Metric = openr::LinkStateMetric;

using SpfResult = unordered_map<
    string /* otherNodeName */,
    pair<Metric, unordered_set<string /* nextHopNodeName */>>>;

// Path starts from neighbor node and ends at destination. It's size must be
// atleast one. Second attribute describe the link that is followed from
// associated node (first attribute)
// Path is described in reverse. Last index is the next immediate node.
using Path = std::vector<std::pair<std::string, std::shared_ptr<openr::Link>>>;

namespace {

// Default HWM is 1k. We set it to 0 to buffer all received messages.
const int kStoreSubReceiveHwm{0};

// check if path A is part of path B.
// Example:
// path A: a->b->c
// path B: d->a->b->c->d
// return True
bool
pathAInPathB(const Path& a, const Path& b) {
  for (int i = 0; i < b.size(); i++) {
    if (b[i].second == a[0].second) {
      if (a.size() > (b.size() - i)) {
        continue;
      }
      bool equal = true;
      for (int j = 0; j < a.size(); j++) {
        if (a[j].second != b[i + j].second) {
          equal = false;
          break;
        }
      }
      if (equal) {
        return equal;
      }
    }
  }

  return false;
}

// Classes needed for running Dijkstra
class DijkstraQNode {
 public:
  DijkstraQNode(const std::string& n, Metric d) : nodeName(n), distance(d) {}
  const std::string nodeName;
  Metric distance{0};
  std::unordered_set<std::string> nextHops;
};

class DijkstraQ {
 private:
  std::vector<std::shared_ptr<DijkstraQNode>> heap_;
  std::unordered_map<std::string, std::shared_ptr<DijkstraQNode>> nameToNode_;

  struct {
    bool
    operator()(
        std::shared_ptr<DijkstraQNode> a,
        std::shared_ptr<DijkstraQNode> b) const {
      if (a->distance != b->distance) {
        return a->distance > b->distance;
      }
      return a->nodeName > b->nodeName;
    }
  } DijkstraQNodeGreater;

 public:
  void
  insertNode(const std::string& nodeName, Metric d) {
    heap_.push_back(std::make_shared<DijkstraQNode>(nodeName, d));
    nameToNode_[nodeName] = heap_.back();
    std::push_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
  }

  std::shared_ptr<DijkstraQNode>
  get(const std::string& nodeName) {
    if (nameToNode_.count(nodeName)) {
      return nameToNode_.at(nodeName);
    }
    return nullptr;
  }

  std::shared_ptr<DijkstraQNode>
  extractMin() {
    if (heap_.empty()) {
      return nullptr;
    }
    auto min = heap_.at(0);
    CHECK(nameToNode_.erase(min->nodeName));
    std::pop_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
    heap_.pop_back();
    return min;
  }

  void
  decreaseKey(const std::string& nodeName, Metric d) {
    if (nameToNode_.count(nodeName)) {
      if (nameToNode_.at(nodeName)->distance < d) {
        throw std::invalid_argument(std::to_string(d));
      }
      nameToNode_.at(nodeName)->distance = d;
      // this is a bit slow but is rarely called in our application. In fact,
      // in networks where the metric is hop count, this will never be called
      // and the Dijkstra run is no different than BFS
      std::make_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
    } else {
      throw std::invalid_argument(nodeName);
    }
  }
};

} // anonymous namespace

namespace openr {

/**
 * Private implementation of the SpfSolver
 */
class SpfSolver::SpfSolverImpl {
 public:
  SpfSolverImpl(
      const std::string& myNodeName,
      bool enableV4,
      bool computeLfaPaths,
      bool enableOrderedFib,
      bool bgpDryRun,
      bool bgpUseIgpMetric)
      : myNodeName_(myNodeName),
        enableV4_(enableV4),
        computeLfaPaths_(computeLfaPaths),
        enableOrderedFib_(enableOrderedFib),
        bgpDryRun_(bgpDryRun),
        bgpUseIgpMetric_(bgpUseIgpMetric) {
    // Initialize stat keys
    tData_.addStatExportType("decision.adj_db_update", fbzmq::COUNT);
    tData_.addStatExportType(
        "decision.incompatible_forwarding_type", fbzmq::COUNT);
    tData_.addStatExportType("decision.missing_loopback_addr", fbzmq::SUM);
    tData_.addStatExportType("decision.no_route_to_label", fbzmq::COUNT);
    tData_.addStatExportType("decision.no_route_to_prefix", fbzmq::COUNT);
    tData_.addStatExportType("decision.path_build_ms", fbzmq::AVG);
    tData_.addStatExportType("decision.path_build_runs", fbzmq::COUNT);
    tData_.addStatExportType("decision.prefix_db_update", fbzmq::COUNT);
    tData_.addStatExportType("decision.route_build_ms", fbzmq::AVG);
    tData_.addStatExportType("decision.route_build_runs", fbzmq::COUNT);
    tData_.addStatExportType("decision.skipped_mpls_route", fbzmq::COUNT);
    tData_.addStatExportType("decision.skipped_unicast_route", fbzmq::COUNT);
    tData_.addStatExportType("decision.spf_ms", fbzmq::AVG);
    tData_.addStatExportType("decision.spf_runs", fbzmq::COUNT);
  }

  ~SpfSolverImpl() = default;

  std::pair<
      bool /* topology has changed*/,
      bool /* route attributes has changed (nexthop addr, node/adj label */>
  updateAdjacencyDatabase(thrift::AdjacencyDatabase const& newAdjacencyDb);

  bool hasHolds() const;

  // returns true if the AdjacencyDatabase existed
  bool deleteAdjacencyDatabase(const std::string& nodeName);

  std::unordered_map<
      std::string /* nodeName */,
      thrift::AdjacencyDatabase> const&
  getAdjacencyDatabases();
  // returns true if the prefixDb changed
  bool updatePrefixDatabase(const thrift::PrefixDatabase& prefixDb);

  // returns true if the PrefixDatabase existed
  bool deletePrefixDatabase(const std::string& nodeName);

  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases();

  folly::Optional<thrift::RouteDatabase> buildPaths(
      const std::string& myNodeName);
  folly::Optional<thrift::RouteDatabase> buildRouteDb(
      const std::string& myNodeName);

  bool decrementHolds();

  std::unordered_map<std::string, int64_t> getCounters();

  fbzmq::ThreadData&
  getThreadData() noexcept {
    return tData_;
  }

  static std::pair<Metric, std::unordered_set<std::string>> getMinCostNodes(
      const SpfResult& spfResult, const std::set<std::string>& dstNodes);

 private:
  // no copy
  SpfSolverImpl(SpfSolverImpl const&) = delete;
  SpfSolverImpl& operator=(SpfSolverImpl const&) = delete;

  // run SPF and produce map from node name to next-hops that have shortest
  // paths to it
  SpfResult runSpf(
      const std::string& nodeName,
      bool useLinkMetric,
      const LinkState::LinkSet& linksToIgnore = {});

  // Trace all edge disjoint paths from source to destination node.
  // srcNodeDistances => map indicating distances of each node from source
  // Returns list of paths.
  // Each path is list<node, Link> starting from immediate neighbor of
  // source-node to destination node. Path length will be atleast one
  std::vector<Path> traceEdgeDisjointPaths(
      const std::string& srcNodeName,
      const std::string& dstNodeName,
      const SpfResult& srcNodeDistances,
      const LinkState::LinkSet& linksToIgnore = {});

  folly::Optional<thrift::UnicastRoute> createOpenRRoute(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
      bool const isV4);
  folly::Optional<thrift::UnicastRoute> createBGPRoute(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
      bool const isV4);

  // helper function to find the nodes for the nexthop for bgp route
  BestPathCalResult findDstNodesForBgpRoute(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
      bool const isV4);

  BestPathCalResult getBestAnnouncingNodes(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
      bool const isV4,
      bool const hasBgp,
      bool const useKsp2EdAlgo);

  folly::Optional<int64_t> getMinNextHopThreshold(
      BestPathCalResult nodes,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes);

  // helper to filter overloaded nodes for anycast addresses
  BestPathCalResult maybeFilterDrainedNodes(BestPathCalResult&& result) const;

  // given curNode and the dst nodes, find 2spf paths from curNode to each
  // dstNode
  std::unordered_map<std::string, std::vector<std::pair<Path, Metric>>>
  createOpenRKsp2EdRouteForNodes(
      std::string const& myNodeName,
      std::unordered_set<std::string> const& nodes);

  // Given prefixes and the nodes who announce it, get the kspf routes.
  folly::Optional<thrift::UnicastRoute> selectKsp2Routes(
      const thrift::IpPrefix& prefix,
      const string& myNodeName,
      BestPathCalResult const& bestPathCalResult,
      const std::unordered_map<
          std::string,
          std::vector<std::pair<Path, Metric>>>& routeToNodes,
      std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes);

  // Give source node-name and dstNodeNames, this function returns the set of
  // nexthops (along with LFA if enabled) towards these set of dstNodeNames
  std::pair<
      Metric /* minimum metric to destination */,
      std::unordered_map<
          std::pair<std::string /* nextHopNodeName */, std::string /* dest */>,
          Metric /* the distance from the nexthop to the dest */>>
  getNextHopsWithMetric(
      const std::string& srcNodeName,
      const std::set<std::string>& dstNodeNames,
      bool perDestination) const;

  // This function converts best nexthop nodes to best nexthop adjacencies
  // which can then be passed to FIB for programming. It considers LFA and
  // parallel link logic (tested by our UT)
  // If swap label is provided then it will be used to associate SWAP or PHP
  // mpls action
  std::vector<thrift::NextHopThrift> getNextHopsThrift(
      const std::string& myNodeName,
      const std::set<std::string>& dstNodeNames,
      bool isV4,
      bool perDestination,
      const Metric minMetric,
      std::unordered_map<std::pair<std::string, std::string>, Metric>
          nextHopNodes,
      folly::Optional<int32_t> swapLabel) const;

  Metric findMinDistToNeighbor(
      const std::string& myNodeName, const std::string& neighborName) const;

  // returns the hop count from myNodeName_ to nodeName
  Metric getMyHopsToNode(const std::string& nodeName);
  // returns the hop count of the furthest node connected to nodeName
  Metric getMaxHopsToNode(const std::string& nodeName);

  LinkState linkState_;

  PrefixState prefixState_;

  // Save all direct next-hop distance from a given source node to a destination
  // node. We update it as we compute all LFA routes from perspective of source
  std::unordered_map<
      std::string /* source nodeName */,
      unordered_map<
          string /* otherNodeName */,
          pair<Metric, unordered_set<string /* nextHopNodeName */>>>>
      spfResults_;

  // track some stats
  fbzmq::ThreadData tData_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool computeLfaPaths_{false};

  const bool enableOrderedFib_{false};

  const bool bgpDryRun_{false};

  // Use IGP metric in metric vector comparision
  const bool bgpUseIgpMetric_{false};
};

std::pair<
    bool /* topology has changed*/,
    bool /* route attributes has changed (nexthop addr, node/adj label */>
SpfSolver::SpfSolverImpl::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb) {
  LinkStateMetric holdUpTtl = 0, holdDownTtl = 0;
  if (enableOrderedFib_) {
    holdUpTtl = getMyHopsToNode(newAdjacencyDb.thisNodeName);
    holdDownTtl = getMaxHopsToNode(newAdjacencyDb.thisNodeName) - holdUpTtl;
  }
  tData_.addStatValue("decision.adj_db_update", 1, fbzmq::COUNT);
  auto rc = linkState_.updateAdjacencyDatabase(
      newAdjacencyDb, holdUpTtl, holdDownTtl);
  // temporary hack needed to keep UTs happy
  rc.second = rc.second && myNodeName_ == newAdjacencyDb.thisNodeName;
  return rc;
}

bool
SpfSolver::SpfSolverImpl::hasHolds() const {
  return linkState_.hasHolds();
}

Metric
SpfSolver::SpfSolverImpl::getMyHopsToNode(const std::string& nodeName) {
  if (myNodeName_ == nodeName) {
    return 0;
  }
  auto spfResult = runSpf(myNodeName_, false);
  if (spfResult.count(nodeName)) {
    return spfResult.at(nodeName).first;
  }
  return getMaxHopsToNode(nodeName);
}

Metric
SpfSolver::SpfSolverImpl::getMaxHopsToNode(const std::string& nodeName) {
  Metric max = 0;
  for (auto const& pathsFromNode : runSpf(nodeName, false)) {
    max = std::max(max, pathsFromNode.second.first);
  }
  return max;
}

bool
SpfSolver::SpfSolverImpl::decrementHolds() {
  return linkState_.decrementHolds();
}

bool
SpfSolver::SpfSolverImpl::deleteAdjacencyDatabase(const std::string& nodeName) {
  return linkState_.deleteAdjacencyDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase> const&
SpfSolver::SpfSolverImpl::getAdjacencyDatabases() {
  return linkState_.getAdjacencyDatabases();
}

bool
SpfSolver::SpfSolverImpl::updatePrefixDatabase(
    thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;
  VLOG(1) << "Updating prefix database for node " << nodeName;
  tData_.addStatValue("decision.prefix_db_update", 1, fbzmq::COUNT);
  return prefixState_.updatePrefixDatabase(prefixDb);
}

bool
SpfSolver::SpfSolverImpl::deletePrefixDatabase(const std::string& nodeName) {
  return prefixState_.deletePrefixDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolver::SpfSolverImpl::getPrefixDatabases() {
  return prefixState_.getPrefixDatabases();
}

/**
 * Compute shortest-path routes from perspective of nodeName;
 */
unordered_map<
    string /* otherNodeName */,
    pair<Metric, unordered_set<string /* nextHopNodeName */>>>
SpfSolver::SpfSolverImpl::runSpf(
    const std::string& thisNodeName,
    bool useLinkMetric,
    const LinkState::LinkSet& linksToIgnore) {
  unordered_map<string, pair<Metric, unordered_set<string>>> result;

  tData_.addStatValue("decision.spf_runs", 1, fbzmq::COUNT);
  const auto startTime = std::chrono::steady_clock::now();

  DijkstraQ q;
  q.insertNode(thisNodeName, 0);
  uint64_t loop = 0;
  for (auto node = q.extractMin(); node; node = q.extractMin()) {
    ++loop;
    // we've found this node's shortest paths. record it
    auto emplaceRc = result.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(node->nodeName),
        std::forward_as_tuple(node->distance, std::move(node->nextHops)));
    CHECK(emplaceRc.second);

    auto& recordedNodeName = emplaceRc.first->first;
    auto& recordedNodeMetric = emplaceRc.first->second.first;
    auto& recordedNodeNextHops = emplaceRc.first->second.second;

    if (linkState_.isNodeOverloaded(recordedNodeName) &&
        recordedNodeName != thisNodeName) {
      // no transit traffic through this node. we've recorded the nexthops to
      // this node, but will not consider any of it's adjancecies as offering
      // lower cost paths towards further away nodes. This effectively drains
      // traffic away from this node
      continue;
    }
    // we have the shortest path nexthops for recordedNodeName. Use these
    // nextHops for any node that is connected to recordedNodeName that doesn't
    // already have a lower cost path from thisNodeName
    //
    // this is the "relax" step in the Dijkstra Algorithm pseudocode in CLRS
    for (const auto& link : linkState_.linksFromNode(recordedNodeName)) {
      auto otherNodeName = link->getOtherNodeName(recordedNodeName);
      if (!link->isUp() or result.count(otherNodeName) or
          linksToIgnore.count(link)) {
        continue;
      }
      auto metric =
          useLinkMetric ? link->getMetricFromNode(recordedNodeName) : 1;
      auto otherNode = q.get(otherNodeName);
      if (!otherNode) {
        q.insertNode(otherNodeName, recordedNodeMetric + metric);
        otherNode = q.get(otherNodeName);
      }
      if (otherNode->distance >= recordedNodeMetric + metric) {
        // recordedNodeName is either along an alternate shortest path towards
        // otherNodeName or is along a new shorter path. In either case,
        // otherNodeName should use recordedNodeName's nextHops until it finds
        // some shorter path
        if (otherNode->distance > recordedNodeMetric + metric) {
          // if this is strictly better, forget about any other nexthops
          otherNode->nextHops.clear();
          q.decreaseKey(otherNode->nodeName, recordedNodeMetric + metric);
        }
        otherNode->nextHops.insert(
            recordedNodeNextHops.begin(), recordedNodeNextHops.end());
      }
      if (otherNode->nextHops.empty()) {
        // this node is directly connected to the source
        otherNode->nextHops.emplace(otherNode->nodeName);
      }
    }
  }
  VLOG(3) << "Dijkstra loop count: " << loop;
  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "SPF elapsed time: " << deltaTime.count() << "ms.";
  tData_.addStatValue("decision.spf_ms", deltaTime.count(), fbzmq::AVG);
  return result;
}

std::vector<Path>
SpfSolver::SpfSolverImpl::traceEdgeDisjointPaths(
    const std::string& srcNodeName,
    const std::string& dstNodeName,
    const SpfResult& spfResult,
    const LinkState::LinkSet& linksToIgnore) {
  std::vector<Path> paths;

  // Return immediately if destination node is not reachable
  if (not spfResult.count(dstNodeName)) {
    return paths;
  }

  //
  // Here we're tracing paths in reverse from destination node. We first pick
  // neighbors of destination node which are on the shortest paths
  //
  std::unordered_set<std::pair<std::string, std::string>> visitedLinks;
  // Starting with dummy entry for expanding paths
  std::vector<Path> partialPaths = {{{dstNodeName, nullptr}}};

  //
  // Now recursively trace all the partial paths to the source
  //
  while (partialPaths.size()) {
    auto spurPath = std::move(partialPaths.back());
    partialPaths.pop_back();

    const auto spurNode = folly::copy(spurPath.back().first);

    // Clear dummy entry in the path
    if (spurNode == dstNodeName) {
      spurPath.clear();
    }

    // If we have encountered source-node then include this path in
    // the return value
    if (spurNode == srcNodeName) {
      paths.emplace_back(std::move(spurPath));
      continue;
    }

    // Iterate over all neighbors to expand spurPath further
    for (auto& link : linkState_.linksFromNode(spurNode)) {
      // Skip ignored links
      // in some scenario, the nbrnode may not be
      // accessible from srcNode, even though link between
      // spur node and nbrnode is still up. For example,
      // if spurnode is overloaded, and the only link between
      // nbrnode and rest of graph is through spurnode.
      // if neighbor node is over loaded skip it.
      auto& nbrName = link->getOtherNodeName(spurNode);
      if (!link->isUp() or linksToIgnore.count(link) or
          spfResult.find(nbrName) == spfResult.end() or
          linkState_.isNodeOverloaded(nbrName)) {
        continue;
      }
      auto& nbrIface = link->getIfaceFromNode(nbrName);
      auto nbrMetric = spfResult.at(nbrName).first;
      auto spurMetric = spfResult.at(spurNode).first;

      // Ignore already seen neighbor (except source-node)
      if (visitedLinks.count({nbrName, nbrIface})) {
        continue;
      }

      // Ignore links not on the shortest path
      // A link is on the shortest path iff following is true
      if (nbrMetric + link->getMetricFromNode(nbrName) != spurMetric) {
        continue;
      }

      CHECK_EQ(nbrMetric + link->getMetricFromNode(nbrName), spurMetric);
      spurPath.emplace_back(std::make_pair(nbrName, link));
      partialPaths.emplace_back(std::move(spurPath));
      visitedLinks.emplace(nbrName, nbrIface);

      // Allow fan-out from dstNodeName else continue tracing only one path
      if (spurNode != dstNodeName) {
        break; // We have extended current path to one of the neighbor
      }
    }
  }

  return paths;
}

folly::Optional<thrift::RouteDatabase>
SpfSolver::SpfSolverImpl::buildPaths(const std::string& myNodeName) {
  if (!linkState_.hasNode(myNodeName)) {
    return folly::none;
  }

  auto const& startTime = std::chrono::steady_clock::now();
  tData_.addStatValue("decision.path_build_runs", 1, fbzmq::COUNT);

  spfResults_.clear();
  spfResults_[myNodeName] = runSpf(myNodeName, true);
  if (computeLfaPaths_) {
    // avoid duplicate iterations over a neighbor which can happen due to
    // multiple adjacencies to it
    std::unordered_set<std::string /* adjacent node name */> visitedAdjNodes;
    for (auto const& link : linkState_.linksFromNode(myNodeName)) {
      auto const& otherNodeName = link->getOtherNodeName(myNodeName);
      // Skip if already visited
      if (!visitedAdjNodes.insert(otherNodeName).second || !link->isUp()) {
        continue;
      }
      spfResults_[otherNodeName] = runSpf(otherNodeName, true);
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildPaths took " << deltaTime.count() << "ms.";
  tData_.addStatValue("decision.path_build_ms", deltaTime.count(), fbzmq::AVG);

  return buildRouteDb(myNodeName);
} // buildPaths

folly::Optional<thrift::RouteDatabase>
SpfSolver::SpfSolverImpl::buildRouteDb(const std::string& myNodeName) {
  if (not linkState_.hasNode(myNodeName) or
      spfResults_.count(myNodeName) == 0) {
    return folly::none;
  }

  const auto startTime = std::chrono::steady_clock::now();
  tData_.addStatValue("decision.route_build_runs", 1, fbzmq::COUNT);

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;

  //
  // Create unicastRoutes - IP and IP2MPLS routes
  //
  std::unordered_map<thrift::IpPrefix, BestPathCalResult> prefixToPerformKsp;

  std::unordered_set<std::string> nodesForKsp;

  for (const auto& kv : prefixState_.prefixes()) {
    const auto& prefix = kv.first;
    const auto& nodePrefixes = kv.second;

    bool hasBGP = false, hasNonBGP = false, missingMv = false;
    bool hasSpEcmp = false, hasKsp2EdEcmp = false;
    for (auto const& npKv : nodePrefixes) {
      bool isBGP = npKv.second.type == thrift::PrefixType::BGP;
      hasBGP |= isBGP;
      hasNonBGP |= !isBGP;
      if (isBGP and not npKv.second.mv.hasValue()) {
        missingMv = true;
        LOG(ERROR) << "Prefix entry for prefix " << toString(npKv.second.prefix)
                   << " advertised by " << npKv.first
                   << " is of type BGP but does not contain a metric vector.";
      }
      hasSpEcmp |= npKv.second.forwardingAlgorithm ==
          thrift::PrefixForwardingAlgorithm::SP_ECMP;
      hasKsp2EdEcmp |= npKv.second.forwardingAlgorithm ==
          thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
    }

    // skip adding route for BGP prefixes that have issues
    if (hasBGP) {
      if (hasNonBGP) {
        LOG(ERROR) << "Skipping route for prefix " << toString(prefix)
                   << " which is advertised with BGP and non-BGP type.";
        tData_.addStatValue("decision.skipped_unicast_route", 1, fbzmq::COUNT);
        continue;
      }
      if (missingMv) {
        LOG(ERROR) << "Skipping route for prefix " << toString(prefix)
                   << " at least one advertiser is missing its metric vector.";
        tData_.addStatValue("decision.skipped_unicast_route", 1, fbzmq::COUNT);
        continue;
      }
    }

    // skip adding route for prefixes advertised by this node
    if (nodePrefixes.count(myNodeName) and not hasBGP) {
      continue;
    }

    // Check for enabledV4_
    auto prefixStr = prefix.prefixAddress.addr;
    bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();
    if (isV4Prefix && !enableV4_) {
      LOG(WARNING) << "Received v4 prefix while v4 is not enabled.";
      tData_.addStatValue("decision.skipped_unicast_route", 1, fbzmq::COUNT);
      continue;
    }

    const auto forwardingAlgorithm = hasKsp2EdEcmp and not hasSpEcmp
        ? thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP
        : thrift::PrefixForwardingAlgorithm::SP_ECMP;

    if (forwardingAlgorithm == thrift::PrefixForwardingAlgorithm::SP_ECMP) {
      auto route = hasBGP
          ? createBGPRoute(myNodeName, prefix, nodePrefixes, isV4Prefix)
          : createOpenRRoute(myNodeName, prefix, nodePrefixes, isV4Prefix);
      if (route.hasValue()) {
        routeDb.unicastRoutes.emplace_back(std::move(route.value()));
      }
    } else {
      const auto nodes = getBestAnnouncingNodes(
          myNodeName, prefix, nodePrefixes, isV4Prefix, hasBGP, true);
      if (nodes.success && nodes.nodes.size() != 0) {
        prefixToPerformKsp[prefix] = nodes;
        for (const auto& node : nodes.nodes) {
          nodesForKsp.insert(node);
        }
      }
    }
  } // for prefixState_.prefixes()

  auto routeToNodes = createOpenRKsp2EdRouteForNodes(myNodeName, nodesForKsp);

  for (const auto& kv : prefixToPerformKsp) {
    auto unicastRoute = selectKsp2Routes(
        kv.first,
        myNodeName,
        kv.second,
        routeToNodes,
        prefixState_.prefixes().at(kv.first));
    if (unicastRoute.hasValue()) {
      routeDb.unicastRoutes.emplace_back(std::move(unicastRoute.value()));
    }
  }

  //
  // Create MPLS routes for all nodeLabel
  //
  for (const auto& kv : linkState_.getAdjacencyDatabases()) {
    const auto& adjDb = kv.second;
    const auto topLabel = adjDb.nodeLabel;
    // Top label is not set => Non-SR mode
    if (topLabel == 0) {
      continue;
    }
    // If mpls label is not valid then ignore it
    if (not isMplsLabelValid(topLabel)) {
      LOG(ERROR) << "Ignoring invalid node label " << topLabel << " of node "
                 << adjDb.thisNodeName;
      tData_.addStatValue("decision.skipped_mpls_route", 1, fbzmq::COUNT);
      continue;
    }

    // Install POP_AND_LOOKUP for next layer
    if (adjDb.thisNodeName == myNodeName) {
      thrift::NextHopThrift nh;
      nh.address = toBinaryAddress(folly::IPAddressV6("::"));
      nh.mplsAction = createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
      routeDb.mplsRoutes.emplace_back(
          createMplsRoute(topLabel, {std::move(nh)}));
      continue;
    }

    // Get best nexthop towards the node
    auto metricNhs =
        getNextHopsWithMetric(myNodeName, {adjDb.thisNodeName}, false);
    if (metricNhs.second.empty()) {
      LOG(WARNING) << "No route to nodeLabel " << std::to_string(topLabel)
                   << " of node " << adjDb.thisNodeName;
      tData_.addStatValue("decision.no_route_to_label", 1, fbzmq::COUNT);
      continue;
    }

    // Create nexthops with appropriate MplsAction (PHP and SWAP). Note that all
    // nexthops are valid for routing without loops. Fib is responsible for
    // installing these routes by making sure it programs least cost nexthops
    // first and of same action type (based on HW limitations)
    auto nextHopsThrift = getNextHopsThrift(
        myNodeName,
        {adjDb.thisNodeName},
        false,
        false,
        metricNhs.first,
        metricNhs.second,
        topLabel);
    routeDb.mplsRoutes.emplace_back(
        createMplsRoute(topLabel, std::move(nextHopsThrift)));
  }

  //
  // Create MPLS routes for all of our adjacencies
  //
  for (const auto& link : linkState_.linksFromNode(myNodeName)) {
    const auto topLabel = link->getAdjLabelFromNode(myNodeName);
    // Top label is not set => Non-SR mode
    if (topLabel == 0) {
      continue;
    }
    // If mpls label is not valid then ignore it
    if (not isMplsLabelValid(topLabel)) {
      LOG(ERROR) << "Ignoring invalid adjacency label " << topLabel
                 << " of link " << link->directionalToString(myNodeName);
      tData_.addStatValue("decision.skipped_mpls_route", 1, fbzmq::COUNT);
      continue;
    }

    auto nh = createNextHop(
        link->getNhV6FromNode(myNodeName),
        link->getIfaceFromNode(myNodeName),
        link->getMetricFromNode(myNodeName),
        createMplsAction(thrift::MplsActionCode::PHP));
    routeDb.mplsRoutes.emplace_back(createMplsRoute(topLabel, {std::move(nh)}));
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  tData_.addStatValue("decision.route_build_ms", deltaTime.count(), fbzmq::AVG);
  return routeDb;
} // buildRouteDb

BestPathCalResult
SpfSolver::SpfSolverImpl::getBestAnnouncingNodes(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
    bool const isV4,
    bool const hasBgp,
    bool const useKsp2EdAlgo) {
  BestPathCalResult dstNodes;
  if (useKsp2EdAlgo) {
    for (const auto& nodePrefix : nodePrefixes) {
      if (nodePrefix.second.forwardingType !=
          thrift::PrefixForwardingType::SR_MPLS) {
        LOG(ERROR) << nodePrefix.first << " has incompatible forwarding type "
                   << TEnumTraits<thrift::PrefixForwardingType>::findName(
                          nodePrefix.second.forwardingType)
                   << " for algorithm KSP2_ED_ECMP;";
        tData_.addStatValue(
            "decision.incompatible_forwarding_type", 1, fbzmq::COUNT);
        return dstNodes;
      }
    }
  }

  // if it is openr route, all nodes are considered as best nodes.
  if (not hasBgp) {
    for (const auto& kv : nodePrefixes) {
      if (kv.first == myNodeName) {
        dstNodes.nodes.clear();
        return dstNodes;
      }
      dstNodes.nodes.insert(kv.first);
    }
    dstNodes.success = true;
    return maybeFilterDrainedNodes(std::move(dstNodes));
  }

  // for bgp route, we need to run best path calculation algorithm to get
  // the nodes
  auto bestPathCalRes =
      findDstNodesForBgpRoute(myNodeName, prefix, nodePrefixes, isV4);
  if (bestPathCalRes.success && (not bestPathCalRes.nodes.count(myNodeName))) {
    return maybeFilterDrainedNodes(std::move(bestPathCalRes));
  } else if (not bestPathCalRes.success) {
    LOG(WARNING) << "No route to BGP prefix " << toString(prefix);
    tData_.addStatValue("decision.no_route_to_prefix", 1, fbzmq::COUNT);
  } else {
    VLOG(2) << "Ignoring route to BGP prefix " << toString(prefix)
            << ". Best path originated by self.";
  }

  return dstNodes;
}

folly::Optional<int64_t>
SpfSolver::SpfSolverImpl::getMinNextHopThreshold(
    BestPathCalResult nodes,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes) {
  folly::Optional<int64_t> maxMinNexthopForPrefix = folly::none;
  for (const auto& node : nodes.nodes) {
    auto npKv = nodePrefixes.find(node);
    if (npKv != nodePrefixes.end()) {
      maxMinNexthopForPrefix = npKv->second.minNexthop.hasValue() &&
              (not maxMinNexthopForPrefix.has_value() ||
               npKv->second.minNexthop.value() > maxMinNexthopForPrefix.value())
          ? npKv->second.minNexthop.value()
          : maxMinNexthopForPrefix;
    }
  }
  return maxMinNexthopForPrefix;
}

BestPathCalResult
SpfSolver::SpfSolverImpl::maybeFilterDrainedNodes(
    BestPathCalResult&& result) const {
  BestPathCalResult filtered = result;
  for (auto iter = filtered.nodes.begin(); iter != filtered.nodes.end();) {
    if (linkState_.isNodeOverloaded(*iter)) {
      iter = filtered.nodes.erase(iter);
    } else {
      ++iter;
    }
  }
  return filtered.nodes.empty() ? result : filtered;
}

folly::Optional<thrift::UnicastRoute>
SpfSolver::SpfSolverImpl::createOpenRRoute(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
    bool const isV4) {
  // Prepare list of nodes announcing the prefix
  const auto dstNodes = getBestAnnouncingNodes(
      myNodeName, prefix, nodePrefixes, isV4, false, false);
  if (not dstNodes.success) {
    return folly::none;
  }

  std::set<std::string> prefixNodes = dstNodes.nodes;

  const bool perDestination = getPrefixForwardingType(nodePrefixes) ==
      thrift::PrefixForwardingType::SR_MPLS;

  const auto metricNhs =
      getNextHopsWithMetric(myNodeName, prefixNodes, perDestination);
  if (metricNhs.second.empty()) {
    LOG(WARNING) << "No route to prefix " << toString(prefix)
                 << ", advertised by: " << folly::join(", ", prefixNodes);
    tData_.addStatValue("decision.no_route_to_prefix", 1, fbzmq::COUNT);
    return folly::none;
  }

  // Convert list of neighbor nodes to nexthops (considering adjacencies)
  return createUnicastRoute(
      prefix,
      getNextHopsThrift(
          myNodeName,
          prefixNodes,
          isV4,
          perDestination,
          metricNhs.first,
          metricNhs.second,
          folly::none));
}

BestPathCalResult
SpfSolver::SpfSolverImpl::findDstNodesForBgpRoute(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
    bool const isV4) {
  BestPathCalResult ret;
  const auto& mySpfResult = spfResults_.at(myNodeName);
  for (auto const& kv : nodePrefixes) {
    auto const& nodeName = kv.first;
    auto const& prefixEntry = kv.second;

    // Skip unreachable nodes
    auto it = mySpfResult.find(nodeName);
    if (it == mySpfResult.end()) {
      LOG(ERROR) << "No route to " << nodeName
                 << ". Skipping considering this.";
      // skip if no route to node
      continue;
    }

    // Sanity check that OPENR_IGP_COST shouldn't exist
    if (MetricVectorUtils::getMetricEntityByType(
            prefixEntry.mv.value(),
            static_cast<int64_t>(thrift::MetricEntityType::OPENR_IGP_COST))) {
      LOG(ERROR) << "Received unexpected metric entity OPENR_IGP_COST in metric"
                 << " vector for prefix " << toString(prefix) << " from node "
                 << nodeName << ". Ignoring";
      continue;
    }

    // Copy is intentional - As we will need to augment metric vector with
    // IGP_COST
    thrift::MetricVector metricVector = prefixEntry.mv.value();

    // Associate IGP_COST to prefixEntry
    if (bgpUseIgpMetric_) {
      const auto igpMetric = static_cast<int64_t>(it->second.first);
      if (not ret.bestIgpMetric.hasValue() or
          *(ret.bestIgpMetric) > igpMetric) {
        ret.bestIgpMetric = igpMetric;
      }
      metricVector.metrics.emplace_back(MetricVectorUtils::createMetricEntity(
          static_cast<int64_t>(thrift::MetricEntityType::OPENR_IGP_COST),
          static_cast<int64_t>(thrift::MetricEntityPriority::OPENR_IGP_COST),
          thrift::CompareType::WIN_IF_NOT_PRESENT,
          false, /* isBestPathTieBreaker */
          /* lowest metric wins */
          {-1 * igpMetric}));
      VLOG(2) << "Attaching IGP metric of " << igpMetric << " to prefix "
              << toString(prefix) << " for node " << nodeName;
    }

    switch (ret.bestVector.hasValue()
                ? MetricVectorUtils::compareMetricVectors(
                      metricVector, *(ret.bestVector))
                : MetricVectorUtils::CompareResult::WINNER) {
    case MetricVectorUtils::CompareResult::WINNER:
      ret.nodes.clear();
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_WINNER:
      ret.bestVector = std::move(metricVector);
      ret.bestData = &(prefixEntry.data);
      ret.bestNode = nodeName;
      FOLLY_FALLTHROUGH;
    case MetricVectorUtils::CompareResult::TIE_LOOSER:
      ret.nodes.emplace(nodeName);
      break;
    case MetricVectorUtils::CompareResult::TIE:
      LOG(ERROR) << "Tie ordering prefix entries. Skipping route for prefix: "
                 << toString(prefix);
      return ret;
    case MetricVectorUtils::CompareResult::ERROR:
      LOG(ERROR) << "Error ordering prefix entries. Skipping route for prefix: "
                 << toString(prefix);
      return ret;
    default:
      break;
    }
  }
  ret.success = true;
  return ret;
}

folly::Optional<thrift::UnicastRoute>
SpfSolver::SpfSolverImpl::createBGPRoute(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes,
    bool const isV4) {
  std::string bestNode;
  // order is intended to comply with API used later.
  std::set<std::string> nodes;
  folly::Optional<thrift::MetricVector> bestVector{folly::none};

  const auto dstInfo = getBestAnnouncingNodes(
      myNodeName, prefix, nodePrefixes, isV4, true, false);
  if (not dstInfo.success) {
    return folly::none;
  }

  if (dstInfo.nodes.empty() or dstInfo.nodes.count(myNodeName)) {
    // do not program a route if we are advertising a best path to it or there
    // is no path to it
    if (not dstInfo.nodes.count(myNodeName)) {
      LOG(WARNING) << "No route to BGP prefix " << toString(prefix);
      tData_.addStatValue("decision.no_route_to_prefix", 1, fbzmq::COUNT);
    }
    return folly::none;
  }
  CHECK_NOTNULL(dstInfo.bestData);

  auto bestNextHop = prefixState_.getLoopbackVias(
      {dstInfo.bestNode}, isV4, dstInfo.bestIgpMetric);
  if (bestNextHop.size() != 1) {
    tData_.addStatValue("decision.missing_loopback_addr", 1, fbzmq::SUM);
    LOG(ERROR) << "Cannot find the best paths loopback address. "
               << "Skipping route for prefix: " << toString(prefix);
    return folly::none;
  }

  const auto nextHopsWithMetric =
      getNextHopsWithMetric(myNodeName, dstInfo.nodes, false);

  auto allNextHops = getNextHopsThrift(
      myNodeName,
      dstInfo.nodes,
      isV4,
      false,
      nextHopsWithMetric.first,
      nextHopsWithMetric.second,
      folly::none);

  return thrift::UnicastRoute{FRAGILE,
                              prefix,
                              thrift::AdminDistance::EBGP,
                              std::move(allNextHops),
                              thrift::PrefixType::BGP,
                              *(dstInfo.bestData),
                              bgpDryRun_, /* doNotInstall */
                              bestNextHop.at(0)};
}

folly::Optional<thrift::UnicastRoute>
SpfSolver::SpfSolverImpl::selectKsp2Routes(
    const thrift::IpPrefix& prefix,
    const string& myNodeName,
    BestPathCalResult const& bestPathCalResult,
    const std::unordered_map<std::string, std::vector<std::pair<Path, Metric>>>&
        routeToNodes,
    std::unordered_map<std::string, thrift::PrefixEntry> const& nodePrefixes) {
  thrift::UnicastRoute route;
  route.dest = prefix;

  std::vector<std::pair<Path, Metric>> paths;

  // get the shortest
  std::vector<std::pair<Path, Metric>> shortestRoutes;
  std::vector<std::pair<Path, Metric>> secShortestRoutes;

  // find shortest and sec shortest routes.
  for (const auto& node : bestPathCalResult.nodes) {
    auto routesIter = routeToNodes.find(node);
    if (routesIter == routeToNodes.end()) {
      continue;
    }
    Metric min_cost = std::numeric_limits<Metric>::max();

    for (const auto& path : routesIter->second) {
      min_cost = std::min(min_cost, path.second);
    }

    for (const auto& path : routeToNodes.at(node)) {
      if (path.second == min_cost) {
        shortestRoutes.push_back(path);
        continue;
      }
      secShortestRoutes.push_back(path);
    }
  }

  paths.reserve(shortestRoutes.size() + secShortestRoutes.size());
  paths.insert(paths.end(), shortestRoutes.begin(), shortestRoutes.end());

  // when get to second shortes routes, we want to make sure the shortest route
  // is not part of second shortest route to avoid double spraying issue
  for (const auto& secPath : secShortestRoutes) {
    bool add = true;
    for (const auto& firPath : shortestRoutes) {
      // this could happen for anycast VIPs.
      // for example, in a full mesh topology contains A, B and C. B and C both
      // annouce a prefix P. When A wants to talk to P, it's shortes paths are
      // A->B and A->C. And it is second shortest path is A->B->C and A->C->B.
      // In this case,  A->B->C containser A->B already, so we want to avoid
      // this.
      if (pathAInPathB(firPath.first, secPath.first)) {
        add = false;
        break;
      }
    }
    if (add) {
      paths.push_back(secPath);
    }
  }

  if (paths.size() == 0) {
    return folly::none;
  }

  for (const auto& pathAndCost : paths) {
    std::vector<int32_t> labels;
    for (auto& nodeLink : pathAndCost.first) {
      auto& otherNodeName = nodeLink.second->getOtherNodeName(nodeLink.first);
      labels.emplace_back(
          linkState_.getAdjacencyDatabases().at(otherNodeName).nodeLabel);
    }
    CHECK(labels.size());
    labels.pop_back(); // Remove first node's label to respect PHP

    // Create nexthop
    CHECK(pathAndCost.first.size());
    auto firstLink = pathAndCost.first.back().second;
    auto& pathCost = pathAndCost.second;
    folly::Optional<thrift::MplsAction> mplsAction;
    if (labels.size()) {
      mplsAction = createMplsAction(
          thrift::MplsActionCode::PUSH, folly::none, std::move(labels));
    }

    auto const& prefixStr = prefix.prefixAddress.addr;
    bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();

    route.nextHops.emplace_back(createNextHop(
        isV4Prefix ? firstLink->getNhV4FromNode(myNodeName)
                   : firstLink->getNhV6FromNode(myNodeName),
        firstLink->getIfaceFromNode(myNodeName),
        pathCost,
        mplsAction,
        true /* useNonShortestRoute */));
  }

  // if we have set minNexthop for prefix and # of nexthop didn't meet the
  // the threshold, we will ignore this route.
  auto minNextHop = getMinNextHopThreshold(bestPathCalResult, nodePrefixes);
  if (minNextHop.has_value() &&
      (minNextHop.value() > static_cast<int64_t>(route.nextHops.size()))) {
    LOG(WARNING) << "Dropping routes to " << toString(prefix) << " because of "
                 << route.nextHops.size() << " of nexthops is smaller than "
                 << minNextHop.value() << " /n route: /n" << toString(route);
    return folly::none;
  }

  if (bestPathCalResult.bestData != nullptr) {
    auto bestNextHop = prefixState_.getLoopbackVias(
        {bestPathCalResult.bestNode},
        prefix.prefixAddress.addr.size() == folly::IPAddressV4::byteCount(),
        bestPathCalResult.bestIgpMetric);
    if (bestNextHop.size() != 1) {
      return route;
    }
    route.data = *(bestPathCalResult.bestData);
    // in order to announce it back to BGP, we have to have the data
    route.prefixType = thrift::PrefixType::BGP;
    route.bestNexthop = bestNextHop[0];
  }

  return std::move(route);
}

std::unordered_map<std::string, std::vector<std::pair<Path, Metric>>>
SpfSolver::SpfSolverImpl::createOpenRKsp2EdRouteForNodes(
    std::string const& myNodeName,
    std::unordered_set<std::string> const& nodes) {
  std::unordered_map<std::string, std::vector<std::pair<Path, Metric>>>
      pathsToNodes;

  // Prepare list of possible destination nodes
  for (const auto& node : nodes) {
    std::set<std::string> dstNodeNames;
    dstNodeNames.emplace(node);

    // Step-1 Get all shortest paths and min-cost nodes to whom we will be
    // forwarding
    auto const& spf1 = spfResults_.at(myNodeName);
    auto const& minMetricNodes1 = getMinCostNodes(spf1, dstNodeNames);
    auto const& minCost1 = minMetricNodes1.first;
    auto const& minCostNodes1 = minMetricNodes1.second;

    // Step-2 Prepare set of links to ignore from subsequent SPF for finding
    //        second shortest paths. Collect all shortest paths
    LinkState::LinkSet linksToIgnore;
    for (auto const& minCostDst : minCostNodes1) {
      auto minPaths = traceEdgeDisjointPaths(myNodeName, minCostDst, spf1);
      for (auto& minPath : minPaths) {
        for (auto& nodeLink : minPath) {
          linksToIgnore.insert(nodeLink.second);
        }
        pathsToNodes[node].push_back(
            std::make_pair(std::move(minPath), minCost1));
      }
    }

    // Step-3 Collect all second shortest paths
    if (linksToIgnore.size()) {
      auto const spf2 = runSpf(myNodeName, true, linksToIgnore);
      auto const& minMetricNodes2 = getMinCostNodes(spf2, dstNodeNames);
      auto const& minCost2 = minMetricNodes2.first;
      auto const& minCostNodes2 = minMetricNodes2.second;
      for (auto const& minCostDst : minCostNodes2) {
        auto minPaths =
            traceEdgeDisjointPaths(myNodeName, minCostDst, spf2, linksToIgnore);
        for (auto& minPath : minPaths) {
          pathsToNodes[node].push_back(
              std::make_pair(std::move(minPath), minCost2));
        }
      }
    }
  }
  return pathsToNodes;
}

std::pair<Metric, std::unordered_set<std::string>>
SpfSolver::SpfSolverImpl::getMinCostNodes(
    const SpfResult& spfResult, const std::set<std::string>& dstNodeNames) {
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  // find the set of the closest nodes in our destination
  std::unordered_set<std::string> minCostNodes;
  for (const auto& dstNode : dstNodeNames) {
    auto it = spfResult.find(dstNode);
    if (it == spfResult.end()) {
      continue;
    }
    const auto nodeDistance = it->second.first;
    if (shortestMetric >= nodeDistance) {
      if (shortestMetric > nodeDistance) {
        shortestMetric = nodeDistance;
        minCostNodes.clear();
      }
      minCostNodes.emplace(dstNode);
    }
  }

  return std::make_pair(shortestMetric, std::move(minCostNodes));
}

std::pair<
    Metric /* min metric to destination */,
    std::unordered_map<
        std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
        Metric /* the distance from the nexthop to the dest */>>
SpfSolver::SpfSolverImpl::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<std::string>& dstNodeNames,
    bool perDestination) const {
  auto& shortestPathsFromHere = spfResults_.at(myNodeName);
  auto const& minMetricNodes =
      getMinCostNodes(shortestPathsFromHere, dstNodeNames);
  auto const& shortestMetric = minMetricNodes.first;
  auto const& minCostNodes = minMetricNodes.second;

  // build up next hop nodes both nodes that are along a shortest path to the
  // prefix and, if enabled, those with an LFA path to the prefix
  std::unordered_map<
      std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
      Metric /* the distance from the nexthop to the dest */>
      nextHopNodes;

  // If no node is reachable then return
  if (minCostNodes.empty()) {
    return std::make_pair(shortestMetric, nextHopNodes);
  }

  // Add neighbors with shortest path to the prefix
  for (const auto& dstNode : minCostNodes) {
    const auto dstNodeRef = perDestination ? dstNode : "";
    for (const auto& nhName : shortestPathsFromHere.at(dstNode).second) {
      nextHopNodes[std::make_pair(nhName, dstNodeRef)] =
          shortestMetric - findMinDistToNeighbor(myNodeName, nhName);
    }
  }

  // add any other neighbors that have LFA paths to the prefix
  if (computeLfaPaths_) {
    for (const auto& kv2 : spfResults_) {
      const auto& neighborName = kv2.first;
      const auto& shortestPathsFromNeighbor = kv2.second;
      if (neighborName == myNodeName) {
        continue;
      }

      const auto neighborToHere =
          shortestPathsFromNeighbor.at(myNodeName).first;
      for (const auto& dstNode : dstNodeNames) {
        auto shortestPathItr = shortestPathsFromNeighbor.find(dstNode);
        if (shortestPathItr == shortestPathsFromNeighbor.end()) {
          continue;
        }
        const auto distanceFromNeighbor = shortestPathItr->second.first;

        // This is the LFA condition per RFC 5286
        if (distanceFromNeighbor < shortestMetric + neighborToHere) {
          const auto nextHopKey =
              std::make_pair(neighborName, perDestination ? dstNode : "");
          auto nextHopItr = nextHopNodes.find(nextHopKey);
          if (nextHopItr == nextHopNodes.end()) {
            nextHopNodes.emplace(nextHopKey, distanceFromNeighbor);
          } else if (nextHopItr->second > distanceFromNeighbor) {
            nextHopItr->second = distanceFromNeighbor;
          }
        } // end if
      } // end for dstNodeNames
    } // end spfResults_
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

std::vector<thrift::NextHopThrift>
SpfSolver::SpfSolverImpl::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<std::string>& dstNodeNames,
    bool isV4,
    bool perDestination,
    const Metric minMetric,
    std::unordered_map<std::pair<std::string, std::string>, Metric>
        nextHopNodes,
    folly::Optional<int32_t> swapLabel) const {
  CHECK(not nextHopNodes.empty());

  std::vector<thrift::NextHopThrift> nextHops;
  for (const auto& link : linkState_.linksFromNode(myNodeName)) {
    for (const auto& dstNode :
         perDestination ? dstNodeNames : std::set<std::string>{""}) {
      const auto neighborNode = link->getOtherNodeName(myNodeName);
      const auto search =
          nextHopNodes.find(std::make_pair(neighborNode, dstNode));

      // Ignore overloaded links or nexthops
      if (search == nextHopNodes.end() or not link->isUp()) {
        continue;
      }

      // Ignore link if other side of link is one of our destination and we
      // are trying to send to dstNode via neighbor (who is also our
      // destination)
      if (not dstNode.empty() and dstNodeNames.count(neighborNode) and
          neighborNode != dstNode) {
        continue;
      }

      // Ignore nexthops that are not shortest if lfa is disabled. All links
      // towards the nexthop on shortest path are LFA routes.
      Metric distOverLink =
          link->getMetricFromNode(myNodeName) + search->second;
      if (not computeLfaPaths_ and distOverLink != minMetric) {
        continue;
      }

      // Create associated mpls action if swapLabel is provided
      folly::Optional<thrift::MplsAction> mplsAction;
      if (swapLabel.hasValue()) {
        CHECK(not mplsAction.hasValue());
        const bool isNextHopAlsoDst = dstNodeNames.count(neighborNode);
        mplsAction = createMplsAction(
            isNextHopAlsoDst ? thrift::MplsActionCode::PHP
                             : thrift::MplsActionCode::SWAP,
            isNextHopAlsoDst ? folly::none : swapLabel);
      }

      // Create associated mpls action if dest node is not empty and destination
      // is not our neighbor
      if (not dstNode.empty() and dstNode != neighborNode) {
        // Validate mpls label before adding mplsAction
        auto const dstNodeLabel =
            linkState_.getAdjacencyDatabases().at(dstNode).nodeLabel;
        if (not isMplsLabelValid(dstNodeLabel)) {
          continue;
        }
        CHECK(not mplsAction.hasValue());
        mplsAction = createMplsAction(
            thrift::MplsActionCode::PUSH,
            folly::none,
            std::vector<int32_t>{dstNodeLabel});
      }

      // if we are computing LFA paths, any nexthop to the node will do
      // otherwise, we only want those nexthops along a shortest path
      nextHops.emplace_back(createNextHop(
          isV4 ? link->getNhV4FromNode(myNodeName)
               : link->getNhV6FromNode(myNodeName),
          link->getIfaceFromNode(myNodeName),
          distOverLink,
          mplsAction));
    } // end for perDestination ...
  } // end for linkState_ ...

  return nextHops;
}

Metric
SpfSolver::SpfSolverImpl::findMinDistToNeighbor(
    const std::string& myNodeName, const std::string& neighborName) const {
  Metric min = std::numeric_limits<Metric>::max();
  for (const auto& link : linkState_.linksFromNode(myNodeName)) {
    if (link->isUp() && link->getOtherNodeName(myNodeName) == neighborName) {
      min = std::min(link->getMetricFromNode(myNodeName), min);
    }
  }
  return min;
}

std::unordered_map<std::string, int64_t>
SpfSolver::SpfSolverImpl::getCounters() {
  using NodeIface = std::pair<std::string, std::string>;

  // Create adjacencies
  std::unordered_map<NodeIface, std::unordered_set<NodeIface>> adjs;
  size_t numPartialAdjacencies{0};
  for (auto const& kv : linkState_.getAdjacencyDatabases()) {
    const auto& adjDb = kv.second;
    size_t numLinks = linkState_.linksFromNode(kv.first).size();
    // Number of links (bi-directional) must be <= number of adjacencies
    CHECK_LE(numLinks, adjDb.adjacencies.size());
    numPartialAdjacencies += adjDb.adjacencies.size() - numLinks;
  }

  // Get stats from tData_
  auto counters = tData_.getCounters();

  // Add custom counters
  counters["decision.num_partial_adjacencies"] = numPartialAdjacencies;
  counters["decision.num_complete_adjacencies"] = linkState_.numLinks();
  // When node has no adjacencies then linkState reports 0
  counters["decision.num_nodes"] =
      std::max(linkState_.numNodes(), static_cast<size_t>(1ul));
  counters["decision.num_prefixes"] = prefixState_.prefixes().size();
  counters["decision.num_nodes_v4_loopbacks"] =
      prefixState_.getNodeHostLoopbacksV4().size();
  counters["decision.num_nodes_v6_loopbacks"] =
      prefixState_.getNodeHostLoopbacksV6().size();

  return counters;
}

//
// Public SpfSolver
//

SpfSolver::SpfSolver(
    const std::string& myNodeName,
    bool enableV4,
    bool computeLfaPaths,
    bool enableOrderedFib,
    bool bgpDryRun,
    bool bgpUseIgpMetric)
    : impl_(new SpfSolver::SpfSolverImpl(
          myNodeName,
          enableV4,
          computeLfaPaths,
          enableOrderedFib,
          bgpDryRun,
          bgpUseIgpMetric)) {}

SpfSolver::~SpfSolver() {}

// update adjacencies for the given router; everything is replaced
std::pair<
    bool /* topology has changed*/,
    bool /* route attributes has changed (nexthop addr, node/adj label */>
SpfSolver::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb) {
  return impl_->updateAdjacencyDatabase(newAdjacencyDb);
}

bool
SpfSolver::hasHolds() const {
  return impl_->hasHolds();
}

bool
SpfSolver::deleteAdjacencyDatabase(const std::string& nodeName) {
  return impl_->deleteAdjacencyDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase> const&
SpfSolver::getAdjacencyDatabases() {
  return impl_->getAdjacencyDatabases();
}

// update prefixes for a given router
bool
SpfSolver::updatePrefixDatabase(const thrift::PrefixDatabase& prefixDb) {
  return impl_->updatePrefixDatabase(prefixDb);
}

bool
SpfSolver::deletePrefixDatabase(const std::string& nodeName) {
  return impl_->deletePrefixDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolver::getPrefixDatabases() {
  return impl_->getPrefixDatabases();
}

folly::Optional<thrift::RouteDatabase>
SpfSolver::buildPaths(const std::string& myNodeName) {
  return impl_->buildPaths(myNodeName);
}

folly::Optional<thrift::RouteDatabase>
SpfSolver::buildRouteDb(const std::string& myNodeName) {
  return impl_->buildRouteDb(myNodeName);
}

bool
SpfSolver::decrementHolds() {
  return impl_->decrementHolds();
}

std::unordered_map<std::string, int64_t>
SpfSolver::getCounters() {
  return impl_->getCounters();
}

//
// Decision class implementation
//

Decision::Decision(
    std::string myNodeName,
    bool enableV4,
    bool computeLfaPaths,
    bool enableOrderedFib,
    bool bgpDryRun,
    bool bgpUseIgpMetric,
    const AdjacencyDbMarker& adjacencyDbMarker,
    const PrefixDbMarker& prefixDbMarker,
    std::chrono::milliseconds debounceMinDur,
    std::chrono::milliseconds debounceMaxDur,
    folly::Optional<std::chrono::seconds> gracefulRestartDuration,
    const KvStoreLocalCmdUrl& storeCmdUrl,
    const KvStoreLocalPubUrl& storePubUrl,
    const DecisionPubUrl& decisionPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : OpenrEventLoop(myNodeName, thrift::OpenrModuleType::DECISION, zmqContext),
      processUpdatesBackoff_(debounceMinDur, debounceMaxDur),
      myNodeName_(myNodeName),
      adjacencyDbMarker_(adjacencyDbMarker),
      prefixDbMarker_(prefixDbMarker),
      storeCmdUrl_(storeCmdUrl),
      storePubUrl_(storePubUrl),
      decisionPubUrl_(decisionPubUrl),
      storeSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      decisionPub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}) {
  routeDb_.thisNodeName = myNodeName_;
  processUpdatesTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { processPendingUpdates(); });
  spfSolver_ = std::make_unique<SpfSolver>(
      myNodeName,
      enableV4,
      computeLfaPaths,
      enableOrderedFib,
      bgpDryRun,
      bgpUseIgpMetric);

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  coldStartTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { coldStartUpdate(); });
  if (gracefulRestartDuration.hasValue()) {
    coldStartTimer_->scheduleTimeout(gracefulRestartDuration.value());
  }

  prepare(zmqContext, enableOrderedFib);
}

void
Decision::prepare(fbzmq::Context& zmqContext, bool enableOrderedFib) noexcept {
  const auto pubBind = decisionPub_.bind(fbzmq::SocketUrl{decisionPubUrl_});
  if (pubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << decisionPubUrl_ << "' "
               << pubBind.error();
  }

  VLOG(2) << "Decision: Connecting to store '" << storePubUrl_ << "'";
  const auto optRet =
      storeSub_.setSockOpt(ZMQ_RCVHWM, &kStoreSubReceiveHwm, sizeof(int));
  if (optRet.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_RCVHWM to " << kStoreSubReceiveHwm << " "
               << optRet.error();
  }
  const auto subConnect = storeSub_.connect(fbzmq::SocketUrl{storePubUrl_});
  if (subConnect.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << storePubUrl_ << "' "
               << subConnect.error();
  }
  const auto subRet = storeSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (subRet.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << subRet.error();
  }

  VLOG(2) << "Decision thread attaching socket/event callbacks...";

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Attach callback for processing publications on storeSub_ socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*storeSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "Decision: publication received...";

        auto maybeThriftPub =
            storeSub_.recvThriftObj<thrift::Publication>(serializer_);
        if (maybeThriftPub.hasError()) {
          LOG(ERROR) << "Error processing KvStore publication: "
                     << maybeThriftPub.error();
          return;
        }

        // Apply publication and update stored update status
        ProcessPublicationResult res; // default initialized to false
        try {
          res = processPublication(maybeThriftPub.value());
        } catch (const std::exception& e) {
#if FOLLY_USE_SYMBOLIZER
          // collect stack strace then fail the process
          for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
            LOG(ERROR) << exInfo;
          }
#endif
          // FATAL to produce core dump
          LOG(FATAL) << "Exception occured in Decision::processPublication - "
                     << folly::exceptionStr(e);
        }
        processUpdatesStatus_.adjChanged |= res.adjChanged;
        processUpdatesStatus_.prefixesChanged |= res.prefixesChanged;
        // compute routes with exponential backoff timer if needed
        if (res.adjChanged || res.prefixesChanged) {
          if (!processUpdatesBackoff_.atMaxBackoff()) {
            processUpdatesBackoff_.reportError();
            processUpdatesTimer_->scheduleTimeout(
                processUpdatesBackoff_.getTimeRemainingUntilRetry());
          } else {
            CHECK(processUpdatesTimer_->isScheduled());
          }
        }
      });

  // Schedule periodic timer to decremtOrderedFibHolds
  if (enableOrderedFib) {
    orderedFibTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
      LOG(INFO) << "Decrementing Holds";
      decrementOrderedFibHolds();
      if (spfSolver_->hasHolds()) {
        auto timeout = getMaxFib();
        LOG(INFO) << "Scheduling next hold decrement in " << timeout.count()
                  << "ms";
        orderedFibTimer_->scheduleTimeout(getMaxFib());
      }
    });
  }

  auto zmqContextPtr = &zmqContext;
  scheduleTimeout(std::chrono::milliseconds(0), [this, zmqContextPtr] {
    initialSync(*zmqContextPtr);
  });
}

folly::Expected<fbzmq::Message, fbzmq::Error>
Decision::processRequestMsg(fbzmq::Message&& request) {
  auto maybeThriftReq =
      request.readThriftObj<thrift::DecisionRequest>(serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "Decision: Error processing request on REP socket: "
               << maybeThriftReq.error();
    return folly::makeUnexpected(fbzmq::Error());
  }

  auto thriftReq = maybeThriftReq.value();
  thrift::DecisionReply reply;
  switch (thriftReq.cmd) {
  case thrift::DecisionCommand::ROUTE_DB_GET: {
    auto nodeName = thriftReq.nodeName;
    if (nodeName.empty()) {
      nodeName = myNodeName_;
    }

    auto maybeRouteDb = spfSolver_->buildPaths(nodeName);
    if (maybeRouteDb.hasValue()) {
      reply.routeDb = std::move(maybeRouteDb.value());
    } else {
      reply.routeDb.thisNodeName = nodeName;
    }
    break;
  }

  case thrift::DecisionCommand::ADJ_DB_GET: {
    reply.adjDbs = spfSolver_->getAdjacencyDatabases();
    break;
  }

  case thrift::DecisionCommand::PREFIX_DB_GET: {
    reply.prefixDbs = spfSolver_->getPrefixDatabases();
    break;
  }

  default: {
    LOG(ERROR) << "Unexpected command received: "
               << folly::get_default(
                      thrift::_DecisionCommand_VALUES_TO_NAMES,
                      thriftReq.cmd,
                      "UNKNOWN");
    return folly::makeUnexpected(fbzmq::Error());
  }
  }

  return fbzmq::Message::fromThriftObj(reply, serializer_);
}

std::unordered_map<std::string, int64_t>
Decision::getCounters() {
  return spfSolver_->getCounters();
}

thrift::PrefixDatabase
Decision::updateNodePrefixDatabase(
    const std::string& key, const thrift::PrefixDatabase& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;

  auto prefixKey = PrefixKey::fromStr(key);
  if (!prefixKey.hasValue()) {
    // either node will advertise per prefix or entire prefix DB,
    // not a combination. Erase from nodePrefixDatabase_ if node switches format
    nodePrefixDatabase_.erase(nodeName);
    return prefixDb;
  }

  auto& nodePrefixEntries = nodePrefixDatabase_[nodeName];
  if (prefixDb.prefixEntries.size()) {
    LOG_IF(ERROR, prefixDb.prefixEntries.size() > 1)
        << "Received more than one prefix, only the first prefix is processed";
    if (prefixDb.deletePrefix) {
      nodePrefixEntries.erase(prefixDb.prefixEntries[0].prefix);
    } else {
      nodePrefixEntries[prefixDb.prefixEntries[0].prefix] =
          prefixDb.prefixEntries[0];
    }
  }

  thrift::PrefixDatabase nodePrefixDb;
  nodePrefixDb.thisNodeName = nodeName;
  nodePrefixDb.perfEvents = prefixDb.perfEvents;
  nodePrefixDb.perPrefixKey = true;

  // create and return prefixDB for the node
  for (const auto& prefix : nodePrefixEntries) {
    nodePrefixDb.prefixEntries.emplace_back(prefix.second);
  }
  return nodePrefixDb;
}

ProcessPublicationResult
Decision::processPublication(thrift::Publication const& thriftPub) {
  ProcessPublicationResult res;

  // LSDB addition/update
  // deserialize contents of every LSDB key

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals.empty() and thriftPub.expiredKeys.empty()) {
    return res;
  }

  for (const auto& kv : thriftPub.keyVals) {
    const auto& key = kv.first;
    const auto& rawVal = kv.second;
    std::string nodeName = getNodeNameFromKey(key);

    if (not rawVal.value.hasValue()) {
      // skip TTL update
      DCHECK(rawVal.ttlVersion > 0);
      continue;
    }

    try {
      if (key.find(adjacencyDbMarker_) == 0) {
        // update adjacencyDb
        auto adjacencyDb =
            fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
                rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, adjacencyDb.thisNodeName);
        auto rc = spfSolver_->updateAdjacencyDatabase(adjacencyDb);
        if (rc.first) {
          res.adjChanged = true;
          pendingAdjUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
        }
        if (rc.second && nodeName == myNodeName_) {
          // route attribute chanegs only matter for the local node
          res.prefixesChanged = true;
          pendingPrefixUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
        }
        if (spfSolver_->hasHolds() && orderedFibTimer_ != nullptr &&
            !orderedFibTimer_->isScheduled()) {
          orderedFibTimer_->scheduleTimeout(getMaxFib());
        }
        continue;
      }

      if (key.find(prefixDbMarker_) == 0) {
        // update prefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, prefixDb.thisNodeName);
        auto nodePrefixDb = updateNodePrefixDatabase(key, prefixDb);
        if (spfSolver_->updatePrefixDatabase(nodePrefixDb)) {
          res.prefixesChanged = true;
          pendingPrefixUpdates_.addUpdate(myNodeName_, nodePrefixDb.perfEvents);
        }
        continue;
      }

      if (key.find(Constants::kFibTimeMarker.toString()) == 0) {
        try {
          std::chrono::milliseconds fibTime{stoll(rawVal.value.value())};
          fibTimes_[nodeName] = fibTime;
        } catch (...) {
          LOG(ERROR) << "Could not convert "
                     << Constants::kFibTimeMarker.toString()
                     << " value to int64";
        }
        continue;
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Failed to deserialize info for key " << key
                 << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  for (const auto& key : thriftPub.expiredKeys) {
    std::string prefix, nodeName;
    folly::split(
        Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

    if (key.find(adjacencyDbMarker_) == 0) {
      if (spfSolver_->deleteAdjacencyDatabase(nodeName)) {
        res.adjChanged = true;
        pendingAdjUpdates_.addUpdate(myNodeName_, folly::none);
      }
      continue;
    }

    if (key.find(prefixDbMarker_) == 0) {
      auto prefixStr = PrefixKey::fromStr(key);
      if (prefixStr.hasValue()) {
        // delete single prefix from the prefix DB for a given node
        thrift::PrefixDatabase deletePrefixDb;
        deletePrefixDb.prefixEntries.emplace_back(
            createPrefixEntry(prefixStr.value().getIpPrefix()));
        deletePrefixDb.thisNodeName = prefixStr.value().getNodeName();
        deletePrefixDb.deletePrefix = true;
        auto nodePrefixDb = updateNodePrefixDatabase(key, deletePrefixDb);
        if (spfSolver_->updatePrefixDatabase(nodePrefixDb)) {
          res.prefixesChanged = true;
        }
      } else {
        if (spfSolver_->deletePrefixDatabase(nodeName)) {
          res.prefixesChanged = true;
          pendingPrefixUpdates_.addUpdate(myNodeName_, folly::none);
        }
      }
      continue;
    }
  }

  return res;
}

// perform full dump of all LSDBs and run initial routing computations
void
Decision::initialSync(fbzmq::Context& zmqContext) {
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> storeReq(zmqContext);

  // we'll be using this to get the full dump from the KvStore
  const auto reqConnect = storeReq.connect(fbzmq::SocketUrl{storeCmdUrl_});
  if (reqConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << storeCmdUrl_ << "' "
               << reqConnect.error();
  }

  thrift::KvStoreRequest thriftReq;
  thrift::KeyDumpParams params;

  thriftReq.cmd = thrift::Command::KEY_DUMP;
  thriftReq.keyDumpParams = params;

  storeReq.sendThriftObj(thriftReq, serializer_);

  VLOG(2) << "Decision process requesting initial state...";

  // receive the full dump of the database
  auto maybeThriftPub = storeReq.recvThriftObj<thrift::Publication>(
      serializer_, Constants::kReadTimeout);
  if (maybeThriftPub.hasError()) {
    LOG(ERROR) << "Error processing KvStore publication: "
               << maybeThriftPub.error();
    return;
  }

  // Process publication and immediately apply updates
  auto const& ret = processPublication(maybeThriftPub.value());
  if (ret.adjChanged) {
    // Graph changes
    processPendingAdjUpdates();
  } else if (ret.prefixesChanged) {
    // Only Prefix changes, no graph changes
    processPendingPrefixUpdates();
  }
}

// periodically submit counters to Counters thread
void
Decision::submitCounters() {
  VLOG(3) << "Submitting counters...";

  // Prepare for submitting counters
  auto counters = spfSolver_->getCounters();
  counters["decision.zmq_event_queue_size"] = getEventQueueSize();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
Decision::processPendingUpdates() {
  if (processUpdatesStatus_.adjChanged) {
    processPendingAdjUpdates();
  } else if (processUpdatesStatus_.prefixesChanged) {
    processPendingPrefixUpdates();
  }

  // reset update status
  processUpdatesStatus_.adjChanged = false;
  processUpdatesStatus_.prefixesChanged = false;

  // update decision debounce flag
  processUpdatesBackoff_.reportSuccess();
}

void
Decision::processPendingAdjUpdates() {
  VLOG(1) << "Decision: processing " << pendingAdjUpdates_.getCount()
          << " accumulated adjacency updates.";

  if (!pendingAdjUpdates_.getCount()) {
    LOG(ERROR) << "Decision route computation triggered without any pending "
               << "adjacency updates.";
    return;
  }

  // Retrieve perf events, add debounce perf event, log information to
  // ZmqMonitor, ad and clear pending updates
  auto maybePerfEvents = pendingAdjUpdates_.getPerfEvents();
  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "DECISION_DEBOUNCE");
    auto const& events = maybePerfEvents->events;
    auto const& eventsCnt = events.size();
    CHECK_LE(2, eventsCnt);
    auto duration = events[eventsCnt - 1].unixTs - events[eventsCnt - 2].unixTs;
    VLOG(1) << "Debounced " << pendingAdjUpdates_.getCount() << " events over "
            << std::chrono::milliseconds(duration).count() << "ms.";
  }
  pendingAdjUpdates_.clear();

  if (coldStartTimer_->isScheduled()) {
    return;
  }

  // run SPF once for all updates received
  LOG(INFO) << "Decision: computing new paths.";
  auto maybeRouteDb = spfSolver_->buildPaths(myNodeName_);
  if (not maybeRouteDb.hasValue()) {
    LOG(WARNING) << "AdjacencyDb updates incurred no route updates";
    return;
  }

  maybeRouteDb.value().perfEvents = maybePerfEvents;
  sendRouteUpdate(maybeRouteDb.value(), "DECISION_SPF");
}

void
Decision::processPendingPrefixUpdates() {
  auto maybePerfEvents = pendingPrefixUpdates_.getPerfEvents();
  pendingPrefixUpdates_.clear();
  if (coldStartTimer_->isScheduled()) {
    return;
  }

  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "DECISION_DEBOUNCE");
  }
  // update routeDb once for all updates received
  LOG(INFO) << "Decision: updating new routeDb.";
  auto maybeRouteDb = spfSolver_->buildRouteDb(myNodeName_);
  if (not maybeRouteDb.hasValue()) {
    LOG(WARNING) << "PrefixDb updates incurred no route updates";
    return;
  }

  maybeRouteDb.value().perfEvents = maybePerfEvents;
  sendRouteUpdate(maybeRouteDb.value(), "ROUTE_UPDATE");
}

void
Decision::decrementOrderedFibHolds() {
  if (spfSolver_->decrementHolds()) {
    if (coldStartTimer_->isScheduled()) {
      return;
    }
    auto maybeRouteDb = spfSolver_->buildPaths(myNodeName_);
    if (not maybeRouteDb.hasValue()) {
      LOG(INFO) << "decrementOrderedFibHolds incurred no route updates";
      return;
    }

    // Create empty perfEvents list. In this case we don't this route update to
    // be inculded in the Fib time
    maybeRouteDb.value().perfEvents = thrift::PerfEvents{};
    sendRouteUpdate(maybeRouteDb.value(), "ORDERED_FIB_HOLDS_EXPIRED");
  }
}

void
Decision::coldStartUpdate() {
  auto maybeRouteDb = spfSolver_->buildPaths(myNodeName_);
  if (not maybeRouteDb.hasValue()) {
    LOG(ERROR) << "SEVERE: No routes to program after cold start duration. "
               << "Sending empty route db to FIB";
    thrift::RouteDatabase db;
    sendRouteUpdate(db, "COLD_START_UPDATE");
    return;
  }
  // Create empty perfEvents list. In this case we don't this route update to
  // be inculded in the Fib time
  maybeRouteDb.value().perfEvents = thrift::PerfEvents{};
  sendRouteUpdate(maybeRouteDb.value(), "COLD_START_UPDATE");
}

void
Decision::sendRouteUpdate(
    thrift::RouteDatabase& db, std::string const& eventDescription) {
  if (db.perfEvents.hasValue()) {
    addPerfEvent(db.perfEvents.value(), myNodeName_, eventDescription);
  }

  // Find out delta to be sent to Fib
  auto routeDelta = findDeltaRoutes(db, routeDb_);
  routeDelta.perfEvents = db.perfEvents;
  routeDb_ = std::move(db);

  // publish the new route state
  auto sendRc = decisionPub_.sendThriftObj(routeDelta, serializer_);
  if (sendRc.hasError()) {
    LOG(ERROR) << "Error publishing new routing table: " << sendRc.error();
  }
}

std::chrono::milliseconds
Decision::getMaxFib() {
  std::chrono::milliseconds maxFib{1};
  for (auto& kv : fibTimes_) {
    maxFib = std::max(maxFib, kv.second);
  }
  return maxFib;
}

} // namespace openr
