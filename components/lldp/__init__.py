"""LLDP (IEEE 802.1AB) transmit and receive for Ethernet devices."""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_DISABLED, CONF_ID, CONF_MDNS
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@davidcoulson"]
DEPENDENCIES = ["ethernet"]

CONF_LLDP_ID = "lldp_id"
CONF_MANAGEMENT_ADDRESS = "management_address"
CONF_ON_NEIGHBOR = "on_neighbor"
CONF_ON_NEIGHBOR_LOST = "on_neighbor_lost"
CONF_PORT_DESCRIPTION = "port_description"
CONF_PORT_ID = "port_id"
CONF_PROMISCUOUS = "promiscuous"
CONF_RECEIVE = "receive"
CONF_SYSTEM_DESCRIPTION = "system_description"
CONF_SYSTEM_NAME = "system_name"
CONF_TRANSMIT = "transmit"
CONF_TX_FAST_COUNT = "tx_fast_count"
CONF_TX_HOLD = "tx_hold"
CONF_TX_INTERVAL = "tx_interval"

lldp_ns = cg.esphome_ns.namespace("lldp")
LLDPComponent = lldp_ns.class_("LLDPComponent", cg.Component)
LLDPNeighbor = lldp_ns.struct("LLDPNeighbor")
LLDPNeighborConstRef = LLDPNeighbor.operator("ref").operator("const")

# 128 keeps the worst-case LLDPDU inside the fixed transmit buffer.
LLDP_STRING = cv.All(cv.string_strict, cv.Length(min=1, max=128))

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LLDPComponent),
            cv.Optional(CONF_TRANSMIT, default=True): cv.boolean,
            cv.Optional(CONF_RECEIVE, default=True): cv.boolean,
            cv.Optional(CONF_PROMISCUOUS, default=False): cv.boolean,
            cv.Optional(CONF_PORT_ID, default="eth0"): LLDP_STRING,
            cv.Optional(CONF_PORT_DESCRIPTION, default="Ethernet"): LLDP_STRING,
            cv.Optional(CONF_SYSTEM_NAME): LLDP_STRING,
            cv.Optional(CONF_SYSTEM_DESCRIPTION): LLDP_STRING,
            cv.Optional(CONF_MANAGEMENT_ADDRESS, default=True): cv.boolean,
            cv.Optional(CONF_MDNS, default=False): cv.boolean,
            cv.Optional(CONF_TX_INTERVAL, default="30s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(hours=1)),
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


def _final_validate(config: ConfigType) -> None:
    if not config[CONF_MDNS]:
        return
    full = fv.full_config.get()
    mdns_conf = full.get("mdns")
    if mdns_conf is None or mdns_conf.get(CONF_DISABLED):
        raise cv.Invalid("'mdns: true' needs the mdns component enabled", [CONF_MDNS])
    if "api" not in full:
        raise cv.Invalid(
            "'mdns: true' publishes on the native API's _esphomelib service; add 'api:'",
            [CONF_MDNS],
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_transmit(config[CONF_TRANSMIT]))
    cg.add(var.set_receive(config[CONF_RECEIVE]))
    cg.add(var.set_promiscuous(config[CONF_PROMISCUOUS]))
    cg.add(var.set_port_id(config[CONF_PORT_ID]))
    cg.add(var.set_port_description(config[CONF_PORT_DESCRIPTION]))
    if (name := config.get(CONF_SYSTEM_NAME)) is not None:
        cg.add(var.set_system_name(name))
    if (description := config.get(CONF_SYSTEM_DESCRIPTION)) is not None:
        cg.add(var.set_system_description(description))
    cg.add(var.set_management_address(config[CONF_MANAGEMENT_ADDRESS]))
    cg.add(var.set_tx_interval(config[CONF_TX_INTERVAL].total_seconds))
    cg.add(var.set_tx_hold(config[CONF_TX_HOLD]))
    cg.add(var.set_tx_fast_count(config[CONF_TX_FAST_COUNT]))
    if config[CONF_MDNS]:
        cg.add_define("USE_LLDP_MDNS")

    if (conf := config.get(CONF_ON_NEIGHBOR)) is not None:
        await automation.build_callback_automation(
            var, "add_on_neighbor_callback", [(LLDPNeighborConstRef, "x")], conf
        )
    if (conf := config.get(CONF_ON_NEIGHBOR_LOST)) is not None:
        await automation.build_callback_automation(
            var, "add_on_neighbor_lost_callback", [], conf
        )
