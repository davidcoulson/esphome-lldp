#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/netif.h"

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
};

class LLDPComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // Run after the ethernet component has created its esp_netif.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_interface(const std::string &ifkey) { this->ifkey_ = ifkey; }
  void set_transmit(bool v) { this->transmit_ = v; }
  void set_receive(bool v) { this->receive_ = v; }
  void set_promiscuous(bool v) { this->promiscuous_ = v; }
  void set_port_id(const std::string &v) { this->port_id_ = v; }
  void set_port_description(const std::string &v) { this->port_description_ = v; }
  void set_system_name(const std::string &v) { this->system_name_ = v; }
  void set_system_description(const std::string &v) { this->system_description_ = v; }
  void set_management_address(bool v) { this->management_address_ = v; }
  void set_tx_interval(uint32_t seconds) { this->tx_interval_s_ = seconds; }
  void set_tx_hold(uint8_t v) { this->tx_hold_ = v; }
  void set_tx_fast_count(uint8_t v) { this->tx_fast_count_ = v; }

#ifdef USE_TEXT_SENSOR
  void add_text_sensor(NeighborField field, text_sensor::TextSensor *s) { this->text_sensors_.push_back({field, s}); }
#endif
#ifdef USE_SENSOR
  void set_vlan_id_sensor(sensor::Sensor *s) { this->vlan_id_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_neighbor_present_sensor(binary_sensor::BinarySensor *s) { this->present_sensor_ = s; }
#endif

  Trigger<const LLDPNeighbor &> *get_neighbor_trigger() { return &this->neighbor_trigger_; }
  Trigger<> *get_neighbor_lost_trigger() { return &this->neighbor_lost_trigger_; }

  bool has_neighbor() const { return this->has_neighbor_; }
  const LLDPNeighbor &get_neighbor() const { return this->neighbor_; }
  /// Send an LLDPDU now (e.g. from a lambda after changing an IP).
  void send_now() { this->tx_now_ = true; }

 protected:
  bool try_init_();
  void enable_multicast_rx_();
  void ensure_rx_hook_();
  void process_rx_();
  void handle_neighbor_(LLDPNeighbor &&n);
  void expire_neighbor_(const char *reason);
  void publish_neighbor_();
  void send_frame_(uint16_t ttl);
  size_t build_frame_(uint8_t *buf, size_t cap, uint16_t ttl);
  void restart_tx_();

  static void eth_event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data);
  // Chained in front of the lwIP netif input function; runs in the Ethernet RX task.
  static err_t netif_input_hook_(struct pbuf *p, struct netif *inp);

  // Configuration
  std::string ifkey_;
  std::string port_id_;
  std::string port_description_;
  std::string system_name_;
  std::string system_description_;
  uint32_t tx_interval_s_{30};
  uint8_t tx_hold_{4};
  uint8_t tx_fast_count_{4};
  bool transmit_{true};
  bool receive_{true};
  bool promiscuous_{false};
  bool management_address_{true};

  // Runtime state
  esp_netif_t *netif_{nullptr};
  esp_eth_handle_t eth_{nullptr};
  struct netif *lwip_netif_{nullptr};
  netif_input_fn next_input_{nullptr};  // the input function we chain to (normally tcpip_input)
  QueueHandle_t rx_queue_{nullptr};
  std::atomic<uint32_t> rx_dropped_{0};
  uint8_t mac_[6]{};
  bool initialized_{false};
  uint32_t last_init_attempt_ms_{0};
  std::atomic<bool> link_up_{false};
  std::atomic<bool> link_changed_{false};
  std::atomic<bool> tx_now_{false};  // set from other tasks to request an immediate LLDPDU

  uint32_t next_tx_ms_{0};
  uint8_t fast_remaining_{0};
  uint32_t tx_ok_{0};
  uint32_t tx_err_{0};
  uint32_t rx_ok_{0};
  uint32_t rx_bad_{0};

  LLDPNeighbor neighbor_;
  bool has_neighbor_{false};
  uint32_t neighbor_expires_ms_{0};

#ifdef USE_TEXT_SENSOR
  struct FieldSensor {
    NeighborField field;
    text_sensor::TextSensor *sensor;
  };
  std::vector<FieldSensor> text_sensors_;
#endif
#ifdef USE_SENSOR
  sensor::Sensor *vlan_id_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *present_sensor_{nullptr};
#endif

  Trigger<const LLDPNeighbor &> neighbor_trigger_;
  Trigger<> neighbor_lost_trigger_;
};

}  // namespace esphome::lldp
