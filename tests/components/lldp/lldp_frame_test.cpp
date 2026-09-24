#include <gtest/gtest.h>

#include <string>

#include "esphome/components/lldp/lldp_frame.h"

namespace esphome::lldp::testing {

// Hand-built switch-style LLDPDU: chassis MAC, locally assigned port "Port 7",
// TTL 120, bridge+router, IPv4 management address, 802.1 PVID 20.
static const uint8_t SWITCH_FRAME[] = {
    0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e, 0x74, 0xac, 0xb9, 0x11, 0x22, 0x33, 0x88, 0xcc,  // Ethernet header
    0x02, 0x07, 0x04, 0x74, 0xac, 0xb9, 0x11, 0x22, 0x33,                                // chassis ID (MAC)
    0x04, 0x07, 0x07, 'P',  'o',  'r',  't',  ' ',  '7',                                 // port ID (local)
    0x06, 0x02, 0x00, 0x78,                                                              // TTL 120
    0x08, 0x06, 'P',  'o',  'r',  't',  ' ',  '7',                                       // port description
    0x0a, 0x08, 'U',  'S',  'W',  '-',  'L',  'i',  't',  'e',                           // system name
    0x0e, 0x04, 0x00, 0x14, 0x00, 0x14,                                                  // capabilities
    0x10, 0x0c, 0x05, 0x01, 192,  168,  1,    2,    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,  // mgmt IPv4
    0xfe, 0x06, 0x00, 0x80, 0xc2, 0x01, 0x00, 0x14,                                      // 802.1 PVID 20
    0x00, 0x00,                                                                          // end
};

static const uint8_t OUR_MAC[6] = {0x24, 0x0a, 0xc4, 0xaa, 0xbb, 0xcc};

TEST(LLDPParse, SwitchFrame) {
  LLDPNeighbor n;
  ASSERT_TRUE(parse_lldp_frame(SWITCH_FRAME, sizeof(SWITCH_FRAME), n));
  EXPECT_STREQ(n.chassis_id, "74:AC:B9:11:22:33");
  EXPECT_STREQ(n.source_mac, "74:AC:B9:11:22:33");
  EXPECT_STREQ(n.port_id, "Port 7");
  EXPECT_STREQ(n.port_description, "Port 7");
  EXPECT_STREQ(n.system_name, "USW-Lite");
  EXPECT_STREQ(n.capabilities, "bridge, router");
  EXPECT_STREQ(n.management_address, "192.168.1.2");
  EXPECT_EQ(n.ttl, 120);
  EXPECT_EQ(n.vlan_id, 20);
}

TEST(LLDPParse, RejectsTruncatedTLV) {
  LLDPNeighbor n;
  EXPECT_FALSE(parse_lldp_frame(SWITCH_FRAME, 30, n));
}

TEST(LLDPParse, RejectsWrongMandatoryOrder) {
  uint8_t bad[sizeof(SWITCH_FRAME)];
  memcpy(bad, SWITCH_FRAME, sizeof(bad));
  bad[14] = 0x04;  // first TLV claims to be Port ID
  LLDPNeighbor n;
  EXPECT_FALSE(parse_lldp_frame(bad, sizeof(bad), n));
}

TEST(LLDPParse, RejectsOtherEthertype) {
  uint8_t bad[sizeof(SWITCH_FRAME)];
  memcpy(bad, SWITCH_FRAME, sizeof(bad));
  bad[13] = 0x00;
  LLDPNeighbor n;
  EXPECT_FALSE(parse_lldp_frame(bad, sizeof(bad), n));
}

TEST(LLDPParse, TruncatesLongStrings) {
  uint8_t buf[600];
  LLDPFrameBuilder b(buf, sizeof(buf));
  ASSERT_TRUE(b.begin(OUR_MAC));
  ASSERT_TRUE(b.add_subtyped(TLV_CHASSIS_ID, CHASSIS_SUBTYPE_MAC, OUR_MAC, 6));
  ASSERT_TRUE(b.add_subtyped(TLV_PORT_ID, PORT_SUBTYPE_INTERFACE_NAME, "eth0", 4));
  ASSERT_TRUE(b.add_ttl(121));
  std::string longname(255, 'x');
  ASSERT_TRUE(b.add_string(TLV_SYSTEM_NAME, longname.c_str()));
  size_t len = b.finish();
  LLDPNeighbor n;
  ASSERT_TRUE(parse_lldp_frame(buf, len, n));
  EXPECT_EQ(strlen(n.system_name), sizeof(n.system_name) - 1);
}

TEST(LLDPBuild, RoundTrip) {
  uint8_t buf[768];
  LLDPFrameBuilder b(buf, sizeof(buf));
  ASSERT_TRUE(b.begin(OUR_MAC));
  ASSERT_TRUE(b.add_subtyped(TLV_CHASSIS_ID, CHASSIS_SUBTYPE_MAC, OUR_MAC, 6));
  ASSERT_TRUE(b.add_subtyped(TLV_PORT_ID, PORT_SUBTYPE_INTERFACE_NAME, "eth0", 4));
  ASSERT_TRUE(b.add_ttl(121));
  ASSERT_TRUE(b.add_string(TLV_SYSTEM_NAME, "garage-poe"));
  ASSERT_TRUE(b.add_capabilities(CAP_STATION, CAP_STATION));
  const uint8_t ip4[4] = {10, 0, 0, 42};
  ASSERT_TRUE(b.add_management_address(1, ip4, 4, 2));
  const uint8_t ip6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x26, 0x0a, 0xc4, 0xff, 0xfe, 0xaa, 0xbb, 0xcc};
  ASSERT_TRUE(b.add_management_address(2, ip6, 16, 2));
  size_t len = b.finish();
  ASSERT_GE(len, 60u);
  EXPECT_EQ(memcmp(buf, LLDP_MULTICAST_MAC, 6), 0);

  LLDPNeighbor n;
  ASSERT_TRUE(parse_lldp_frame(buf, len, n));
  EXPECT_STREQ(n.chassis_id, "24:0A:C4:AA:BB:CC");
  EXPECT_STREQ(n.port_id, "eth0");
  EXPECT_STREQ(n.system_name, "garage-poe");
  EXPECT_STREQ(n.capabilities, "station");
  EXPECT_STREQ(n.management_address, "10.0.0.42");  // IPv4 preferred over IPv6
  EXPECT_EQ(n.ttl, 121);
  EXPECT_EQ(n.vlan_id, -1);
}

TEST(LLDPBuild, NeverOverflows) {
  uint8_t small[40];
  LLDPFrameBuilder b(small, sizeof(small));
  ASSERT_TRUE(b.begin(OUR_MAC));
  ASSERT_TRUE(b.add_subtyped(TLV_CHASSIS_ID, CHASSIS_SUBTYPE_MAC, OUR_MAC, 6));
  std::string big(100, 'x');
  EXPECT_FALSE(b.add_string(TLV_SYSTEM_NAME, big.c_str()));
  EXPECT_LE(b.finish(), sizeof(small));
}

TEST(LLDPNeighbor, BridgeWinsOverStationOnSharedSegment) {
  // The switch frame advertises bridge+router; build a station frame like another ESPHome node sends.
  LLDPNeighbor bridge, station;
  ASSERT_TRUE(parse_lldp_frame(SWITCH_FRAME, sizeof(SWITCH_FRAME), bridge));
  uint8_t buf[256];
  LLDPFrameBuilder b(buf, sizeof(buf));
  ASSERT_TRUE(b.begin(OUR_MAC));
  ASSERT_TRUE(b.add_subtyped(TLV_CHASSIS_ID, CHASSIS_SUBTYPE_MAC, OUR_MAC, 6));
  ASSERT_TRUE(b.add_subtyped(TLV_PORT_ID, PORT_SUBTYPE_INTERFACE_NAME, "eth0", 4));
  ASSERT_TRUE(b.add_ttl(121));
  ASSERT_TRUE(b.add_capabilities(CAP_STATION, CAP_STATION));
  ASSERT_TRUE(parse_lldp_frame(buf, b.finish(), station));
  EXPECT_EQ(bridge.enabled_capabilities, CAP_BRIDGE | CAP_ROUTER);
  EXPECT_EQ(station.enabled_capabilities, CAP_STATION);

  EXPECT_FALSE(should_replace_neighbor(bridge, station));  // keep the switch
  EXPECT_TRUE(should_replace_neighbor(station, bridge));   // upgrade to the switch
  EXPECT_TRUE(should_replace_neighbor(bridge, bridge));    // refresh from the same port
  EXPECT_TRUE(should_replace_neighbor(station, station));  // same port, refresh
  // Two different stations: most recent wins, as before.
  LLDPNeighbor other = station;
  strcpy(other.port_id, "eth1");
  EXPECT_TRUE(should_replace_neighbor(station, other));
}

TEST(LLDPNeighbor, SameContentIgnoresTTL) {
  LLDPNeighbor a, b;
  ASSERT_TRUE(parse_lldp_frame(SWITCH_FRAME, sizeof(SWITCH_FRAME), a));
  ASSERT_TRUE(parse_lldp_frame(SWITCH_FRAME, sizeof(SWITCH_FRAME), b));
  b.ttl = 5;
  EXPECT_TRUE(a.same_content(b));
  b.vlan_id = 30;
  EXPECT_FALSE(a.same_content(b));
  EXPECT_TRUE(a.same_port(b));
}

}  // namespace esphome::lldp::testing
