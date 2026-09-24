#pragma once

// LLDPDU (IEEE 802.1AB) encode/decode. Platform independent so it can be
// unit tested on the host.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::lldp {

inline constexpr uint16_t LLDP_ETHERTYPE = 0x88CC;
inline constexpr uint8_t LLDP_MULTICAST_MAC[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};
inline constexpr size_t LLDP_ETH_HEADER_LEN = 14;

enum TLVType : uint8_t {
  TLV_END = 0,
  TLV_CHASSIS_ID = 1,
  TLV_PORT_ID = 2,
  TLV_TTL = 3,
  TLV_PORT_DESCRIPTION = 4,
  TLV_SYSTEM_NAME = 5,
  TLV_SYSTEM_DESCRIPTION = 6,
  TLV_SYSTEM_CAPABILITIES = 7,
  TLV_MANAGEMENT_ADDRESS = 8,
  TLV_ORG_SPECIFIC = 127,
};

// System capability bits (802.1AB 8.5.8)
enum Capability : uint16_t {
  CAP_OTHER = 1 << 0,
  CAP_REPEATER = 1 << 1,
  CAP_BRIDGE = 1 << 2,
  CAP_WLAN_AP = 1 << 3,
  CAP_ROUTER = 1 << 4,
  CAP_TELEPHONE = 1 << 5,
  CAP_DOCSIS = 1 << 6,
  CAP_STATION = 1 << 7,
};

// Chassis/Port ID subtypes (802.1AB 8.5.2/8.5.3)
inline constexpr uint8_t CHASSIS_SUBTYPE_MAC = 4;
inline constexpr uint8_t PORT_SUBTYPE_INTERFACE_NAME = 5;

/// A received neighbor. Fixed-size so parsing never allocates; longer values
/// are truncated (LLDP allows 255 bytes, real switches send far less).
struct LLDPNeighbor {
  char source_mac[18];
  char chassis_id[64];
  char port_id[64];
  char port_description[128];
  char system_name[128];
  char system_description[256];
  char management_address[40];
  char capabilities[96];          // enabled capabilities, e.g. "bridge, router"
  uint16_t enabled_capabilities;  // the same as Capability bits
  int16_t vlan_id;                // 802.1 Port VLAN ID, -1 when not advertised
  uint16_t ttl;

  void clear() {
    memset(this, 0, sizeof(*this));
    this->vlan_id = -1;
  }
  /// Same advertising port (chassis + port).
  bool same_port(const LLDPNeighbor &o) const {
    return strcmp(this->chassis_id, o.chassis_id) == 0 && strcmp(this->port_id, o.port_id) == 0;
  }
  /// Same content, ignoring the TTL.
  bool same_content(const LLDPNeighbor &o) const {
    return this->same_port(o) && this->vlan_id == o.vlan_id && strcmp(this->source_mac, o.source_mac) == 0 &&
           strcmp(this->port_description, o.port_description) == 0 && strcmp(this->system_name, o.system_name) == 0 &&
           strcmp(this->system_description, o.system_description) == 0 &&
           strcmp(this->management_address, o.management_address) == 0 &&
           strcmp(this->capabilities, o.capabilities) == 0;
  }
};

/// Parse a full Ethernet frame carrying an LLDPDU. Returns false if the frame
/// is malformed or a mandatory TLV is missing.
bool parse_lldp_frame(const uint8_t *frame, size_t len, LLDPNeighbor &out);

/// Whether an LLDPDU from `incoming` should replace the neighbor we are
/// currently tracking. Only one neighbor is tracked, so on a shared segment
/// (an unmanaged switch between us and the real one) the bridge wins over
/// stations that happen to be heard too. Refreshes from the same port always win.
bool should_replace_neighbor(const LLDPNeighbor &current, const LLDPNeighbor &incoming);

/// Incremental LLDPDU builder writing into a caller-provided buffer. Always
/// keeps room for the End TLV, so finish() cannot fail after begin().
class LLDPFrameBuilder {
 public:
  LLDPFrameBuilder(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}

  bool begin(const uint8_t src_mac[6]);
  bool add(uint8_t type, const void *data, size_t len);
  bool add_subtyped(uint8_t type, uint8_t subtype, const void *data, size_t len);
  bool add_string(uint8_t type, const char *s) { return this->add(type, s, strlen(s)); }
  bool add_ttl(uint16_t ttl);
  bool add_capabilities(uint16_t system, uint16_t enabled);
  /// family: 1 = IPv4 (4 bytes), 2 = IPv6 (16 bytes); addr in network byte order.
  bool add_management_address(uint8_t family, const uint8_t *addr, size_t addr_len, uint32_t if_index);
  size_t finish();

 protected:
  bool reserve_(size_t n) const { return this->used_ + n + 2 /* End TLV */ <= this->cap_; }
  void put_header_(uint8_t type, size_t len);

  uint8_t *buf_;
  size_t cap_;
  size_t used_{0};
};

}  // namespace esphome::lldp
