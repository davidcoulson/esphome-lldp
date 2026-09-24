"""LLDP (IEEE 802.1AB) transmitter + receiver for ESPHome Ethernet devices."""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

DEPENDENCIES = ["ethernet"]
MULTI_CONF = False

CONF_LLDP_ID = "lldp_id"
CONF_INTERFACE = "interface"
CONF_TRANSMIT = "transmit"
CONF_RECEIVE = "receive"
CONF_PROMISCUOUS = "promiscuous"
CONF_PORT_ID = "port_id"
CONF_PORT_DESCRIPTION = "port_description"
CONF_SYSTEM_NAME = "system_name"
CONF_SYSTEM_DESCRIPTION = "system_description"
CONF_TX_INTERVAL = "tx_interval"
CONF_TX_HOLD = "tx_hold"
CONF_TX_FAST_COUNT = "tx_fast_count"
CONF_MANAGEMENT_ADDRESS = "management_address"
CONF_ON_NEIGHBOR = "on_neighbor"
CONF_ON_NEIGHBOR_LOST = "on_neighbor_lost"

lldp_ns = cg.esphome_ns.namespace("lldp")
LLDPComponent = lldp_ns.class_("LLDPComponent", cg.Component)
LLDPNeighbor = lldp_ns.struct("LLDPNeighbor")

LLDP_STRING = cv.All(cv.string_strict, cv.Length(min=1, max=255))

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LLDPComponent),
            # esp_netif if_key of the Ethernet interface. ESPHome's ethernet
            # component uses the IDF default.
            cv.Optional(CONF_INTERFACE, default="ETH_DEF"): cv.string_strict,
            cv.Optional(CONF_TRANSMIT, default=True): cv.boolean,
            cv.Optional(CONF_RECEIVE, default=True): cv.boolean,
            # Needed for chips that can't accept the LLDP multicast MAC via a
            # filter (W5500 and friends).
            cv.Optional(CONF_PROMISCUOUS, default=False): cv.boolean,
            cv.Optional(CONF_PORT_ID, default="eth0"): LLDP_STRING,
            cv.Optional(CONF_PORT_DESCRIPTION, default="Ethernet"): LLDP_STRING,
            cv.Optional(CONF_SYSTEM_NAME): LLDP_STRING,
            cv.Optional(CONF_SYSTEM_DESCRIPTION): LLDP_STRING,
            cv.Optional(CONF_MANAGEMENT_ADDRESS, default=True): cv.boolean,
            cv.Optional(CONF_TX_INTERVAL, default="30s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(
                    min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(seconds=3600)
                ),
            ),
            cv.Optional(CONF_TX_HOLD, default=4): cv.int_range(min=1, max=100),
            cv.Optional(CONF_TX_FAST_COUNT, default=4): cv.int_range(min=0, max=8),
            cv.Optional(CONF_ON_NEIGHBOR): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_NEIGHBOR_LOST): automation.validate_automation(
                single=True
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # No L2 TAP on purpose: components that timestamp in the Ethernet driver
    # (pps_ntp) turn that off when CONFIG_ESP_NETIF_L2_TAP is set. See lldp_component.cpp.

    cg.add(var.set_interface(config[CONF_INTERFACE]))
    cg.add(var.set_transmit(config[CONF_TRANSMIT]))
    cg.add(var.set_receive(config[CONF_RECEIVE]))
    cg.add(var.set_promiscuous(config[CONF_PROMISCUOUS]))
    cg.add(var.set_port_id(config[CONF_PORT_ID]))
    cg.add(var.set_port_description(config[CONF_PORT_DESCRIPTION]))
    # Default to the hostname; that's what switches show in their neighbor tables.
    cg.add(var.set_system_name(config.get(CONF_SYSTEM_NAME, CORE.name)))
    if CONF_SYSTEM_DESCRIPTION in config:
        cg.add(var.set_system_description(config[CONF_SYSTEM_DESCRIPTION]))
    cg.add(var.set_management_address(config[CONF_MANAGEMENT_ADDRESS]))
    cg.add(var.set_tx_interval(config[CONF_TX_INTERVAL].total_seconds))
    cg.add(var.set_tx_hold(config[CONF_TX_HOLD]))
    cg.add(var.set_tx_fast_count(config[CONF_TX_FAST_COUNT]))

    if conf := config.get(CONF_ON_NEIGHBOR):
        await automation.build_automation(
            var.get_neighbor_trigger(),
            [(LLDPNeighbor.operator("ref").operator("const"), "x")],
            conf,
        )
    if conf := config.get(CONF_ON_NEIGHBOR_LOST):
        await automation.build_automation(var.get_neighbor_lost_trigger(), [], conf)
