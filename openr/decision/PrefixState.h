/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <unordered_map>
#include <vector>

#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>

namespace openr {
class PrefixState {
 public:
  std::unordered_map<
      thrift::IpPrefix,
      std::unordered_map<std::string, thrift::PrefixEntry>> const&
  prefixes() const {
    return prefixes_;
  }

  // update loopback prefix deletes
  void deleteLoopbackPrefix(
      thrift::IpPrefix const& prefix, const std::string& nodename);

  // returns true if the prefixDb changed
  bool updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb);

  // returns true if the PrefixDatabase existed
  bool deletePrefixDatabase(const std::string& nodeName);

  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases() const;

  std::vector<thrift::NextHopThrift> getLoopbackVias(
      std::unordered_set<std::string> const& nodes,
      bool const isV4,
      folly::Optional<int64_t> const& igpMetric) const;

  std::unordered_map<std::string, thrift::BinaryAddress> const&
  getNodeHostLoopbacksV4() const {
    return nodeHostLoopbacksV4_;
  }

  std::unordered_map<std::string, thrift::BinaryAddress> const&
  getNodeHostLoopbacksV6() const {
    return nodeHostLoopbacksV6_;
  }

 private:
  // For each prefix in the network, stores a set of nodes that advertise it
  std::unordered_map<
      thrift::IpPrefix,
      std::unordered_map<std::string, thrift::PrefixEntry>>
      prefixes_;
  std::unordered_map<std::string, std::set<thrift::IpPrefix>> nodeToPrefixes_;
  std::unordered_map<std::string, thrift::BinaryAddress> nodeHostLoopbacksV4_;
  std::unordered_map<std::string, thrift::BinaryAddress> nodeHostLoopbacksV6_;
  // maintain list of nodes that advertised per prefix keys
  std::unordered_map<std::string, bool> nodePerPrefixKey_;
}; // class PrefixState

} // namespace openr
