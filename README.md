# esphome-lldp

LLDP (IEEE 802.1AB) for ESPHome devices with wired Ethernet on ESP32.

- **Transmit:** the device announces itself so your managed switch (UniFi, Cisco,
  Juniper, MikroTik, lldpd…) shows its name, port and IP addresses in the neighbor table.
- **Receive:** the device learns which switch and port it's plugged into and
  publishes that to Home Assistant as sensors.

It's a standalone external component and needs no changes to ESPHome core. It
works on the existing `ethernet:` interface: it sends with `esp_eth_transmit()`
and receives by chaining the lwIP netif input function.

It deliberately does **not** use ESP-IDF's L2 TAP. Components that hook the
Ethernet driver's input path, such as
[esphome-pps-ntp](https://github.com/davidcoulson/esphome-pps-ntp)'s driver-level
receive timestamps, turn themselves off when `CONFIG_ESP_NETIF_L2_TAP` is set.
The lwIP hook runs after any driver hook, so both work together.

> Upstream ESPHome has an open PR for transmit-only LLDP inside the `ethernet`
> component ([esphome/esphome#11760](https://github.com/esphome/esphome/pull/11760)).
> This component also receives, and works on released ESPHome today.

## Requirements

| | |
|---|---|
| ESPHome | 2026.9.0 or newer |
| Platform | ESP32 family with the `ethernet:` component |
| ESP-IDF | 5.5.x, 6.0.x, 6.1.x (compile-tested on 5.5.5, 6.0.1, 6.1.0) |
| Ethernet chips | RMII PHYs (LAN8720, IP101, RTL8201, …) with the internal EMAC: TX + RX. SPI chips (W5500, DM9051, …): TX works; for RX you may need `promiscuous: true` (see below). |

## Install

```yaml
external_components:
  - source: github://davidcoulson/esphome-lldp@v0.2.0
    components: [lldp]
```

## Configuration

```yaml
lldp:
  # Everything is optional; these are the defaults.
  port_id: eth0
  port_description: Ethernet
  system_name: <esphome.name>
  system_description: "ESPHome <version> <board>"   # or esphome.project name/version
  management_address: true   # advertise the interface's IPv4/IPv6 addresses
  tx_interval: 30s           # 5s–3600s
  tx_hold: 4                 # TTL = tx_interval × tx_hold + 1
  tx_fast_count: 4           # 1 s burst after link-up
  transmit: true
  receive: true
  promiscuous: false         # set to true for W5500 and other MACs that can't filter for LLDP multicast
  mdns: false                # add the neighbor to the device's mDNS TXT record (see below)

  on_neighbor:               # fires when a neighbor appears or its data changes
    - logger.log:
        format: "Plugged into %s port %s"
        args: [x.system_name, x.port_id]
  on_neighbor_lost:          # TTL expired, link down, or a shutdown LLDPDU was received
    - logger.log: "Neighbor gone"

text_sensor:
  - platform: lldp
    system_name: { name: Switch }
    port_id: { name: Switch port }
    port_description: { name: Switch port description }
    chassis_id: { name: Switch chassis ID }
    management_address: { name: Switch address }
    system_description: { name: Switch description }
    capabilities: { name: Switch capabilities }   # e.g. "bridge, router"
    source_mac: { name: Switch source MAC }

sensor:
  - platform: lldp
    vlan_id: { name: Port VLAN }   # 802.1 Port VLAN ID TLV; unknown if not advertised

binary_sensor:
  - platform: lldp
    neighbor_present: { name: LLDP neighbor }
```

In lambdas, `id(<lldp id>).get_neighbor()` returns the current `LLDPNeighbor`
(its fields are plain C strings, e.g. `x.system_name`),
`has_neighbor()` says whether there is one, and `send_now()` sends an LLDPDU
immediately. See [`example.yaml`](example.yaml) for a full Olimex ESP32-POE-ISO config.

## mDNS

With `mdns: true`, the neighbor is added to the `_esphomelib._tcp` service
that Home Assistant and the ESPHome dashboard already browse:

| TXT key | Value |
|---|---|
| `lldp_switch` | neighbor System Name |
| `lldp_port` | neighbor Port ID |
| `lldp_vlan` | 802.1 Port VLAN ID, when advertised |

The keys are updated at runtime and removed when the neighbor goes away. That
needs `api:` and the `mdns` component to be enabled. Check with
`dns-sd -L <name> _esphomelib._tcp` (macOS) or
`avahi-browse -rt _esphomelib._tcp` (Linux).

## What gets sent

Chassis ID (MAC), Port ID (interface name), TTL, Port Description, System
Name, System Description, System Capabilities (station), and one Management
Address TLV per IPv4/IPv6 address. After link-up it sends a burst at 1 s
intervals, then one frame every `tx_interval`. It also sends a frame straight
away when the interface gets a new IP. On a clean shutdown or OTA reboot it
sends a TTL=0 frame so the switch drops the entry at once.

## Receive notes

- LLDP goes to multicast `01:80:C2:00:00:0E`. From IDF 5.5 the ESP32 EMAC no
  longer accepts all multicast by default, so the component adds a MAC filter
  entry (`ETH_CMD_ADD_MAC_FILTER`) for that address.
- **W5500** has no filter that can admit this address, so the IDF driver
  drops the frames. Set `promiscuous: true`. This costs a little CPU on busy
  networks, because the chip then passes every frame up to the stack.
- The component tracks **one** neighbor, the directly attached switch port.
  If several LLDP speakers can reach the device (for example through an
  unmanaged switch), the sensors show whichever one spoke last.
- Frames are checked for bounds and TLV order; malformed frames are dropped.
  Received strings are stored in fixed buffers (no heap), so very long values
  are truncated: 127 characters for names and port descriptions, 255 for the
  system description.

## Layout

```
components/lldp/
  __init__.py            config schema + codegen
  lldp_component.{h,cpp} driver/netif binding, TX scheduler, RX hook + neighbor aging
  lldp_frame.{h,cpp}     IDF-free LLDPDU builder/parser
  text_sensor.py sensor.py binary_sensor.py
tests/components/lldp/
  common.yaml, test.esp32-idf.yaml   compile test
  lldp_frame_test.cpp                gtest unit tests for the codec
```

The tests follow ESPHome's own layout. To run them, copy both directories into
an [esphome](https://github.com/esphome/esphome) checkout and run:

```bash
script/cpp_unit_test.py lldp
```

```bash
script/test_build_components -c lldp -t esp32-idf
```
