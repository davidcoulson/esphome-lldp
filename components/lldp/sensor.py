import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_LLDP_ID, LLDPComponent

DEPENDENCIES = ["lldp"]

CONF_VLAN_ID = "vlan_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LLDP_ID): cv.use_id(LLDPComponent),
        cv.Optional(CONF_VLAN_ID): sensor.sensor_schema(
            icon="mdi:lan",
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LLDP_ID])
    if conf := config.get(CONF_VLAN_ID):
        sens = await sensor.new_sensor(conf)
        cg.add(parent.set_vlan_id_sensor(sens))
