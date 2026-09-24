#include "lldp_component.h"

#if defined(USE_ESP32) && defined(USE_ETHERNET)

#include <cinttypes>
#include <cmath>
#include <cstring>

#include <esp_idf_version.h>
#include <esp_netif_net_stack.h>
#include <lwip/opt.h>
#include <lwip/pbuf.h>

#ifdef USE_LLDP_MDNS
#include <mdns.h>
#endif

#include "esphome/components/ethernet/ethernet_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/version.h"

// Deliberately no ESP-IDF L2 TAP: components that hook the Ethernet driver's
// input path (e.g. for receive timestamps) back off when CONFIG_ESP_NETIF_L2_TAP
// is set. Frames are sent with esp_eth_transmit() and received by chaining the
// lwIP netif input function, which runs after any driver-level hook.

namespace esphome::lldp {

static const char *const TAG = "lldp";

static constexpr uint32_t TICK_MS = 1000;
static constexpr UBaseType_t RX_QUEUE_DEPTH = 4;
// Largest frame we build: header, three 128-byte strings, and up to 1 + LWIP_IPV6_NUM_ADDRESSES
// management addresses. See the length limits in __init__.py.
static constexpr size_t TX_BUFFER_SIZE = 768;
// Advertise ourselves as an end station.
static constexpr uint16_t OUR_CAPABILITIES = CAP_STATION;

#ifdef USE_LLDP_MDNS
static const char *const MDNS_SERVICE = "_esphomelib";
static const char *const MDNS_PROTO = "_tcp";
#endif

// The netif input hook is a plain function pointer with no context argument.
static LLDPComponent *global_lldp_component = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void LLDPComponent::setup() {
  // Default to the hostname; that's what switches show in their neighbor tables.
  if (this->system_name_ == nullptr)
    this->system_name_ = App.get_name().c_str();
  if (this->system_description_ == nullptr) {
#ifdef ESPHOME_PROJECT_NAME
    this->system_description_ = ESPHOME_PROJECT_NAME " " ESPHOME_PROJECT_VERSION " (ESPHome " ESPHOME_VERSION ")";
#else
    this->system_description_ = "ESPHome " ESPHOME_VERSION " " ESPHOME_BOARD;
#endif
  }
  global_lldp_component = this;
#ifdef USE_BINARY_SENSOR
  // No neighbor is known yet; report "off" rather than leaving the entity unknown until one shows up.
  if (this->present_sensor_ != nullptr)
    this->present_sensor_->publish_state(false);
#endif
  this->set_interval(TICK_MS, [this]() { this->tick_(); });
  this->try_init_();
}

bool LLDPComponent::try_init_() {
  auto *eth = ethernet::global_eth_component;
  if (eth == nullptr || eth->get_esp_netif() == nullptr || eth->get_eth_handle() == nullptr) {
    ESP_LOGV(TAG, "Ethernet driver not installed yet");
    return false;
  }
  this->netif_ = eth->get_esp_netif();
  this->eth_ = eth->get_eth_handle();

  esp_err_t err = esp_eth_ioctl(this->eth_, ETH_CMD_G_MAC_ADDR, this->mac_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Reading MAC address failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return false;
  }

  if (this->receive_) {
    this->rx_queue_ = xQueueCreate(RX_QUEUE_DEPTH, sizeof(struct pbuf *));
    if (this->rx_queue_ == nullptr) {
      ESP_LOGE(TAG, "Receive queue allocation failed");
      this->mark_failed();
      return false;
    }
    this->lwip_netif_ = static_cast<struct netif *>(esp_netif_get_netif_impl(this->netif_));
    this->enable_multicast_rx_();
    this->ensure_rx_hook_();
  }

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &LLDPComponent::event_handler_, this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &LLDPComponent::event_handler_, this);
#if CONFIG_LWIP_IPV6
  esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &LLDPComponent::event_handler_, this);
#endif

  // The link may already be up if the driver started before we bound to it.
  this->link_up_ = esp_netif_is_netif_up(this->netif_);
  this->link_changed_ = true;
  this->initialized_ = true;
  this->enable_loop();
  return true;
}

void LLDPComponent::enable_multicast_rx_() {
  // LLDP is sent to 01:80:C2:00:00:0E. Before IDF 5.5 the ESP32 EMAC passed all
  // multicast; since 5.5 it needs a filter entry. SPI MACs such as the W5500
  // can't filter for it at all and need promiscuous mode.
  esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  uint8_t mac[6];
  memcpy(mac, LLDP_MULTICAST_MAC, sizeof(mac));
  err = esp_eth_ioctl(this->eth_, ETH_CMD_ADD_MAC_FILTER, mac);
#endif
  if (this->promiscuous_) {
    bool on = true;
    esp_err_t perr = esp_eth_ioctl(this->eth_, ETH_CMD_S_PROMISCUOUS, &on);
    if (perr != ESP_OK) {
      ESP_LOGW(TAG, "Enabling promiscuous mode failed: %s", esp_err_to_name(perr));
    }
  } else if (err != ESP_OK) {
    ESP_LOGW(TAG, "MAC can't filter for the LLDP address (%s); set 'promiscuous: true' if no neighbor appears",
             esp_err_to_name(err));
  }
}

// esp_netif_start() (every ETHERNET_EVENT_START, including after
// ethernet.disable/enable) re-runs netif_add(), which resets netif->input,
// so this is re-checked on every tick rather than hooked once.
void LLDPComponent::ensure_rx_hook_() {
  struct netif *nif = this->lwip_netif_;
  if (nif == nullptr)
    return;
  netif_input_fn current = nif->input;
  if (current == nullptr || current == &LLDPComponent::netif_input_hook_)
    return;
  this->next_input_ = current;
  nif->input = &LLDPComponent::netif_input_hook_;
  ESP_LOGV(TAG, "Receive hook installed");
}

err_t LLDPComponent::netif_input_hook_(struct pbuf *p, struct netif *inp) {
  LLDPComponent *self = global_lldp_component;
  if (p->len >= LLDP_ETH_HEADER_LEN) {
    const auto *d = static_cast<const uint8_t *>(p->payload);
    if (d[12] == (LLDP_ETHERTYPE >> 8) && d[13] == (LLDP_ETHERTYPE & 0xFF)) {
      // Hand the pbuf itself to loop(); it is freed there. lwIP would only drop it.
      if (xQueueSend(self->rx_queue_, &p, 0) == pdTRUE) {
        self->enable_loop_soon_any_context();
      } else {
        pbuf_free(p);
        self->rx_dropped_++;
      }
      return ERR_OK;
    }
  }
  return self->next_input_(p, inp);
}

void LLDPComponent::event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data) {
  // Runs in the event loop task: only touch atomics here.
  auto *self = static_cast<LLDPComponent *>(arg);
  if (base == ETH_EVENT) {
    if (data == nullptr || *static_cast<esp_eth_handle_t *>(data) != self->eth_)
      return;
    if (id == ETHERNET_EVENT_CONNECTED) {
      self->link_up_ = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
      self->link_up_ = false;
    } else {
      return;
    }
    self->link_changed_ = true;
  } else {
    // New address: advertise it now rather than at the next interval.
    if (data == nullptr)
      return;
    esp_netif_t *netif = id == IP_EVENT_ETH_GOT_IP ? static_cast<ip_event_got_ip_t *>(data)->esp_netif
                                                   : static_cast<ip_event_got_ip6_t *>(data)->esp_netif;
    if (netif != self->netif_)
      return;
    self->tx_now_ = true;
  }
  self->enable_loop_soon_any_context();
}

uint16_t LLDPComponent::ttl_() const {
  uint32_t ttl = uint32_t(this->tx_interval_s_) * this->tx_hold_ + 1;
  return ttl > 65535 ? 65535 : ttl;
}

void LLDPComponent::restart_tx_() {
  // 802.1AB fast start: a burst at 1 s spacing, then the normal interval. Skip
  // the first second; the PHY often can't transmit yet.
  this->fast_remaining_ = this->tx_fast_count_ > 0 ? this->tx_fast_count_ - 1 : 0;
  this->tx_countdown_s_ = 2;
}

// Event-driven work only; everything periodic runs in tick_().
void LLDPComponent::loop() {
  if (this->initialized_) {
    if (this->link_changed_.exchange(false)) {
      if (this->link_up_) {
        ESP_LOGD(TAG, "Link up");
        this->restart_tx_();
      } else {
        ESP_LOGD(TAG, "Link down");
        this->tx_countdown_s_ = 0;
        this->expire_neighbor_(LOG_STR("link down"));
      }
    }
    if (this->tx_now_.exchange(false) && this->transmit_ && this->link_up_) {
      this->send_frame_(this->ttl_());
    }
    this->process_rx_();
  }
  this->disable_loop();
}

void LLDPComponent::tick_() {
  if (!this->initialized_) {
    this->try_init_();
    return;
  }
  if (this->receive_)
    this->ensure_rx_hook_();

  if (this->has_neighbor_ && static_cast<int32_t>(millis() - this->neighbor_expires_ms_) >= 0)
    this->expire_neighbor_(LOG_STR("TTL expired"));

#ifdef USE_LLDP_MDNS
  if (this->mdns_pending_)
    this->publish_mdns_();
#endif

  if (!this->transmit_ || !this->link_up_ || this->tx_countdown_s_ == 0 || --this->tx_countdown_s_ > 0)
    return;
  this->send_frame_(this->ttl_());
  if (this->fast_remaining_ > 0) {
    this->fast_remaining_--;
    this->tx_countdown_s_ = 1;
  } else {
    this->tx_countdown_s_ = this->tx_interval_s_;
  }
}

void LLDPComponent::process_rx_() {
  if (this->rx_queue_ == nullptr)
    return;
  struct pbuf *p;
  while (xQueueReceive(this->rx_queue_, &p, 0) == pdTRUE) {
    const auto *data = static_cast<const uint8_t *>(p->payload);
    // Ignore our own frames if the MAC loops them back.
    if (memcmp(data + 6, this->mac_, 6) != 0) {
      LLDPNeighbor neighbor;
      // The Ethernet glue hands lwIP a single contiguous pbuf.
      if (p->len == p->tot_len && parse_lldp_frame(data, p->len, neighbor)) {
        this->handle_neighbor_(neighbor);
      } else {
        ESP_LOGV(TAG, "Dropped malformed LLDPDU (%u bytes)", p->tot_len);
      }
    }
    pbuf_free(p);
  }
}

void LLDPComponent::handle_neighbor_(const LLDPNeighbor &neighbor) {
  if (neighbor.ttl == 0) {
    // Shutdown LLDPDU: the neighbor is going away.
    if (this->has_neighbor_ && this->neighbor_.same_port(neighbor))
      this->expire_neighbor_(LOG_STR("shutdown received"));
    return;
  }

  bool changed = !this->has_neighbor_ || !this->neighbor_.same_content(neighbor);
  if (this->has_neighbor_ && !this->neighbor_.same_port(neighbor)) {
    if (!should_replace_neighbor(this->neighbor_, neighbor)) {
      ESP_LOGV(TAG, "Ignoring station '%s' while bridge '%s' is known", neighbor.system_name,
               this->neighbor_.system_name);
      return;
    }
    ESP_LOGI(TAG, "Neighbor changed from %s/%s to %s/%s", this->neighbor_.chassis_id, this->neighbor_.port_id,
             neighbor.chassis_id, neighbor.port_id);
  }
  this->neighbor_expires_ms_ = millis() + uint32_t(neighbor.ttl) * 1000;
  this->neighbor_ = neighbor;
  this->has_neighbor_ = true;
  if (!changed)
    return;

  const auto &nb = this->neighbor_;
  ESP_LOGI(TAG, "Neighbor '%s' port '%s' (%s)", nb.system_name, nb.port_id, nb.port_description);
  ESP_LOGD(TAG,
           "  Chassis ID: %s\n"
           "  Source MAC: %s\n"
           "  Management address: %s\n"
           "  Capabilities: %s\n"
           "  VLAN: %d\n"
           "  TTL: %us",
           nb.chassis_id, nb.source_mac, nb.management_address, nb.capabilities, nb.vlan_id, nb.ttl);
  this->publish_neighbor_();
  this->neighbor_callback_.call(this->neighbor_);
}

void LLDPComponent::expire_neighbor_(const LogString *reason) {
  if (!this->has_neighbor_)
    return;
  ESP_LOGI(TAG, "Neighbor '%s' lost (%s)", this->neighbor_.system_name, LOG_STR_ARG(reason));
  this->has_neighbor_ = false;
  this->neighbor_.clear();
  this->publish_neighbor_();
  this->neighbor_lost_callback_.call();
}

void LLDPComponent::publish_neighbor_() {
  const auto &nb = this->neighbor_;
#ifdef USE_TEXT_SENSOR
  const char *values[FIELD_COUNT] = {
      nb.system_name,      nb.system_description, nb.chassis_id,   nb.port_id,
      nb.port_description, nb.management_address, nb.capabilities, nb.source_mac,
  };
  for (size_t i = 0; i < FIELD_COUNT; i++) {
    if (this->text_sensors_[i] != nullptr)
      this->text_sensors_[i]->publish_state(values[i]);
  }
#endif
#ifdef USE_SENSOR
  if (this->vlan_id_sensor_ != nullptr)
    this->vlan_id_sensor_->publish_state(nb.vlan_id >= 0 ? static_cast<float>(nb.vlan_id) : NAN);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->present_sensor_ != nullptr)
    this->present_sensor_->publish_state(this->has_neighbor_);
#endif
#ifdef USE_LLDP_MDNS
  this->publish_mdns_();
#endif
}

#ifdef USE_LLDP_MDNS
// Adds lldp_switch / lldp_port / lldp_vlan TXT records to the native API
// service, updated at runtime (the mdns component only evaluates TXT values at
// boot). Retried from tick_() until the mdns component has registered it.
void LLDPComponent::publish_mdns_() {
  if (!mdns_service_exists(MDNS_SERVICE, MDNS_PROTO, nullptr)) {
    this->mdns_pending_ = true;
    return;
  }
  this->mdns_pending_ = false;
  const auto &nb = this->neighbor_;
  if (this->has_neighbor_) {
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "lldp_switch", nb.system_name);
    mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "lldp_port", nb.port_id);
    if (nb.vlan_id >= 0) {
      char vlan[8];
      snprintf(vlan, sizeof(vlan), "%d", nb.vlan_id);
      mdns_service_txt_item_set(MDNS_SERVICE, MDNS_PROTO, "lldp_vlan", vlan);
    } else {
      mdns_service_txt_item_remove(MDNS_SERVICE, MDNS_PROTO, "lldp_vlan");
    }
  } else {
    mdns_service_txt_item_remove(MDNS_SERVICE, MDNS_PROTO, "lldp_switch");
    mdns_service_txt_item_remove(MDNS_SERVICE, MDNS_PROTO, "lldp_port");
    mdns_service_txt_item_remove(MDNS_SERVICE, MDNS_PROTO, "lldp_vlan");
  }
}
#endif

size_t LLDPComponent::build_frame_(uint8_t *buf, size_t cap, uint16_t ttl) {
  LLDPFrameBuilder b(buf, cap);
  bool ok = b.begin(this->mac_);
  ok = ok && b.add_subtyped(TLV_CHASSIS_ID, CHASSIS_SUBTYPE_MAC, this->mac_, 6);
  ok = ok && b.add_subtyped(TLV_PORT_ID, PORT_SUBTYPE_INTERFACE_NAME, this->port_id_, strlen(this->port_id_));
  ok = ok && b.add_ttl(ttl);
  if (!ok)
    return 0;
  // Optional TLVs: skip any that don't fit rather than failing the frame.
  if (this->port_description_[0] != '\0')
    b.add_string(TLV_PORT_DESCRIPTION, this->port_description_);
  if (this->system_name_[0] != '\0')
    b.add_string(TLV_SYSTEM_NAME, this->system_name_);
  if (this->system_description_[0] != '\0')
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
  uint8_t buf[TX_BUFFER_SIZE];
  size_t len = this->build_frame_(buf, sizeof(buf), ttl);
  if (len == 0) {
    ESP_LOGE(TAG, "Building LLDPDU failed");
    return;
  }
  esp_err_t err = esp_eth_transmit(this->eth_, buf, len);
  if (err != ESP_OK) {
    // Expected while the link is (re)negotiating.
    ESP_LOGD(TAG, "Transmit failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGV(TAG, "Sent %u-byte LLDPDU (TTL %u)", (unsigned) len, ttl);
}

void LLDPComponent::on_shutdown() {
  // Tell the switch we're leaving so it drops us now rather than at TTL expiry.
  if (this->initialized_ && this->transmit_ && this->link_up_)
    this->send_frame_(0);
}

void LLDPComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LLDP:\n"
                "  Transmit: %s\n"
                "  Receive: %s\n"
                "  Promiscuous: %s\n"
                "  Port ID: %s\n"
                "  Port Description: %s\n"
                "  System Name: %s\n"
                "  System Description: %s\n"
                "  TX Interval: %us, Hold: %u (TTL %us), Fast Count: %u",
                YESNO(this->transmit_), YESNO(this->receive_), YESNO(this->promiscuous_), this->port_id_,
                this->port_description_, this->system_name_, this->system_description_, this->tx_interval_s_,
                this->tx_hold_, this->ttl_(), this->tx_fast_count_);
#ifdef USE_LLDP_MDNS
  ESP_LOGCONFIG(TAG, "  mDNS TXT: YES");
#endif
  if (!this->initialized_) {
    ESP_LOGCONFIG(TAG, "  Not bound to the Ethernet driver yet");
  }
  if (this->rx_dropped_ > 0) {
    ESP_LOGCONFIG(TAG, "  Dropped frames: %" PRIu32, this->rx_dropped_.load());
  }
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "System Name", this->text_sensors_[FIELD_SYSTEM_NAME]);
  LOG_TEXT_SENSOR("  ", "System Description", this->text_sensors_[FIELD_SYSTEM_DESCRIPTION]);
  LOG_TEXT_SENSOR("  ", "Chassis ID", this->text_sensors_[FIELD_CHASSIS_ID]);
  LOG_TEXT_SENSOR("  ", "Port ID", this->text_sensors_[FIELD_PORT_ID]);
  LOG_TEXT_SENSOR("  ", "Port Description", this->text_sensors_[FIELD_PORT_DESCRIPTION]);
  LOG_TEXT_SENSOR("  ", "Management Address", this->text_sensors_[FIELD_MANAGEMENT_ADDRESS]);
  LOG_TEXT_SENSOR("  ", "Capabilities", this->text_sensors_[FIELD_CAPABILITIES]);
  LOG_TEXT_SENSOR("  ", "Source MAC", this->text_sensors_[FIELD_SOURCE_MAC]);
#endif
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "VLAN ID", this->vlan_id_sensor_);
#endif
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Neighbor Present", this->present_sensor_);
#endif
}

}  // namespace esphome::lldp

#endif  // USE_ESP32 && USE_ETHERNET
