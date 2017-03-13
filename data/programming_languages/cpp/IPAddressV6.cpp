/*
 * Copyright 2017 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/IPAddressV6.h>

#include <ostream>
#include <string>

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/MacAddress.h>
#include <folly/detail/IPAddressSource.h>

using std::ostream;
using std::string;

namespace folly {

// public static const
const uint32_t IPAddressV6::PREFIX_TEREDO = 0x20010000;
const uint32_t IPAddressV6::PREFIX_6TO4 = 0x2002;

// free functions
size_t hash_value(const IPAddressV6& addr) {
  return addr.hash();
}
ostream& operator<<(ostream& os, const IPAddressV6& addr) {
  os << addr.str();
  return os;
}
void toAppend(IPAddressV6 addr, string* result) {
  result->append(addr.str());
}
void toAppend(IPAddressV6 addr, fbstring* result) {
  result->append(addr.str());
}

bool IPAddressV6::validate(StringPiece ip) {
  if (ip.size() > 0 && ip.front() == '[' && ip.back() == ']') {
    ip = ip.subpiece(1, ip.size() - 2);
  }

  constexpr size_t kStrMaxLen = INET6_ADDRSTRLEN;
  std::array<char, kStrMaxLen + 1> ip_cstr;
  const size_t len = std::min(ip.size(), kStrMaxLen);
  std::memcpy(ip_cstr.data(), ip.data(), len);
  ip_cstr[len] = 0;
  struct in6_addr addr;
  return 1 == inet_pton(AF_INET6, ip_cstr.data(), &addr);
}

// public default constructor
IPAddressV6::IPAddressV6() {
}

// public string constructor
IPAddressV6::IPAddressV6(StringPiece addr) {
  auto ip = addr.str();

  // Allow addresses surrounded in brackets
  if (ip.size() < 2) {
    throw IPAddressFormatException(
        to<std::string>("Invalid IPv6 address '", ip, "': address too short"));
  }
  if (ip.front() == '[' && ip.back() == ']') {
    ip = ip.substr(1, ip.size() - 2);
  }

  struct addrinfo* result;
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  if (!getaddrinfo(ip.c_str(), nullptr, &hints, &result)) {
    struct sockaddr_in6* ipAddr = (struct sockaddr_in6*)result->ai_addr;
    addr_.in6Addr_ = ipAddr->sin6_addr;
    scope_ = uint16_t(ipAddr->sin6_scope_id);
    freeaddrinfo(result);
  } else {
    throw IPAddressFormatException(
        to<std::string>("Invalid IPv6 address '", ip, "'"));
  }
}

// in6_addr constructor
IPAddressV6::IPAddressV6(const in6_addr& src)
  : addr_(src)
{
}

// sockaddr_in6 constructor
IPAddressV6::IPAddressV6(const sockaddr_in6& src)
  : addr_(src.sin6_addr)
  , scope_(uint16_t(src.sin6_scope_id))
{
}

// ByteArray16 constructor
IPAddressV6::IPAddressV6(const ByteArray16& src)
  : addr_(src)
{
}

// link-local constructor
IPAddressV6::IPAddressV6(LinkLocalTag, MacAddress mac)
  : addr_(mac) {
}

IPAddressV6::AddressStorage::AddressStorage(MacAddress mac) {
  // The link-local address uses modified EUI-64 format,
  // See RFC 4291 sections 2.5.1, 2.5.6, and Appendix A
  const auto* macBytes = mac.bytes();
  memcpy(&bytes_.front(), "\xfe\x80\x00\x00\x00\x00\x00\x00", 8);
  bytes_[8] = macBytes[0] ^ 0x02;
  bytes_[9] = macBytes[1];
  bytes_[10] = macBytes[2];
  bytes_[11] = 0xff;
  bytes_[12] = 0xfe;
  bytes_[13] = macBytes[3];
  bytes_[14] = macBytes[4];
  bytes_[15] = macBytes[5];
}

void IPAddressV6::setFromBinary(ByteRange bytes) {
  if (bytes.size() != 16) {
    throw IPAddressFormatException(to<std::string>(
        "Invalid IPv6 binary data: length must ",
        "be 16 bytes, got ",
        bytes.size()));
  }
  memcpy(&addr_.in6Addr_.s6_addr, bytes.data(), sizeof(in6_addr));
  scope_ = 0;
}

// public
IPAddressV4 IPAddressV6::createIPv4() const {
  if (!isIPv4Mapped()) {
    throw IPAddressFormatException("addr is not v4-to-v6-mapped");
  }
  const unsigned char* by = bytes();
  return IPAddressV4(detail::Bytes::mkAddress4(&by[12]));
}

// convert two uint8_t bytes into a uint16_t as hibyte.lobyte
static inline uint16_t unpack(uint8_t lobyte, uint8_t hibyte) {
  return ((uint16_t)hibyte << 8) | (uint16_t)lobyte;
}

// given a src string, unpack count*2 bytes into dest
// dest must have as much storage as count
static inline void unpackInto(const unsigned char* src,
                              uint16_t* dest,
                              size_t count) {
  for (size_t i = 0, hi = 1, lo = 0; i < count; i++) {
    dest[i] = unpack(src[hi], src[lo]);
    hi += 2;
    lo += 2;
  }
}

// public
IPAddressV4 IPAddressV6::getIPv4For6To4() const {
  if (!is6To4()) {
    throw IPAddressV6::TypeError(format(
            "Invalid IP '{}': not a 6to4 address", str()).str());
  }
  // convert 16x8 bytes into first 4x16 bytes
  uint16_t ints[4] = {0,0,0,0};
  unpackInto(bytes(), ints, 4);
  // repack into 4x8
  union {
    unsigned char bytes[4];
    in_addr addr;
  } ipv4;
  ipv4.bytes[0] = (uint8_t)((ints[1] & 0xFF00) >> 8);
  ipv4.bytes[1] = (uint8_t)(ints[1] & 0x00FF);
  ipv4.bytes[2] = (uint8_t)((ints[2] & 0xFF00) >> 8);
  ipv4.bytes[3] = (uint8_t)(ints[2] & 0x00FF);
  return IPAddressV4(ipv4.addr);
}

// public
bool IPAddressV6::isIPv4Mapped() const {
  // v4 mapped addresses have their first 10 bytes set to 0, the next 2 bytes
  // set to 255 (0xff);
  const unsigned char* by = bytes();

  // check if first 10 bytes are 0
  for (int i = 0; i < 10; i++) {
    if (by[i] != 0x00) {
      return false;
    }
  }
  // check if bytes 11 and 12 are 255
  if (by[10] == 0xff && by[11] == 0xff) {
    return true;
  }
  return false;
}

// public
IPAddressV6::Type IPAddressV6::type() const {
  // convert 16x8 bytes into first 2x16 bytes
  uint16_t ints[2] = {0,0};
  unpackInto(bytes(), ints, 2);

  if ((((uint32_t)ints[0] << 16) | ints[1]) == IPAddressV6::PREFIX_TEREDO) {
    return Type::TEREDO;
  }

  if ((uint32_t)ints[0] == IPAddressV6::PREFIX_6TO4) {
    return Type::T6TO4;
  }

  return Type::NORMAL;
}

// public
string IPAddressV6::toJson() const {
  return format(
      "{{family:'AF_INET6', addr:'{}', hash:{}}}", str(), hash()).str();
}

// public
size_t IPAddressV6::hash() const {
  if (isIPv4Mapped()) {
    /* An IPAddress containing this object would be equal (i.e. operator==)
       to an IPAddress containing the corresponding IPv4.
       So we must make sure that the hash values are the same as well */
    return IPAddress::createIPv4(*this).hash();
  }

  static const uint64_t seed = AF_INET6;
  uint64_t hash1 = 0, hash2 = 0;
  hash::SpookyHashV2::Hash128(&addr_, 16, &hash1, &hash2);
  return hash::hash_combine(seed, hash1, hash2);
}

// public
bool IPAddressV6::inSubnet(StringPiece cidrNetwork) const {
  auto subnetInfo = IPAddress::createNetwork(cidrNetwork);
  auto addr = subnetInfo.first;
  if (!addr.isV6()) {
    throw IPAddressFormatException(to<std::string>(
        "Address '", addr.toJson(), "' ", "is not a V6 address"));
  }
  return inSubnetWithMask(addr.asV6(), fetchMask(subnetInfo.second));
}

// public
bool IPAddressV6::inSubnetWithMask(const IPAddressV6& subnet,
                                   const ByteArray16& cidrMask) const {
  const ByteArray16 mask = detail::Bytes::mask(toByteArray(), cidrMask);
  const ByteArray16 subMask = detail::Bytes::mask(subnet.toByteArray(),
                                                  cidrMask);
  return (mask == subMask);
}

// public
bool IPAddressV6::isLoopback() const {
  // Check if v4 mapped is loopback
  if (isIPv4Mapped() && createIPv4().isLoopback()) {
    return true;
  }
  auto socka = toSockAddr();
  return IN6_IS_ADDR_LOOPBACK(&socka.sin6_addr);
}

bool IPAddressV6::isRoutable() const {
  return
    // 2000::/3 is the only assigned global unicast block
    inBinarySubnet({{0x20, 0x00}}, 3) ||
    // ffxe::/16 are global scope multicast addresses,
    // which are eligible to be routed over the internet
    (isMulticast() && getMulticastScope() == 0xe);
}

bool IPAddressV6::isLinkLocalBroadcast() const {
  static const IPAddressV6 kLinkLocalBroadcast("ff02::1");
  return *this == kLinkLocalBroadcast;
}

// public
bool IPAddressV6::isPrivate() const {
  // Check if mapped is private
  if (isIPv4Mapped() && createIPv4().isPrivate()) {
    return true;
  }
  return isLoopback() || inBinarySubnet({{0xfc, 0x00}}, 7);
}

// public
bool IPAddressV6::isLinkLocal() const {
  return inBinarySubnet({{0xfe, 0x80}}, 10);
}

bool IPAddressV6::isMulticast() const {
  return addr_.bytes_[0] == 0xff;
}

uint8_t IPAddressV6::getMulticastFlags() const {
  DCHECK(isMulticast());
  return ((addr_.bytes_[1] >> 4) & 0xf);
}

uint8_t IPAddressV6::getMulticastScope() const {
  DCHECK(isMulticast());
  return (addr_.bytes_[1] & 0xf);
}

IPAddressV6 IPAddressV6::getSolicitedNodeAddress() const {
  // Solicted node addresses must be constructed from unicast (or anycast)
  // addresses
  DCHECK(!isMulticast());

  uint8_t bytes[16] = { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00 };
  bytes[13] = addr_.bytes_[13];
  bytes[14] = addr_.bytes_[14];
  bytes[15] = addr_.bytes_[15];
  return IPAddressV6::fromBinary(ByteRange(bytes, 16));
}

// public
IPAddressV6 IPAddressV6::mask(size_t numBits) const {
  static const auto bits = bitCount();
  if (numBits > bits) {
    throw IPAddressFormatException(
        to<std::string>("numBits(", numBits, ") > bitCount(", bits, ")"));
  }
  ByteArray16 ba = detail::Bytes::mask(fetchMask(numBits), addr_.bytes_);
  return IPAddressV6(ba);
}

// public
string IPAddressV6::str() const {
  char buffer[INET6_ADDRSTRLEN] = {0};
  sockaddr_in6 sock = toSockAddr();
  if (!getnameinfo(
        (sockaddr*)&sock, sizeof(sock),
        buffer, INET6_ADDRSTRLEN,
        nullptr, 0, NI_NUMERICHOST)) {
    string ip(buffer);
    return ip;
  } else {
    throw IPAddressFormatException(to<std::string>(
        "Invalid address with hex ",
        "'",
        detail::Bytes::toHex(bytes(), 16),
        "'"));
  }
}

// public
string IPAddressV6::toFullyQualified() const {
  return detail::fastIpv6ToString(addr_.in6Addr_);
}

// public
uint8_t IPAddressV6::getNthMSByte(size_t byteIndex) const {
  const auto highestIndex = byteCount() - 1;
  if (byteIndex > highestIndex) {
    throw std::invalid_argument(to<string>("Byte index must be <= ",
        to<string>(highestIndex), " for addresses of type :",
        detail::familyNameStr(AF_INET6)));
  }
  return bytes()[byteIndex];
}

// protected
const ByteArray16 IPAddressV6::fetchMask(size_t numBits) {
  static const size_t bits = bitCount();
  if (numBits > bits) {
    throw IPAddressFormatException("IPv6 addresses are 128 bits.");
  }
  // masks_ is backed by an array so is zero indexed
  return masks_[numBits];
}

// public static
CIDRNetworkV6 IPAddressV6::longestCommonPrefix(
    const CIDRNetworkV6& one,
    const CIDRNetworkV6& two) {
  auto prefix = detail::Bytes::longestCommonPrefix(
      one.first.addr_.bytes_, one.second, two.first.addr_.bytes_, two.second);
  return {IPAddressV6(prefix.first), prefix.second};
}

// protected
bool IPAddressV6::inBinarySubnet(const std::array<uint8_t, 2> addr,
                                 size_t numBits) const {
  auto masked = mask(numBits);
  return (std::memcmp(addr.data(), masked.bytes(), 2) == 0);
}

// static private
const std::array<ByteArray16, 129> IPAddressV6::masks_ = {{
/* /0   */ {{ 0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /1   */ {{ 0x80,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /2   */ {{ 0xc0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /3   */ {{ 0xe0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /4   */ {{ 0xf0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /5   */ {{ 0xf8,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /6   */ {{ 0xfc,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /7   */ {{ 0xfe,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /8   */ {{ 0xff,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /9   */ {{ 0xff,0x80,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /10  */ {{ 0xff,0xc0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /11  */ {{ 0xff,0xe0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /12  */ {{ 0xff,0xf0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /13  */ {{ 0xff,0xf8,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /14  */ {{ 0xff,0xfc,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /15  */ {{ 0xff,0xfe,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /16  */ {{ 0xff,0xff,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /17  */ {{ 0xff,0xff,0x80,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /18  */ {{ 0xff,0xff,0xc0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /19  */ {{ 0xff,0xff,0xe0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /20  */ {{ 0xff,0xff,0xf0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /21  */ {{ 0xff,0xff,0xf8,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /22  */ {{ 0xff,0xff,0xfc,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /23  */ {{ 0xff,0xff,0xfe,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /24  */ {{ 0xff,0xff,0xff,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /25  */ {{ 0xff,0xff,0xff,0x80,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /26  */ {{ 0xff,0xff,0xff,0xc0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /27  */ {{ 0xff,0xff,0xff,0xe0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /28  */ {{ 0xff,0xff,0xff,0xf0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /29  */ {{ 0xff,0xff,0xff,0xf8,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /30  */ {{ 0xff,0xff,0xff,0xfc,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /31  */ {{ 0xff,0xff,0xff,0xfe,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /32  */ {{ 0xff,0xff,0xff,0xff,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /33  */ {{ 0xff,0xff,0xff,0xff,
             0x80,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /34  */ {{ 0xff,0xff,0xff,0xff,
             0xc0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /35  */ {{ 0xff,0xff,0xff,0xff,
             0xe0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /36  */ {{ 0xff,0xff,0xff,0xff,
             0xf0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /37  */ {{ 0xff,0xff,0xff,0xff,
             0xf8,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /38  */ {{ 0xff,0xff,0xff,0xff,
             0xfc,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /39  */ {{ 0xff,0xff,0xff,0xff,
             0xfe,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /40  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /41  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0x80,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /42  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xc0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /43  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xe0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /44  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xf0,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /45  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xf8,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /46  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xfc,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /47  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xfe,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /48  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0x00,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /49  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0x80,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /50  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xc0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /51  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xe0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /52  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xf0,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /53  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xf8,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /54  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xfc,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /55  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xfe,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /56  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x00,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /57  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x80,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /58  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xc0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /59  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xe0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /60  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf0,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /61  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf8,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /62  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfc,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /63  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfe,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /64  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0x00,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /65  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0x80,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /66  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xc0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /67  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xe0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /68  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xf0,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /69  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xf8,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /70  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xfc,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /71  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xfe,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /72  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0x00,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /73  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0x80,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /74  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xc0,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /75  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xe0,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /76  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xf0,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /77  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xf8,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /78  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xfc,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /79  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xfe,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /80  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0x00,0x00,
             0x00,0x00,0x00,0x00 }},
/* /81  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0x80,0x00,
             0x00,0x00,0x00,0x00 }},
/* /82  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xc0,0x00,
             0x00,0x00,0x00,0x00 }},
/* /83  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xe0,0x00,
             0x00,0x00,0x00,0x00 }},
/* /84  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xf0,0x00,
             0x00,0x00,0x00,0x00 }},
/* /85  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xf8,0x00,
             0x00,0x00,0x00,0x00 }},
/* /86  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xfc,0x00,
             0x00,0x00,0x00,0x00 }},
/* /87  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xfe,0x00,
             0x00,0x00,0x00,0x00 }},
/* /88  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x00,
             0x00,0x00,0x00,0x00 }},
/* /89  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x80,
             0x00,0x00,0x00,0x00 }},
/* /90  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xc0,
             0x00,0x00,0x00,0x00 }},
/* /91  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xe0,
             0x00,0x00,0x00,0x00 }},
/* /92  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf0,
             0x00,0x00,0x00,0x00 }},
/* /93  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf8,
             0x00,0x00,0x00,0x00 }},
/* /94  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfc,
             0x00,0x00,0x00,0x00 }},
/* /95  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfe,
             0x00,0x00,0x00,0x00 }},
/* /96  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0x00,0x00,0x00,0x00 }},
/* /97  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0x80,0x00,0x00,0x00 }},
/* /98  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xc0,0x00,0x00,0x00 }},
/* /99  */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xe0,0x00,0x00,0x00 }},
/* /100 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xf0,0x00,0x00,0x00 }},
/* /101 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xf8,0x00,0x00,0x00 }},
/* /102 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xfc,0x00,0x00,0x00 }},
/* /103 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xfe,0x00,0x00,0x00 }},
/* /104 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0x00,0x00,0x00 }},
/* /105 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0x80,0x00,0x00 }},
/* /106 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xc0,0x00,0x00 }},
/* /107 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xe0,0x00,0x00 }},
/* /108 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xf0,0x00,0x00 }},
/* /109 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xf8,0x00,0x00 }},
/* /110 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xfc,0x00,0x00 }},
/* /111 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xfe,0x00,0x00 }},
/* /112 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0x00,0x00 }},
/* /113 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0x80,0x00 }},
/* /114 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xc0,0x00 }},
/* /115 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xe0,0x00 }},
/* /116 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xf0,0x00 }},
/* /117 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xf8,0x00 }},
/* /118 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xfc,0x00 }},
/* /119 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xfe,0x00 }},
/* /120 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x00 }},
/* /121 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0x80 }},
/* /122 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xc0 }},
/* /123 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xe0 }},
/* /124 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf0 }},
/* /125 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xf8 }},
/* /126 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfc }},
/* /127 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xfe }},
/* /128 */ {{ 0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff,
             0xff,0xff,0xff,0xff }},
}};

} // folly
