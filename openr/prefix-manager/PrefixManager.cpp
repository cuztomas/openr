/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <folly/futures/Future.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

namespace {
// key for the persist config on disk
const std::string kConfigKey{"prefix-manager-config"};
// various error messages
const std::string kErrorNoChanges{"No changes in prefixes to be advertised"};
const std::string kErrorNoPrefixToRemove{"No prefix to remove"};
const std::string kErrorNoPrefixesOfType{"No prefixes of type"};
const std::string kErrorUnknownCommand{"Unknown command"};
} // namespace

PrefixManager::PrefixManager(
    const std::string& nodeId,
    const PersistentStoreUrl& persistentStoreUrl,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const PrefixDbMarker& prefixDbMarker,
    bool perPrefixKeys,
    bool enablePerfMeasurement,
    const std::chrono::seconds prefixHoldTime,
    const std::chrono::milliseconds ttlKeyInKvStore,
    fbzmq::Context& zmqContext)
    : OpenrEventLoop(
          nodeId, thrift::OpenrModuleType::PREFIX_MANAGER, zmqContext),
      nodeId_(nodeId),
      configStoreClient_{persistentStoreUrl, zmqContext},
      prefixDbMarker_{prefixDbMarker},
      perPrefixKeys_{perPrefixKeys},
      enablePerfMeasurement_{enablePerfMeasurement},
      prefixHoldUntilTimePoint_(
          std::chrono::steady_clock::now() + prefixHoldTime),
      ttlKeyInKvStore_(ttlKeyInKvStore),
      kvStoreClient_{
          zmqContext, this, nodeId_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl} {
  // pick up prefixes from disk
  auto maybePrefixDb =
      configStoreClient_.loadThriftObj<thrift::PrefixDatabase>(kConfigKey);
  if (maybePrefixDb.hasValue()) {
    LOG(INFO) << "Successfully loaded " << maybePrefixDb->prefixEntries.size()
              << " prefixes from disk";
    for (const auto& entry : maybePrefixDb.value().prefixEntries) {
      LOG(INFO) << "  > " << toString(entry.prefix) << ", type "
                << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                       entry.type);
      prefixMap_[entry.prefix].emplace(entry.type, entry);
    }
    // Prefixes will be advertised after prefixHoldUntilTimePoint_
  }

  // register kvstore publication callback
  std::vector<std::string> keyPrefixList;
  keyPrefixList.emplace_back(folly::sformat(
      "{}{}", static_cast<std::string>(prefixDbMarker_), nodeId_));
  std::set<std::string> originatorIds{};
  KvStoreFilters kvFilters = KvStoreFilters(keyPrefixList, originatorIds);
  kvStoreClient_.subscribeKeyFilter(
      std::move(kvFilters),
      [this](
          const std::string& key,
          folly::Optional<thrift::Value> thriftVal) noexcept {
        processKeyPrefixUpdate(key, thriftVal);
      });

  // Create throttled updateKvStore
  updateKvStoreThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      this, Constants::kPrefixMgrKvThrottleTimeout, [this]() noexcept {
        updateKvStore();
      });

  // Create a timer to update all prefixes after HoldTime (2 * KA) during
  // initial start up
  // Holdtime zero is used during testing to do inline without delay
  if (prefixHoldTime != std::chrono::seconds(0)) {
    scheduleTimeoutAt(prefixHoldUntilTimePoint_, [this]() {
      persistPrefixDb();
      if (perPrefixKeys_) {
        // advertise all prefixes
        for (const auto& kv : prefixMap_) {
          prefixesToUpdate_.emplace(kv.first);
        }
      }
      updateKvStore();
    });

    // Cancel throttle as we are publishing latest state
    if (updateKvStoreThrottled_->isActive()) {
      updateKvStoreThrottled_->cancel();
    }
  }

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);
}

void
PrefixManager::processKeyPrefixUpdate(
    std::string const& key, folly::Optional<thrift::Value> value) noexcept {
  LOG(INFO) << nodeId_ << ": Received update for " << key;
  if (!value.hasValue()) {
    return;
  }
  auto prefixKey = PrefixKey::fromStr(key);
  if (prefixKey.hasValue()) {
    auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
        value.value().value.value(), serializer_);

    CHECK_EQ(prefixDb.prefixEntries.size(), 1);
    auto ipPrefix = prefixDb.prefixEntries[0];
    auto it = prefixMap_.find(ipPrefix.prefix);
    if (it == prefixMap_.end()) {
      // Got an update for a key origninated by this node and that is not
      // found in the prefix map. If the prefix DB is not set to delete
      // then it is a stale update that must be overridden by
      // advertising a prefix DB with deletePrefix set to True
      if (!prefixDb.deletePrefix) {
        LOG(INFO) << "Stale Prefix update received for non existing entry "
                  << toString(ipPrefix.prefix)
                  << ". Advertising delete prefix DB";
        advertisePrefixWithdraw(ipPrefix);
      }
    } else {
      // Prefix entry exists in prefix manager. We should not be here since
      // active prefix entry will be in persistent DB. But send a prefix key
      // update just in case.
      advertisePrefix(it->second.begin()->second);
    }
  } else {
    // old key format, send prefix key update
    updateKvStore();
  }
}

void
PrefixManager::persistPrefixDb() {
  if (std::chrono::steady_clock::now() < prefixHoldUntilTimePoint_) {
    // Too early for updating persistent file. Let timeout handle it
    return;
  }

  // prefixDb persistent entries have changed,
  // save the newest persistent entries to disk.
  thrift::PrefixDatabase persistentPrefixDb;
  persistentPrefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    for (const auto& kv2 : kv.second) {
      if (not kv2.second.ephemeral.value_or(false)) {
        persistentPrefixDb.prefixEntries.emplace_back(kv2.second);
      }
    }
  }

  auto ret = configStoreClient_.storeThriftObj(kConfigKey, persistentPrefixDb);
  if (ret.hasError()) {
    LOG(ERROR) << "Error saving persistent prefixDb to file. " << ret.error();
  }
}

void
PrefixManager::advertisePrefixWithdraw(const thrift::PrefixEntry& prefixEntry) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  prefixDb.prefixEntries = {prefixEntry};
  prefixDb.deletePrefix = true;
  auto prefixKey = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  VLOG(1) << "Withdrawing prefix " << prefixKey.getPrefixKey()
          << " from KvStore";
  kvStoreClient_.clearKey(
      prefixKey.getPrefixKey(),
      serializePrefixDb(std::move(prefixDb)),
      ttlKeyInKvStore_);
}

void
PrefixManager::advertisePrefix(const thrift::PrefixEntry& prefixEntry) {
  /* code */
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  prefixDb.prefixEntries.emplace_back(prefixEntry);

  const auto prefixKey = PrefixKey(
      nodeId_,
      folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  VLOG(1) << "Advertising prefix " << prefixKey.getPrefixKey()
          << " to KvStore ";
  kvStoreClient_.persistKey(
      prefixKey.getPrefixKey(),
      serializePrefixDb(std::move(prefixDb)),
      ttlKeyInKvStore_);
}

void
PrefixManager::updateKvStorePrefixKeys() {
  // Incremental prefix updates, either add or delete from kvstore
  // Check prefixMap_ to decide whether to add or delete
  for (const auto& ipPrefix : prefixesToUpdate_) {
    auto it = prefixMap_.find(ipPrefix);
    if (it == prefixMap_.end()) {
      thrift::PrefixEntry prefixEntry;
      prefixEntry.prefix = ipPrefix;
      advertisePrefixWithdraw(prefixEntry);
    } else {
      advertisePrefix(it->second.begin()->second);
    }
  }
  prefixesToUpdate_.clear();
}

void
PrefixManager::updateKvStore() {
  if (std::chrono::steady_clock::now() < prefixHoldUntilTimePoint_) {
    // Too early for advertising my own prefixes. Let timeout advertise it
    // and skip here.
    return;
  }

  // prefixDb has changed.
  if (perPrefixKeys_) {
    return updateKvStorePrefixKeys();
  }
  // Update the kvstore with both persistent and ephemeral entries
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    prefixDb.prefixEntries.emplace_back(kv.second.begin()->second);
  }

  const auto prefixDbKey = folly::sformat(
      "{}{}", static_cast<std::string>(prefixDbMarker_), nodeId_);
  LOG(INFO) << "Updating all " << prefixDb.prefixEntries.size()
            << " prefixes in KvStore " << prefixDbKey;
  kvStoreClient_.persistKey(
      prefixDbKey, serializePrefixDb(std::move(prefixDb)), ttlKeyInKvStore_);
}

folly::Expected<fbzmq::Message, fbzmq::Error>
PrefixManager::processRequestMsg(fbzmq::Message&& request) {
  const auto maybeThriftReq =
      request.readThriftObj<thrift::PrefixManagerRequest>(serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::PrefixRequest "
               << maybeThriftReq.error();
    return folly::makeUnexpected(fbzmq::Error());
  }

  const auto& thriftReq = maybeThriftReq.value();
  thrift::PrefixManagerResponse response;
  bool persistentEntryChange = false;
  bool kvStoreChange = false;
  switch (thriftReq.cmd) {
  case thrift::PrefixManagerCommand::ADD_PREFIXES: {
    tData_.addStatValue("prefix_manager.add_prefixes", 1, fbzmq::COUNT);
    if (isAnyInputPrefixPersistent(thriftReq.prefixes)) {
      persistentEntryChange = true;
    }
    if (addOrUpdatePrefixes(thriftReq.prefixes)) {
      kvStoreChange = true;
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoChanges;
    }

    break;
  }
  case thrift::PrefixManagerCommand::WITHDRAW_PREFIXES: {
    if (isAnyExistingPrefixPersistent(thriftReq.prefixes)) {
      persistentEntryChange = true;
    }
    if (removePrefixes(thriftReq.prefixes)) {
      kvStoreChange = true;
      response.success = true;
      tData_.addStatValue("prefix_manager.withdraw_prefixes", 1, fbzmq::COUNT);
    } else {
      response.success = false;
      response.message = kErrorNoPrefixToRemove;
    }
    break;
  }
  case thrift::PrefixManagerCommand::WITHDRAW_PREFIXES_BY_TYPE: {
    if (isAnyExistingPrefixPersistentByType(thriftReq.type)) {
      persistentEntryChange = true;
    }
    if (removePrefixesByType(thriftReq.type)) {
      kvStoreChange = true;
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoPrefixesOfType;
    }
    break;
  }
  case thrift::PrefixManagerCommand::SYNC_PREFIXES_BY_TYPE: {
    if (isAnyExistingPrefixPersistentByType(thriftReq.type) or
        isAnyInputPrefixPersistent(thriftReq.prefixes)) {
      persistentEntryChange = true;
    }
    if (syncPrefixesByType(thriftReq.type, thriftReq.prefixes)) {
      kvStoreChange = true;
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoChanges;
    }
    break;
  }
  case thrift::PrefixManagerCommand::GET_ALL_PREFIXES: {
    for (const auto& kv : prefixMap_) {
      for (const auto& kv2 : kv.second) {
        response.prefixes.emplace_back(kv2.second);
      }
    }
    response.success = true;
    break;
  }
  case thrift::PrefixManagerCommand::GET_PREFIXES_BY_TYPE: {
    for (const auto& kv : prefixMap_) {
      for (const auto& kv2 : kv.second) {
        if (kv2.second.type == thriftReq.type) {
          response.prefixes.emplace_back(kv2.second);
        }
      }
    }
    response.success = true;
    break;
  }
  default: {
    LOG(ERROR) << "Unknown command received";
    response.success = false;
    response.message = kErrorUnknownCommand;
    break;
  }
  }

  if (response.success) {
    if (persistentEntryChange) {
      persistPrefixDb();
    }
    if ((kvStoreChange) and
        (std::chrono::steady_clock::now() >= prefixHoldUntilTimePoint_)) {
      // Update kv store only after holdtime. All updates before holdtime
      // will be updated one shot by holdtimer
      updateKvStoreThrottled_->operator()();
    }
  }

  return fbzmq::Message::fromThriftObj(response, serializer_);
}

void
PrefixManager::submitCounters() {
  VLOG(2) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();
  counters["prefix_manager.zmq_event_queue_size"] = getEventQueueSize();

  // Count total route number
  counters["prefix_manager.num_routes"] = prefixMap_.size();

  // Count route number for each client type
  std::unordered_map<thrift::PrefixType, int64_t> prefixCountMap;
  for (const auto& kv : prefixMap_) {
    for (const auto& kv2 : kv.second) {
      prefixCountMap[kv2.first] += 1;
    }
  }
  for (const auto& kv : prefixCountMap) {
    auto typeName = folly::sformat(
        "prefix_manager.num_routes.{}",
        apache::thrift::TEnumTraits<thrift::PrefixType>::findName(kv.first));
    counters[typeName] = kv.second;
  }

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

int64_t
PrefixManager::getCounter(const std::string& key) {
  std::unordered_map<std::string, int64_t> counters;

  folly::Promise<std::unordered_map<std::string, int64_t>> promise;
  auto future = promise.getFuture();
  runImmediatelyOrInEventLoop([this, promise = std::move(promise)]() mutable {
    promise.setValue(tData_.getCounters());
  });
  counters = std::move(future).get();

  if (counters.find(key) != counters.end()) {
    return counters[key];
  }
  return 0;
}

int64_t
PrefixManager::getPrefixAddCounter() {
  return getCounter("prefix_manager.add_prefixes.count.0");
}

int64_t
PrefixManager::getPrefixWithdrawCounter() {
  return getCounter("prefix_manager.withdraw_prefixes.count.0");
}

// helpers for modifying our Prefix Db
bool
PrefixManager::addOrUpdatePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  bool updated{false};
  for (const auto& prefixEntry : prefixEntries) {
    LOG(INFO) << "Advertising prefix " << toString(prefixEntry.prefix)
              << ", client: "
              << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                     prefixEntry.type);
    bool prefixUpdated{false};

    auto& prefixes = prefixMap_[prefixEntry.prefix];
    auto it = prefixes.find(prefixEntry.type);
    if (it == prefixes.end()) {
      // Add missing prefix
      prefixes.emplace(prefixEntry.type, prefixEntry);
      prefixUpdated = true;
    } else if (it->second != prefixEntry) {
      it->second = prefixEntry;
      prefixUpdated = true;
    }

    updated |= prefixUpdated;
    if (prefixUpdated and perPrefixKeys_) {
      prefixesToUpdate_.emplace(prefixEntry.prefix);
    }
  }

  return updated;
}

bool
PrefixManager::removePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  // Verify all prefixes exist
  for (const auto& prefix : prefixes) {
    auto it = prefixMap_.find(prefix.prefix);
    if (it == prefixMap_.end()) {
      LOG(ERROR) << "Cannot withdraw prefix " << toString(prefix.prefix);
      return false;
    }

    auto it2 = it->second.find(prefix.type);
    if (it2 == it->second.end()) {
      // Missing prefix or invalid type
      LOG(ERROR) << "Cannot withdraw prefix " << toString(prefix.prefix)
                 << ", client: "
                 << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                        prefix.type);
      return false;
    }
  }

  for (const auto& prefix : prefixes) {
    LOG(INFO) << "Withdrawing prefix " << toString(prefix.prefix)
              << ", client: "
              << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                     prefix.type);
    auto& prefixEntries = prefixMap_.at(prefix.prefix);
    if (prefixEntries.erase(prefix.type)) {
      if (not prefixEntries.size()) {
        prefixMap_.erase(prefix.prefix);
      }
      if (perPrefixKeys_) {
        prefixesToUpdate_.emplace(prefix.prefix);
      }
    }
  }
  return true;
}

bool
PrefixManager::syncPrefixesByType(
    thrift::PrefixType type, const std::vector<thrift::PrefixEntry>& prefixes) {
  bool updated{false};

  // Remove old prefixes
  std::unordered_set<thrift::IpPrefix> newPrefixes;
  for (auto const& prefix : prefixes) {
    CHECK(type == prefix.type);
    newPrefixes.emplace(prefix.prefix);
  }
  for (auto it = prefixMap_.begin(); it != prefixMap_.end();) {
    // Iterate over each type
    for (auto it2 = it->second.begin(); it2 != it->second.end();) {
      // Skip prefixes not of our type
      if (it2->first != type) {
        ++it2;
        continue;
      }

      // Erase prefixes not present in newPrefixes
      if (newPrefixes.count(it->first) == 0) {
        if (perPrefixKeys_) {
          prefixesToUpdate_.emplace(it->first);
        }
        it2 = it->second.erase(it2);
        updated = true;
      } else {
        ++it2;
      }
    }

    // If no type left then remove prefix from prefixMap_
    if (not it->second.size()) {
      it = prefixMap_.erase(it);
    } else {
      ++it;
    }
  }

  // Add/update new prefixes
  updated |= addOrUpdatePrefixes(prefixes);

  return updated;
}

bool
PrefixManager::removePrefixesByType(thrift::PrefixType type) {
  bool changed = false;
  for (auto it = prefixMap_.begin(); it != prefixMap_.end();) {
    if (it->second.erase(type)) {
      changed = true;
      if (perPrefixKeys_) {
        prefixesToUpdate_.emplace(it->first);
      }
    }
    if (not it->second.size()) {
      it = prefixMap_.erase(it);
    } else {
      ++it;
    }
  }
  return changed;
}

bool
PrefixManager::isAnyInputPrefixPersistent(
    const std::vector<thrift::PrefixEntry>& prefixes) const {
  for (const auto& prefix : prefixes) {
    if ((not prefix.ephemeral.hasValue()) || (not prefix.ephemeral.value())) {
      return true;
    }
  }
  return false;
}

bool
PrefixManager::isAnyExistingPrefixPersistentByType(
    thrift::PrefixType type) const {
  for (const auto& kv : prefixMap_) {
    auto it = kv.second.find(type);
    if (it == kv.second.end()) {
      continue;
    }

    if (not it->second.ephemeral.value_or(false)) {
      return true;
    }
  }
  return false;
}

bool
PrefixManager::isAnyExistingPrefixPersistent(
    const std::vector<thrift::PrefixEntry>& prefixes) const {
  for (const auto& prefix : prefixes) {
    auto it = prefixMap_.find(prefix.prefix);
    if (it == prefixMap_.end()) {
      continue;
    }
    auto it2 = it->second.find(prefix.type);
    if (it2 != it->second.end()) {
      if (not it2->second.ephemeral.value_or(false)) {
        return true;
      }
    }
  }
  return false;
}

std::string
PrefixManager::serializePrefixDb(thrift::PrefixDatabase&& prefixDb) {
  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "PREFIX_DB_UPDATED");
    prefixDb.perfEvents = perfEvents;
  } else {
    DCHECK(!prefixDb.perfEvents.hasValue());
  }

  return fbzmq::util::writeThriftObjStr(prefixDb, serializer_);
}

} // namespace openr
