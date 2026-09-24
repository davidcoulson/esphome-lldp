import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_LLDP_ID, LLDPComponent

DEPENDENCIES = ["lldp"]

CONF_NEIGHBOR_PRESENT = "neighbor_present"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LLDP_ID): cv.use_id(LLDPComponent),
        cv.Optional(CONF_NEIGHBOR_PRESENT): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_LLDP_ID])
    if conf := config.get(CONF_NEIGHBOR_PRESENT):
        sens = await binary_sensor.new_binary_sensor(conf)
        cg.add(parent.set_neighbor_present_sensor(sens))
