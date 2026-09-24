#include "lldp_frame.h"

#include <cstdio>

#include "esphome/core/helpers.h"

namespace esphome::lldp {

namespace {

constexpr uint8_t CHASSIS_SUBTYPE_NETADDR = 5;
constexpr uint8_t PORT_SUBTYPE_MAC = 3;
constexpr uint8_t PORT_SUBTYPE_NETADDR = 4;

constexpr uint8_t OUI_8021[3] = {0x00, 0x80, 0xC2};
constexpr uint8_t OUI_8021_PVID = 1;

constexpr const char *CAPABILITY_NAMES[] = {"other",  "repeater", "bridge", "wlan-ap", "router", "telephone",
                                            "docsis", "station",  "c-vlan", "s-vlan",  "tpmr"};

inline uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }

/// Copy a TLV string, dropping trailing NUL/whitespace padding some switches send.
void copy_text(char *dst, size_t cap, const uint8_t *p, size_t len) {
  while (len > 0 && (p[len - 1] == 0 || p[len - 1] == ' ' || p[len - 1] == '\n' || p[len - 1] == '\r'))
    len--;
  if (len >= cap)
    len = cap - 1;
  memcpy(dst, p, len);
  dst[len] = '\0';
}

bool is_printable(const uint8_t *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (p[i] < 0x20 || p[i] > 0x7E)
      return false;
  }
  return len > 0;
}

/// Network address as used in Chassis/Port ID and Management Address TLVs:
/// [IANA address family][address bytes]
void format_netaddr(char *dst, size_t cap, const uint8_t *p, size_t len) {
  if (len == 5 && p[0] == 1) {
    snprintf(dst, cap, "%u.%u.%u.%u", p[1], p[2], p[3], p[4]);
  } else if (len == 17 && p[0] == 2) {
    snprintf(dst, cap, "%x:%x:%x:%x:%x:%x:%x:%x", be16(p + 1), be16(p + 3), be16(p + 5), be16(p + 7), be16(p + 9),
             be16(p + 11), be16(p + 13), be16(p + 15));
  } else if (len > 1) {
    format_hex_pretty_to(dst, cap, p + 1, len - 1);
  } else {
    dst[0] = '\0';
  }
}

void format_id(char *dst, size_t cap, uint8_t subtype, const uint8_t *p, size_t len, uint8_t mac_subtype,
               uint8_t netaddr_subtype) {
  if (subtype == netaddr_subtype) {
    format_netaddr(dst, cap, p, len);
  } else if (subtype != mac_subtype && is_printable(p, len)) {
    copy_text(dst, cap, p, len);
  } else {
    format_hex_pretty_to(dst, cap, p, len);
  }
}

void format_capabilities(char *dst, size_t cap, uint16_t caps) {
  size_t pos = 0;
  dst[0] = '\0';
  for (size_t bit = 0; bit < sizeof(CAPABILITY_NAMES) / sizeof(CAPABILITY_NAMES[0]); bit++) {
    if ((caps & (1u << bit)) == 0)
      continue;
    int n = snprintf(dst + pos, cap - pos, pos ? ", %s" : "%s", CAPABILITY_NAMES[bit]);
    if (n < 0 || pos + n >= cap)
      return;
    pos += n;
  }
}

}  // namespace

bool parse_lldp_frame(const uint8_t *frame, size_t len, LLDPNeighbor &out) {
  if (len < LLDP_ETH_HEADER_LEN || be16(frame + 12) != LLDP_ETHERTYPE)
    return false;

  out.clear();
  format_hex_pretty_to(out.source_mac, frame + 6, 6);

  const uint8_t *p = frame + LLDP_ETH_HEADER_LEN;
  const uint8_t *end = frame + len;
  int index = 0;
  bool have_ttl = false;
  bool have_ipv4 = false;

  while (p + 2 <= end) {
    uint8_t type = p[0] >> 1;
    size_t tlv_len = ((p[0] & 0x01) << 8) | p[1];
    const uint8_t *v = p + 2;
    if (v + tlv_len > end)
      return false;
    p = v + tlv_len;

    // The first three TLVs are mandatory and ordered.
    if ((index == 0 && type != TLV_CHASSIS_ID) || (index == 1 && type != TLV_PORT_ID) ||
        (index == 2 && type != TLV_TTL))
      return false;
    index++;

    switch (type) {
      case TLV_END:
        return have_ttl;
      case TLV_CHASSIS_ID:
        if (tlv_len < 2)
          return false;
        format_id(out.chassis_id, sizeof(out.chassis_id), v[0], v + 1, tlv_len - 1, CHASSIS_SUBTYPE_MAC,
                  CHASSIS_SUBTYPE_NETADDR);
        break;
      case TLV_PORT_ID:
        if (tlv_len < 2)
          return false;
        format_id(out.port_id, sizeof(out.port_id), v[0], v + 1, tlv_len - 1, PORT_SUBTYPE_MAC, PORT_SUBTYPE_NETADDR);
        break;
      case TLV_TTL:
        if (tlv_len < 2)
          return false;
        out.ttl = be16(v);
        have_ttl = true;
        break;
      case TLV_PORT_DESCRIPTION:
        copy_text(out.port_description, sizeof(out.port_description), v, tlv_len);
        break;
      case TLV_SYSTEM_NAME:
        copy_text(out.system_name, sizeof(out.system_name), v, tlv_len);
        break;
      case TLV_SYSTEM_DESCRIPTION:
        copy_text(out.system_description, sizeof(out.system_description), v, tlv_len);
        break;
      case TLV_SYSTEM_CAPABILITIES:
        if (tlv_len >= 4) {
          out.enabled_capabilities = be16(v + 2);
          format_capabilities(out.capabilities, sizeof(out.capabilities), out.enabled_capabilities);
        }
        break;
      case TLV_MANAGEMENT_ADDRESS: {
        // [addr_len][family][addr...]...; addr_len counts the family byte.
        if (tlv_len < 2 || v[0] < 2 || size_t(v[0]) + 1 > tlv_len)
          break;
        // Keep the first IPv4 address, otherwise the first address of any kind.
        bool is_v4 = v[1] == 1;
        if (have_ipv4 || (!is_v4 && out.management_address[0] != '\0'))
          break;
        format_netaddr(out.management_address, sizeof(out.management_address), v + 1, v[0]);
        have_ipv4 = is_v4;
        break;
      }
      case TLV_ORG_SPECIFIC:
        if (tlv_len >= 6 && memcmp(v, OUI_8021, 3) == 0 && v[3] == OUI_8021_PVID)
          out.vlan_id = static_cast<int16_t>(be16(v + 4) & 0x0FFF);
        break;
      default:
        break;
    }
  }
  // Tolerate a missing End TLV as long as the mandatory TLVs were present.
  return have_ttl;
}

static bool is_bridge(const LLDPNeighbor &n) { return (n.enabled_capabilities & (CAP_BRIDGE | CAP_REPEATER)) != 0; }

bool should_replace_neighbor(const LLDPNeighbor &current, const LLDPNeighbor &incoming) {
  if (current.same_port(incoming))
    return true;
  // Keep a bridge over a station; otherwise the most recent speaker wins.
  return !(is_bridge(current) && !is_bridge(incoming));
}

bool LLDPFrameBuilder::begin(const uint8_t src_mac[6]) {
  if (this->cap_ < LLDP_ETH_HEADER_LEN + 2)
    return false;
  memcpy(this->buf_, LLDP_MULTICAST_MAC, 6);
  memcpy(this->buf_ + 6, src_mac, 6);
  this->buf_[12] = LLDP_ETHERTYPE >> 8;
  this->buf_[13] = LLDP_ETHERTYPE & 0xFF;
  this->used_ = LLDP_ETH_HEADER_LEN;
  return true;
}

void LLDPFrameBuilder::put_header_(uint8_t type, size_t len) {
  this->buf_[this->used_++] = (type << 1) | ((len >> 8) & 0x01);
  this->buf_[this->used_++] = len & 0xFF;
}

bool LLDPFrameBuilder::add(uint8_t type, const void *data, size_t len) {
  if (len > 511 || !this->reserve_(2 + len))
    return false;
  this->put_header_(type, len);
  if (len > 0)
    memcpy(this->buf_ + this->used_, data, len);
  this->used_ += len;
  return true;
}

bool LLDPFrameBuilder::add_subtyped(uint8_t type, uint8_t subtype, const void *data, size_t len) {
  if (len + 1 > 511 || !this->reserve_(3 + len))
    return false;
  this->put_header_(type, len + 1);
  this->buf_[this->used_++] = subtype;
  memcpy(this->buf_ + this->used_, data, len);
  this->used_ += len;
  return true;
}

bool LLDPFrameBuilder::add_ttl(uint16_t ttl) {
  uint8_t v[2] = {uint8_t(ttl >> 8), uint8_t(ttl & 0xFF)};
  return this->add(TLV_TTL, v, sizeof(v));
}

bool LLDPFrameBuilder::add_capabilities(uint16_t system, uint16_t enabled) {
  uint8_t v[4] = {uint8_t(system >> 8), uint8_t(system & 0xFF), uint8_t(enabled >> 8), uint8_t(enabled & 0xFF)};
  return this->add(TLV_SYSTEM_CAPABILITIES, v, sizeof(v));
}

bool LLDPFrameBuilder::add_management_address(uint8_t family, const uint8_t *addr, size_t addr_len, uint32_t if_index) {
  if (addr_len > 16)
    return false;
  uint8_t v[1 + 1 + 16 + 1 + 4 + 1];
  size_t n = 0;
  v[n++] = uint8_t(addr_len + 1);
  v[n++] = family;
  memcpy(v + n, addr, addr_len);
  n += addr_len;
  v[n++] = 2;  // interface numbering subtype: ifIndex
  v[n++] = uint8_t(if_index >> 24);
  v[n++] = uint8_t(if_index >> 16);
  v[n++] = uint8_t(if_index >> 8);
  v[n++] = uint8_t(if_index);
  v[n++] = 0;  // no OID
  return this->add(TLV_MANAGEMENT_ADDRESS, v, n);
}

size_t LLDPFrameBuilder::finish() {
  // add() always leaves room for the End TLV.
  this->put_header_(TLV_END, 0);
  // Pad to the 60-byte Ethernet minimum (without FCS); not every MAC does it.
  while (this->used_ < 60 && this->used_ < this->cap_)
    this->buf_[this->used_++] = 0;
  return this->used_;
}

}  // namespace esphome::lldp
