/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.Network
namespace py3 openr.thrift
namespace php Openr

// Using the defaults from here:
// https://en.wikipedia.org/wiki/Administrative_distance
enum AdminDistance {
  DIRECTLY_CONNECTED = 0,
  STATIC_ROUTE = 1,
  EBGP = 20,
  IBGP = 200,
  NETLINK_LISTENER = 225,
  MAX_ADMIN_DISTANCE = 255
}

enum MplsActionCode {
  PUSH = 0
  SWAP = 1
  PHP = 2      # Pen-ultimate hop popping => POP and FORWARD
  POP_AND_LOOKUP = 3
  NOOP = 4,
}

// For mimicing FBOSS agent thrift interfaces
enum PortAdminState {
  DISABLED = 0
  ENABLED = 1
}

// For mimicing FBOSS agent thrift interfaces
enum PortOperState {
  DOWN = 0
  UP = 1
}

struct MplsAction {
  1: MplsActionCode action;
  2: optional i32 swapLabel;          // Required if action == SWAP
  // front() (index=0) in list will be bottom of stack and back()
  // element is top of the stack
  3: optional list<i32> pushLabels;   // Required if action == PUSH
}

struct BinaryAddress {
  1: required binary addr
  3: optional string ifName
}

struct IpPrefix {
  1: BinaryAddress prefixAddress
  2: i16 prefixLength
}

struct NextHopThrift {
  1: BinaryAddress address
  // Default weight of 0 represents an ECMP route.
  // This default is chosen for two reasons:
  // 1) We rely on the arithmetic properties of 0 for ECMP vs UCMP route
  //    resolution calculations. A 0 weight next hop being present at a variety
  //    of layers in a route resolution tree will cause the entire route
  //    resolution to use ECMP.
  // 2) A client which does not set a value will result in
  //    0 being populated even with strange behavior in the client language
  //    which is consistent with C++
  2: i32 weight = 0
  // MPLS encapsulation information for IP->MPLS and MPLS routes
  3: optional MplsAction mplsAction

  // Metric (aka cost) associated with this nexthop
  51: i32 metric = 0

  // Use non-shortest route (usually false but enabled for KSP2_ED_ECMP)
  52: bool useNonShortestRoute = 0
}

struct MplsRoute {
  1: required i32 topLabel
  3: optional AdminDistance adminDistance
  4: list<NextHopThrift> nextHops
}

enum PrefixType {
  LOOPBACK = 1,
  DEFAULT = 2,
  BGP = 3,
  PREFIX_ALLOCATOR = 4,
  BREEZE = 5,   // Prefixes injected via breeze

  // Placeholder Types
  TYPE_1 = 21,
  TYPE_2 = 22,
  TYPE_3 = 23,
  TYPE_4 = 24,
  TYPE_5 = 25,
}

struct UnicastRoute {
  1: required IpPrefix dest
  3: optional AdminDistance adminDistance
  4: list<NextHopThrift> nextHops

  // fields used for route redistribution
  5: optional PrefixType prefixType
  6: optional binary data
  7: bool doNotInstall = false

  41: optional NextHopThrift bestNexthop

  # DEPREDCATED - Use nextHops instead
  # 2: list<BinaryAddress> deprecatedNexthops
}

// For mimicing FBOSS agent thrift interfaces
struct LinkNeighborThrift {
  1: i32 localPort
  2: i32 localVlan
  11: string printablePortId
  12: optional string systemName
}

// For mimicing FBOSS agent thrift interfaces
struct PortInfoThrift {
  1: i32 portId
  2: i64 speedMbps
  3: PortAdminState adminState
  4: PortOperState operState
  12: string name
}
