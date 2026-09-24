// Host-side test of the LLDPDU codec: c++ -std=c++20 -I components tests/test_lldp_frame.cpp components/lldp/lldp_frame.cpp
#include <cassert>
#include <cstdio>
#include <cstring>

#include "lldp/lldp_frame.h"

using namespace esphome::lldp;

// Hand-built switch-style LLDPDU (chassis MAC, Port "Port 7", PVID 20, mgmt 192.168.1.2, bridge+router).
static const uint8_t UNIFI[] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e, 0x74, 0xac, 0xb9, 0x11, 0x22, 0x33, 0x88, 0xcc,
    0x02, 0x07, 0x04, 0x74, 0xac, 0xb9, 0x11, 0x22, 0x33,                                // chassis MAC
    0x04, 0x07, 0x07, 'P', 'o', 'r', 't', ' ', '7',                                      // port id (local)
    0x06, 0x02, 0x00, 0x78,                                                              // TTL 120
    0x08, 0x06, 'P', 'o', 'r', 't', ' ', '7',                                            // port desc
    0x0a, 0x08, 'U', 'S', 'W', '-', 'L', 'i', 't', 'e',                                  // system name
    0x0e, 0x04, 0x00, 0x14, 0x00, 0x14,                                                  // caps bridge+router
    0x10, 0x0c, 0x05, 0x01, 192, 168, 1, 2, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,           // mgmt v4
    0xfe, 0x06, 0x00, 0x80, 0xc2, 0x01, 0x00, 0x14,                                      // 802.1 PVID 20
    0x00, 0x00,
};

int main() {
  LLDPNeighbor n;
  assert(parse_lldp_frame(UNIFI, sizeof(UNIFI), n));
  assert(n.chassis_id == "74:AC:B9:11:22:33");
  assert(n.source_mac == "74:AC:B9:11:22:33");
  assert(n.port_id == "Port 7");
  assert(n.port_description == "Port 7");
  assert(n.system_name == "USW-Lite");
  assert(n.ttl == 120);
  assert(n.capabilities == "bridge, router");
  assert(n.management_address == "192.168.1.2");
  assert(n.vlan_id == 20);

  // Truncated TLV must be rejected, not over-read.
  assert(!parse_lldp_frame(UNIFI, 30, n));
  // Wrong mandatory order.
  uint8_t bad[sizeof(UNIFI)];
  memcpy(bad, UNIFI, sizeof(bad));
  bad[14] = 0x04;  // first TLV claims to be Port ID
  assert(!parse_lldp_frame(bad, sizeof(bad), n));

  // Round-trip our own frame.
  uint8_t buf[1100];
  const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0xaa, 0xbb, 0xcc};
  LLDPFrameBuilder b(buf, sizeof(buf));
  assert(b.begin(mac));
  assert(b.add_subtyped(TLV_CHASSIS_ID, 4, mac, 6));
  assert(b.add_subtyped(TLV_PORT_ID, 5, "eth0", 4));
  assert(b.add_ttl(121));
  assert(b.add_string(TLV_SYSTEM_NAME, "garage-poe"));
  assert(b.add_capabilities(CAP_STATION, CAP_STATION));
  const uint8_t ip4[4] = {10, 0, 0, 42};
  assert(b.add_management_address(1, ip4, 4, 2));
  const uint8_t ip6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x26, 0x0a, 0xc4, 0xff, 0xfe, 0xaa, 0xbb, 0xcc};
  assert(b.add_management_address(2, ip6, 16, 2));
  size_t len = b.finish();
  assert(len >= 60);
  assert(memcmp(buf, LLDP_MULTICAST_MAC, 6) == 0);
  assert(parse_lldp_frame(buf, len, n));
  assert(n.chassis_id == "24:0A:C4:AA:BB:CC");
  assert(n.port_id == "eth0");
  assert(n.system_name == "garage-poe");
  assert(n.ttl == 121);
  assert(n.capabilities == "station");
  assert(n.management_address == "10.0.0.42");  // IPv4 preferred over IPv6
  assert(n.vlan_id == -1);

  // Builder must refuse to overflow and still leave room for End TLV.
  uint8_t small[40];
  LLDPFrameBuilder s(small, sizeof(small));
  assert(s.begin(mac));
  assert(s.add_subtyped(TLV_CHASSIS_ID, 4, mac, 6));
  assert(!s.add_string(TLV_SYSTEM_NAME, std::string(100, 'x')));
  assert(s.finish() <= sizeof(small));

  puts("all LLDP frame tests passed");
  return 0;
}
