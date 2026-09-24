#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ETHERNET)

#include <array>
#include <atomic>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include <esp_eth.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lwip/netif.h>

#include "lldp_frame.h"

namespace esphome::lldp {

enum NeighborField : uint8_t {
  FIELD_SYSTEM_NAME,
  FIELD_SYSTEM_DESCRIPTION,
  FIELD_CHASSIS_ID,
  FIELD_PORT_ID,
  FIELD_PORT_DESCRIPTION,
  FIELD_MANAGEMENT_ADDRESS,
  FIELD_CAPABILITIES,
  FIELD_SOURCE_MAC,
  FIELD_COUNT,
};

class LLDPComponent final : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // The ethernet component (setup_priority::ETHERNET) must have created its netif first.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_transmit(bool transmit) { this->transmit_ = transmit; }
  void set_receive(bool receive) { this->receive_ = receive; }
  void set_promiscuous(bool promiscuous) { this->promiscuous_ = promiscuous; }
  void set_management_address(bool management_address) { this->management_address_ = management_address; }
  void set_port_id(const char *port_id) { this->port_id_ = port_id; }
  void set_port_description(const char *port_description) { this->port_description_ = port_description; }
  void set_system_name(const char *system_name) { this->system_name_ = system_name; }
  void set_system_description(const char *system_description) { this->system_description_ = system_description; }
  void set_tx_interval(uint16_t seconds) { this->tx_interval_s_ = seconds; }
  void set_tx_hold(uint8_t tx_hold) { this->tx_hold_ = tx_hold; }
  void set_tx_fast_count(uint8_t tx_fast_count) { this->tx_fast_count_ = tx_fast_count; }

#ifdef USE_TEXT_SENSOR
  void set_text_sensor(NeighborField field, text_sensor::TextSensor *sensor) { this->text_sensors_[field] = sensor; }
#endif
#ifdef USE_SENSOR
  void set_vlan_id_sensor(sensor::Sensor *sensor) { this->vlan_id_sensor_ = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_neighbor_present_sensor(binary_sensor::BinarySensor *sensor) { this->present_sensor_ = sensor; }
#endif

  /// Called when a neighbor appears or its advertised data changes.
  template<typename F> void add_on_neighbor_callback(F &&callback) {
    this->neighbor_callback_.add(std::forward<F>(callback));
  }
  /// Called when the neighbor's TTL expires, the link drops, or it announces shutdown.
  template<typename F> void add_on_neighbor_lost_callback(F &&callback) {
    this->neighbor_lost_callback_.add(std::forward<F>(callback));
  }

  bool has_neighbor() const { return this->has_neighbor_; }
  const LLDPNeighbor &get_neighbor() const { return this->neighbor_; }
  /// Send an LLDPDU on the next loop iteration. Safe to call from any task.
  void send_now() {
    this->tx_now_ = true;
    this->enable_loop_soon_any_context();
  }

 protected:
  bool try_init_();
  void enable_multicast_rx_();
  void ensure_rx_hook_();
  void tick_();
  void restart_tx_();
  void process_rx_();
  void handle_neighbor_(const LLDPNeighbor &neighbor);
  void expire_neighbor_(const LogString *reason);
  void publish_neighbor_();
#ifdef USE_LLDP_MDNS
  void publish_mdns_();
#endif
  void send_frame_(uint16_t ttl);
  size_t build_frame_(uint8_t *buf, size_t cap, uint16_t ttl);
  uint16_t ttl_() const;

  static void event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data);
  // Chained in front of the lwIP netif input function; runs in the Ethernet RX task.
  static err_t netif_input_hook_(struct pbuf *p, struct netif *inp);

  // Configuration
  const char *port_id_{"eth0"};
  const char *port_description_{""};
  const char *system_name_{nullptr};
  const char *system_description_{nullptr};
  uint16_t tx_interval_s_{30};
  uint8_t tx_hold_{4};
  uint8_t tx_fast_count_{4};
  bool transmit_{true};
  bool receive_{true};
  bool promiscuous_{false};
  bool management_address_{true};

  // Driver binding
  esp_netif_t *netif_{nullptr};
  esp_eth_handle_t eth_{nullptr};
  struct netif *lwip_netif_{nullptr};
  netif_input_fn next_input_{nullptr};  // the input function we chain to (normally tcpip_input)
  QueueHandle_t rx_queue_{nullptr};     // received LLDP pbufs, handed from the RX task to loop()
  uint8_t mac_[6]{};
  bool initialized_{false};

  // Set from the event loop / RX task
  std::atomic<bool> link_up_{false};
  std::atomic<bool> link_changed_{false};
  std::atomic<bool> tx_now_{false};
  std::atomic<uint32_t> rx_dropped_{0};

  // Transmit schedule, advanced by the 1 s tick
  uint16_t tx_countdown_s_{0};
  uint8_t fast_remaining_{0};

  LLDPNeighbor neighbor_{};
  bool has_neighbor_{false};
  uint32_t neighbor_expires_ms_{0};
#ifdef USE_LLDP_MDNS
  bool mdns_pending_{false};
#endif

#ifdef USE_TEXT_SENSOR
  std::array<text_sensor::TextSensor *, FIELD_COUNT> text_sensors_{};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *vlan_id_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *present_sensor_{nullptr};
#endif

  LazyCallbackManager<void(const LLDPNeighbor &)> neighbor_callback_;
  LazyCallbackManager<void()> neighbor_lost_callback_;
};

}  // namespace esphome::lldp

#endif  // USE_ESP32 && USE_ETHERNET
