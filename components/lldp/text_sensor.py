import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import (
    CONF_LLDP_ID,
    CONF_MANAGEMENT_ADDRESS,
    CONF_PORT_DESCRIPTION,
    CONF_PORT_ID,
    CONF_SYSTEM_DESCRIPTION,
    CONF_SYSTEM_NAME,
    LLDPComponent,
    lldp_ns,
)

DEPENDENCIES = ["lldp"]

NeighborField = lldp_ns.enum("NeighborField")

CONF_CHASSIS_ID = "chassis_id"
CONF_CAPABILITIES = "capabilities"
CONF_SOURCE_MAC = "source_mac"

FIELDS = {
    CONF_SYSTEM_NAME: (NeighborField.FIELD_SYSTEM_NAME, "mdi:switch"),
    CONF_SYSTEM_DESCRIPTION: (NeighborField.FIELD_SYSTEM_DESCRIPTION, "mdi:information-outline"),
    CONF_CHASSIS_ID: (NeighborField.FIELD_CHASSIS_ID, "mdi:identifier"),
    CONF_PORT_ID: (NeighborField.FIELD_PORT_ID, "mdi:ethernet"),
    CONF_PORT_DESCRIPTION: (NeighborField.FIELD_PORT_DESCRIPTION, "mdi:ethernet"),
    CONF_MANAGEMENT_ADDRESS: (NeighborField.FIELD_MANAGEMENT_ADDRESS, "mdi:ip-network"),
    CONF_CAPABILITIES: (NeighborField.FIELD_CAPABILITIES, "mdi:format-list-bulleted"),
    CONF_SOURCE_MAC: (NeighborField.FIELD_SOURCE_MAC, "mdi:network"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LLDP_ID): cv.use_id(LLDPComponent),
        **{
            cv.Optional(key): text_sensor.text_sensor_schema(
                icon=icon, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            )
            for key, (_, icon) in FIELDS.items()
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LLDP_ID])
    for key, (field, _) in FIELDS.items():
        if conf := config.get(key):
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(parent.add_text_sensor(field, sens))
