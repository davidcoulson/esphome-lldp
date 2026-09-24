#include "lldp_component.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_idf_version.h"
#include "esp_netif_net_stack.h"
#include "lwip/pbuf.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

// Deliberately no L2 TAP: pps_ntp (and anything else that timestamps in the
// Ethernet driver) replaces the driver input path and backs off when
// CONFIG_ESP_NETIF_L2_TAP is set. Instead we transmit with esp_eth_transmit()
// and receive by chaining the lwIP netif input function, which sits after any
// driver-level hook, so both coexist.

namespace esphome::lldp {

static const char *const TAG = "lldp";

static constexpr uint32_t INIT_RETRY_MS = 1000;
static constexpr uint32_t FAST_TX_MS = 1000;
static constexpr uint8_t MAX_RX_PER_LOOP = 4;
static constexpr UBaseType_t RX_QUEUE_DEPTH = 4;

// Advertise ourselves as an end station.
static constexpr uint16_t OUR_CAPABILITIES = CAP_STATION;

// A received LLDPDU handed from the Ethernet RX task to loop().
struct RxFrame {
  uint16_t len;
  uint8_t data[];
};

// The netif input hook is a plain function pointer with no context argument.
static LLDPComponent *instance = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void LLDPComponent::setup() {
  if (this->system_description_.empty()) {
#ifdef ESPHOME_PROJECT_NAME
    this->system_description_ = ESPHOME_PROJECT_NAME " " ESPHOME_PROJECT_VERSION " (ESPHome " ESPHOME_VERSION ")";
#else
    this->system_description_ = "ESPHome " ESPHOME_VERSION " " ESPHOME_BOARD;
#endif
  }
  instance = this;
  this->try_init_();
}

bool LLDPComponent::try_init_() {
  this->last_init_attempt_ms_ = millis();

  if (this->netif_ == nullptr) {
    this->netif_ = esp_netif_get_handle_from_ifkey(this->ifkey_.c_str());
    if (this->netif_ == nullptr) {
      ESP_LOGV(TAG, "Interface '%s' not available yet", this->ifkey_.c_str());
      return false;
    }
  }
  // For Ethernet the netif glue registers the esp_eth_handle_t as IO driver.
  this->eth_ = static_cast<esp_eth_handle_t>(esp_netif_get_io_driver(this->netif_));
  if (this->eth_ == nullptr) {
    ESP_LOGV(TAG, "Interface '%s' has no driver attached yet", this->ifkey_.c_str());
    return false;
  }

  esp_err_t err = esp_eth_ioctl(this->eth_, ETH_CMD_G_MAC_ADDR, this->mac_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Interface '%s' is not an Ethernet interface: %s", this->ifkey_.c_str(), esp_err_to_name(err));
    this->mark_failed();
    return false;
  }

  if (this->receive_) {
    this->rx_queue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(RxFrame *));
    if (this->rx_queue_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate receive queue");
      this->mark_failed();
      return false;
    }
    this->lwip_netif_ = static_cast<struct netif *>(esp_netif_get_netif_impl(this->netif_));
    this->enable_multicast_rx_();
  }

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &LLDPComponent::eth_event_handler_, this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &LLDPComponent::eth_event_handler_, this);
#if CONFIG_LWIP_IPV6
  esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &LLDPComponent::eth_event_handler_, this);
#endif

  // The link may already be up if we initialised late.
  this->link_up_ = esp_netif_is_netif_up(this->netif_);
  this->link_changed_ = true;
  this->initialized_ = true;
  ESP_LOGD(TAG, "Bound to '%s' (link %s)", this->ifkey_.c_str(), this->link_up_ ? "up" : "down");
  return true;
}

void LLDPComponent::enable_multicast_rx_() {
  // LLDP is sent to 01:80:C2:00:00:0E, which the MAC must be told to accept.
  // Before IDF 5.5 the ESP32 EMAC passed all multicast by default; since 5.5
  // it needs an explicit filter entry.
  esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  uint8_t mac[6];
  memcpy(mac, LLDP_MULTICAST_MAC, sizeof(mac));
  err = esp_eth_ioctl(this->eth_, ETH_CMD_ADD_MAC_FILTER, mac);
  if (err == ESP_OK) {
    ESP_LOGD(TAG, "Added LLDP multicast MAC filter");
  }
#endif
  if (this->promiscuous_) {
    bool on = true;
    esp_err_t perr = esp_eth_ioctl(this->eth_, ETH_CMD_S_PROMISCUOUS, &on);
    if (perr == ESP_OK) {
      ESP_LOGD(TAG, "Promiscuous mode enabled");
    } else {
      ESP_LOGW(TAG, "Could not enable promiscuous mode: %s", esp_err_to_name(perr));
    }
  } else if (err != ESP_OK) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    ESP_LOGW(TAG,
             "Ethernet MAC can't filter for the LLDP multicast address (%s); neighbors may not be received. "
             "Set 'promiscuous: true' if nothing shows up (needed for W5500).",
             esp_err_to_name(err));
#endif
  }
}

// esp_netif_start() (every ETHERNET_EVENT_START, including after
// ethernet.disable/enable) re-runs netif_add(), which resets netif->input, so
// check and re-chain from loop() rather than hooking once.
void LLDPComponent::ensure_rx_hook_() {
  struct netif *nif = this->lwip_netif_;
  if (nif == nullptr)
    return;
  netif_input_fn current = nif->input;
  if (current == nullptr || current == &LLDPComponent::netif_input_hook_)
    return;
  this->next_input_ = current;
  nif->input = &LLDPComponent::netif_input_hook_;
  ESP_LOGD(TAG, "Receive hook installed");
}

err_t LLDPComponent::netif_input_hook_(struct pbuf *p, struct netif *inp) {
  LLDPComponent *self = instance;
  if (p->len >= LLDP_ETH_HEADER_LEN) {
    const uint8_t *d = static_cast<const uint8_t *>(p->payload);
    if (d[12] == (LLDP_ETHERTYPE >> 8) && d[13] == (LLDP_ETHERTYPE & 0xFF)) {
      uint16_t len = p->tot_len > LLDP_MAX_FRAME ? LLDP_MAX_FRAME : p->tot_len;
      auto *f = static_cast<RxFrame *>(malloc(sizeof(RxFrame) + len));  // NOLINT(cppcoreguidelines-no-malloc)
      if (f != nullptr) {
        f->len = pbuf_copy_partial(p, f->data, len, 0);
        if (xQueueSend(self->rx_queue_, &f, 0) != pdTRUE) {
          free(f);  // NOLINT(cppcoreguidelines-no-malloc)
          self->rx_dropped_++;
        }
      } else {
        self->rx_dropped_++;
      }
      // lwIP has no use for it (it would drop the unknown ethertype anyway).
      pbuf_free(p);
      return ERR_OK;
    }
  }
  return self->next_input_(p, inp);
}

void LLDPComponent::eth_event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data) {
  // Runs in the event loop task: only touch atomics here.
  auto *self = static_cast<LLDPComponent *>(arg);
  if (base == ETH_EVENT) {
    if (data == nullptr || *static_cast<esp_eth_handle_t *>(data) != self->eth_)
      return;
    if (id == ETHERNET_EVENT_CONNECTED) {
      self->link_up_ = true;
      self->link_changed_ = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
      self->link_up_ = false;
      self->link_changed_ = true;
    }
  } else if (base == IP_EVENT) {
    // New addresses: re-advertise so the switch sees the management address promptly.
    self->tx_now_ = true;
  }
}

void LLDPComponent::restart_tx_() {
  // 802.1AB fast start: a short burst at 1s spacing, then normal interval.
  // Skip the first second; the PHY often isn't ready to transmit yet.
  this->fast_remaining_ = this->tx_fast_count_;
  this->next_tx_ms_ = millis() + FAST_TX_MS;
}

void LLDPComponent::loop() {
  if (!this->initialized_) {
    if (millis() - this->last_init_attempt_ms_ >= INIT_RETRY_MS)
      this->try_init_();
    return;
  }

  if (this->link_changed_.exchange(false)) {
    if (this->link_up_) {
      ESP_LOGD(TAG, "Link up");
      this->restart_tx_();
    } else {
      ESP_LOGD(TAG, "Link down");
      this->expire_neighbor_("link down");
    }
  }

  this->process_rx_();

  const uint32_t now = millis();
  if (this->has_neighbor_ && (int32_t) (now - this->neighbor_expires_ms_) >= 0)
    this->expire_neighbor_("TTL expired");

  bool due = this->tx_now_.exchange(false) || (int32_t) (now - this->next_tx_ms_) >= 0;
  if (this->transmit_ && this->link_up_ && due) {
    uint32_t ttl = this->tx_interval_s_ * this->tx_hold_ + 1;
    this->send_frame_(ttl > 65535 ? 65535 : ttl);
    if (this->fast_remaining_ > 0) {
      this->fast_remaining_--;
      this->next_tx_ms_ = now + FAST_TX_MS;
    } else {
      this->next_tx_ms_ = now + this->tx_interval_s_ * 1000;
    }
  }
}

void LLDPComponent::process_rx_() {
  if (this->rx_queue_ == nullptr)
    return;
  this->ensure_rx_hook_();
  RxFrame *f;
  for (uint8_t i = 0; i < MAX_RX_PER_LOOP && xQueueReceive(this->rx_queue_, &f, 0) == pdTRUE; i++) {
    // Ignore our own frames if the MAC loops them back.
    bool own = f->len >= 12 && memcmp(f->data + 6, this->mac_, 6) == 0;
    LLDPNeighbor nb;
    bool ok = !own && parse_lldp_frame(f->data, f->len, nb);
    if (!own && !ok) {
      this->rx_bad_++;
      ESP_LOGV(TAG, "Dropped malformed LLDPDU (%u bytes)", f->len);
    }
    free(f);  // NOLINT(cppcoreguidelines-no-malloc)
    if (ok) {
      this->rx_ok_++;
      this->handle_neighbor_(std::move(nb));
    }
  }
}

static bool same_content(const LLDPNeighbor &a, const LLDPNeighbor &b) {
  return a.source_mac == b.source_mac && a.chassis_id == b.chassis_id && a.port_id == b.port_id &&
         a.port_description == b.port_description && a.system_name == b.system_name &&
         a.system_description == b.system_description && a.management_address == b.management_address &&
         a.capabilities == b.capabilities && a.vlan_id == b.vlan_id;
}

void LLDPComponent::handle_neighbor_(LLDPNeighbor &&n) {
  if (n.ttl == 0) {
    // Shutdown LLDPDU: the neighbor is going away.
    if (this->has_neighbor_ && this->neighbor_.same_port(n))
      this->expire_neighbor_("shutdown received");
    return;
  }

  bool changed = !this->has_neighbor_ || !same_content(this->neighbor_, n);
  if (this->has_neighbor_ && !this->neighbor_.same_port(n)) {
    ESP_LOGI(TAG, "Neighbor changed from %s/%s to %s/%s", this->neighbor_.chassis_id.c_str(),
             this->neighbor_.port_id.c_str(), n.chassis_id.c_str(), n.port_id.c_str());
  }
  this->neighbor_expires_ms_ = millis() + uint32_t(n.ttl) * 1000;
  this->neighbor_ = std::move(n);
  this->has_neighbor_ = true;

  if (!changed) {
    ESP_LOGV(TAG, "Refreshed neighbor %s (ttl %us)", this->neighbor_.system_name.c_str(), this->neighbor_.ttl);
    return;
  }

  const auto &nb = this->neighbor_;
  ESP_LOGI(TAG, "Neighbor: '%s' port '%s' (%s)", nb.system_name.c_str(), nb.port_id.c_str(),
           nb.port_description.c_str());
  ESP_LOGD(TAG,
           "  Chassis ID: %s\n"
           "  Source MAC: %s\n"
           "  Mgmt address: %s\n"
           "  Capabilities: %s\n"
           "  VLAN: %d\n"
           "  TTL: %us",
           nb.chassis_id.c_str(), nb.source_mac.c_str(), nb.management_address.c_str(), nb.capabilities.c_str(),
           nb.vlan_id, nb.ttl);
  this->publish_neighbor_();
  this->neighbor_trigger_.trigger(this->neighbor_);
}

void LLDPComponent::expire_neighbor_(const char *reason) {
  if (!this->has_neighbor_)
    return;
  ESP_LOGI(TAG, "Neighbor '%s' lost (%s)", this->neighbor_.system_name.c_str(), reason);
  this->has_neighbor_ = false;
  this->neighbor_ = LLDPNeighbor{};
  this->publish_neighbor_();
  this->neighbor_lost_trigger_.trigger();
}

void LLDPComponent::publish_neighbor_() {
  const auto &nb = this->neighbor_;
#ifdef USE_TEXT_SENSOR
  for (auto &fs : this->text_sensors_) {
    const std::string *v = nullptr;
    switch (fs.field) {
      case FIELD_SYSTEM_NAME:
        v = &nb.system_name;
        break;
      case FIELD_SYSTEM_DESCRIPTION:
        v = &nb.system_description;
        break;
      case FIELD_CHASSIS_ID:
        v = &nb.chassis_id;
        break;
      case FIELD_PORT_ID:
        v = &nb.port_id;
        break;
      case FIELD_PORT_DESCRIPTION:
        v = &nb.port_description;
        break;
      case FIELD_MANAGEMENT_ADDRESS:
        v = &nb.management_address;
        break;
      case FIELD_CAPABILITIES:
        v = &nb.capabilities;
        break;
      case FIELD_SOURCE_MAC:
        v = &nb.source_mac;
        break;
    }
    if (v != nullptr)
      fs.sensor->publish_state(*v);
  }
#endif
#ifdef USE_SENSOR
  if (this->vlan_id_sensor_ != nullptr)
    this->vlan_id_sensor_->publish_state(nb.vlan_id >= 0 ? float(nb.vlan_id) : NAN);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->present_sensor_ != nullptr)
    this->present_sensor_->publish_state(this->has_neighbor_);
#endif
}

size_t LLDPComponent::build_frame_(uint8_t *buf, size_t cap, uint16_t ttl) {
  LLDPFrameBuilder b(buf, cap);
  bool ok = b.begin(this->mac_);
  ok = ok && b.add_subtyped(TLV_CHASSIS_ID, 4 /* MAC address */, this->mac_, 6);
  ok = ok && b.add_subtyped(TLV_PORT_ID, 5 /* interface name */, this->port_id_.data(), this->port_id_.size());
  ok = ok && b.add_ttl(ttl);
  if (!ok)
    return 0;
  // Optional TLVs: skip any that don't fit rather than failing the frame.
  if (!this->port_description_.empty())
    b.add_string(TLV_PORT_DESCRIPTION, this->port_description_);
  if (!this->system_name_.empty())
    b.add_string(TLV_SYSTEM_NAME, this->system_name_);
  if (!this->system_description_.empty())
    b.add_string(TLV_SYSTEM_DESCRIPTION, this->system_description_);
  b.add_capabilities(OUR_CAPABILITIES, OUR_CAPABILITIES);

  if (this->management_address_ && ttl != 0) {
    uint32_t if_index = esp_netif_get_netif_impl_index(this->netif_);
    esp_netif_ip_info_t ip4;
    if (esp_netif_get_ip_info(this->netif_, &ip4) == ESP_OK && ip4.ip.addr != 0) {
      // esp_ip4_addr_t is stored in network byte order.
      b.add_management_address(1, reinterpret_cast<const uint8_t *>(&ip4.ip.addr), 4, if_index);
    }
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ip6[LWIP_IPV6_NUM_ADDRESSES];
    int n6 = esp_netif_get_all_ip6(this->netif_, ip6);
    for (int i = 0; i < n6; i++)
      b.add_management_address(2, reinterpret_cast<const uint8_t *>(ip6[i].addr), 16, if_index);
#endif
  }
  return b.finish();
}

void LLDPComponent::send_frame_(uint16_t ttl) {
  // Worst case: header + 3 strings of 255 + a handful of mgmt addresses; 1.1 KB
  // keeps us well under the Ethernet MTU and off the heap.
  uint8_t buf[1100];
  size_t len = this->build_frame_(buf, sizeof(buf), ttl);
  if (len == 0) {
    ESP_LOGE(TAG, "Failed to build LLDPDU");
    return;
  }
  esp_err_t err = esp_eth_transmit(this->eth_, buf, len);
  if (err != ESP_OK) {
    this->tx_err_++;
    // Failures while the link is (re)negotiating are expected; keep it quiet.
    ESP_LOGD(TAG, "LLDPDU transmit failed: %s", esp_err_to_name(err));
    return;
  }
  this->tx_ok_++;
  ESP_LOGV(TAG, "Sent %u-byte LLDPDU (ttl %u)", (unsigned) len, ttl);
}

void LLDPComponent::on_shutdown() {
  // Tell the switch we're leaving so it drops us immediately instead of
  // waiting for the TTL to run out.
  if (this->initialized_ && this->transmit_ && this->link_up_)
    this->send_frame_(0);
}

void LLDPComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LLDP:\n"
                "  Interface: %s\n"
                "  Transmit: %s\n"
                "  Receive: %s%s",
                this->ifkey_.c_str(), YESNO(this->transmit_), YESNO(this->receive_),
                this->promiscuous_ ? " (promiscuous)" : "");
  if (this->transmit_) {
    ESP_LOGCONFIG(TAG,
                  "  Port ID: %s\n"
                  "  Port Description: %s\n"
                  "  System Name: %s\n"
                  "  System Description: %s\n"
                  "  TX Interval: %us, Hold: %u (TTL %us), Fast Count: %u",
                  this->port_id_.c_str(), this->port_description_.c_str(), this->system_name_.c_str(),
                  this->system_description_.c_str(), (unsigned) this->tx_interval_s_, this->tx_hold_,
                  (unsigned) (this->tx_interval_s_ * this->tx_hold_ + 1), this->tx_fast_count_);
  }
  if (!this->initialized_) {
    ESP_LOGCONFIG(TAG, "  Not bound to interface yet");
  }
  if (this->has_neighbor_) {
    ESP_LOGCONFIG(TAG, "  Neighbor: %s port %s", this->neighbor_.system_name.c_str(), this->neighbor_.port_id.c_str());
  }
}

}  // namespace esphome::lldp
