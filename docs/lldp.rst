LLDP Component
==============

.. seo::
    :description: Instructions for setting up LLDP (IEEE 802.1AB) on Ethernet-connected ESP32 devices.
    :image: ethernet.svg
    :keywords: LLDP, 802.1AB, switch, neighbor, Ethernet

The ``lldp`` component lets an Ethernet-connected ESP32 speak the Link Layer Discovery
Protocol (IEEE 802.1AB).

- **Transmit:** the device announces itself, so a managed switch shows its name, port and IP
  addresses in its LLDP neighbor table (``show lldp neighbors`` on Cisco, the Devices page on
  UniFi, and so on).
- **Receive:** the device learns which switch and port it is plugged into and can expose that
  as :doc:`text sensors </components/text_sensor/index>`, a
  :doc:`sensor </components/sensor/index>` and a
  :doc:`binary sensor </components/binary_sensor/index>`, and add it to the device's mDNS
  record.

This component requires the :doc:`ethernet` component. It is only available on ESP32 with the
ESP-IDF framework.

.. code-block:: yaml

    # Example configuration entry
    ethernet:
      # ...

    lldp:

    text_sensor:
      - platform: lldp
        system_name:
          name: Switch
        port_id:
          name: Switch Port

    binary_sensor:
      - platform: lldp
        neighbor_present:
          name: LLDP Neighbor

Configuration variables:
------------------------

- **transmit** (*Optional*, boolean): Send LLDP frames. Defaults to ``true``.
- **receive** (*Optional*, boolean): Listen for LLDP frames from the switch. Defaults to
  ``true``.
- **promiscuous** (*Optional*, boolean): Put the Ethernet MAC into promiscuous mode so it
  receives LLDP frames. Needed for SPI Ethernet chips such as the W5500, which cannot filter for
  the LLDP multicast address. See :ref:`lldp-receive`. Defaults to ``false``.
- **port_id** (*Optional*, string): The Port ID advertised to the switch. Defaults to ``eth0``.
- **port_description** (*Optional*, string): The Port Description advertised to the switch.
  Defaults to ``Ethernet``.
- **system_name** (*Optional*, string): The System Name advertised to the switch. Defaults to
  the device name.
- **system_description** (*Optional*, string): The System Description advertised to the switch.
  Defaults to the ``esphome.project`` name and version when configured, otherwise the ESPHome
  version and board.
- **management_address** (*Optional*, boolean): Advertise the interface's IPv4 and IPv6
  addresses as Management Address TLVs. Defaults to ``true``.
- **mdns** (*Optional*, boolean): Add the received neighbor to the device's mDNS record. See
  :ref:`lldp-mdns`. Requires the :doc:`mdns` and :doc:`api` components. Defaults to ``false``.
- **tx_interval** (*Optional*, :ref:`config-time`): How often to send an LLDP frame. Between
  ``5s`` and ``1h``. Defaults to ``30s``.
- **tx_hold** (*Optional*, int): Hold multiplier. The advertised time-to-live is
  ``tx_interval × tx_hold + 1`` seconds. Between 1 and 100. Defaults to ``4``.
- **tx_fast_count** (*Optional*, int): Number of frames sent at one-second intervals right after
  the link comes up, before dropping back to ``tx_interval``. Between 0 and 8. Defaults to ``4``.
- **on_neighbor** (*Optional*, :ref:`Automation <automation>`): An automation to perform when a
  neighbor appears or its advertised data changes. See :ref:`lldp-on_neighbor`.
- **on_neighbor_lost** (*Optional*, :ref:`Automation <automation>`): An automation to perform
  when the neighbor's time-to-live expires, the link goes down, or the neighbor announces it is
  shutting down.
- **id** (*Optional*, :ref:`config-id`): Manually specify the ID for code generation.

Text Sensor
-----------

The ``lldp`` text sensor platform exposes fields of the received neighbor. Every field is
optional; each takes all options from :ref:`Text Sensor <config-text_sensor>`. The values are
cleared when the neighbor is lost.

.. code-block:: yaml

    text_sensor:
      - platform: lldp
        system_name:
          name: Switch
        port_id:
          name: Switch Port
        port_description:
          name: Switch Port Description
        chassis_id:
          name: Switch Chassis ID
        management_address:
          name: Switch Address
        system_description:
          name: Switch Description
        capabilities:
          name: Switch Capabilities
        source_mac:
          name: Switch Source MAC

Configuration variables:

- **system_name** (*Optional*): The neighbor's System Name, usually the switch hostname.
- **port_id** (*Optional*): The neighbor's Port ID, for example ``Te1/0/10`` or ``Port 7``.
- **port_description** (*Optional*): The neighbor's Port Description.
- **chassis_id** (*Optional*): The neighbor's Chassis ID, usually its MAC address.
- **management_address** (*Optional*): The neighbor's management address. IPv4 is preferred
  when several are advertised.
- **system_description** (*Optional*): The neighbor's System Description, usually the switch
  model and firmware.
- **capabilities** (*Optional*): The neighbor's enabled capabilities, for example
  ``bridge, router``.
- **source_mac** (*Optional*): The MAC address the frame was sent from.

Sensor
------

The ``lldp`` sensor platform exposes the VLAN the port is in.

.. code-block:: yaml

    sensor:
      - platform: lldp
        vlan_id:
          name: Switch Port VLAN

Configuration variables:

- **vlan_id** (*Optional*): The 802.1 Port VLAN ID advertised by the switch. Unknown when the
  switch does not advertise it. All options from :ref:`Sensor <config-sensor>`.

Binary Sensor
-------------

The ``lldp`` binary sensor platform reports whether a neighbor is currently known.

.. code-block:: yaml

    binary_sensor:
      - platform: lldp
        neighbor_present:
          name: LLDP Neighbor

Configuration variables:

- **neighbor_present** (*Optional*): ``on`` while a neighbor is known. All options from
  :ref:`Binary Sensor <config-binary_sensor>`.

.. _lldp-on_neighbor:

``on_neighbor`` Trigger
-----------------------

This automation runs when a neighbor appears or its advertised data changes. The neighbor is
available as ``x``, with the fields ``system_name``, ``port_id``, ``port_description``,
``chassis_id``, ``management_address``, ``system_description``, ``capabilities``, ``source_mac``
(all C strings), ``vlan_id`` (``-1`` when not advertised) and ``ttl``.

.. code-block:: yaml

    lldp:
      on_neighbor:
        - logger.log:
            format: "Plugged into %s port %s"
            args: [x.system_name, x.port_id]
      on_neighbor_lost:
        - logger.log: "Neighbor gone"

In a lambda, ``id(my_lldp).has_neighbor()`` and ``id(my_lldp).get_neighbor()`` give the same
information at any time, and ``id(my_lldp).send_now()`` sends an LLDP frame immediately.

.. _lldp-mdns:

mDNS
----

With ``mdns: true``, the neighbor is added to the ``_esphomelib._tcp`` service that Home
Assistant and the ESPHome dashboard already browse:

======================== ===============================================
TXT key                  Value
======================== ===============================================
``lldp_switch``          The neighbor's System Name
``lldp_port``            The neighbor's Port ID
``lldp_vlan``            The Port VLAN ID, when advertised
======================== ===============================================

The keys are updated when the neighbor changes and removed when it is lost. You can check them
with ``dns-sd -L <device name> _esphomelib._tcp`` (macOS) or
``avahi-browse -rt _esphomelib._tcp`` (Linux).

.. _lldp-receive:

Receiving LLDP
--------------

LLDP frames are sent to the multicast address ``01:80:C2:00:00:0E``. The ESP32's internal
Ethernet MAC (used with RMII PHYs such as the LAN8720, IP101 or RTL8201) accepts that address
through a MAC filter entry which this component adds automatically.

SPI Ethernet chips such as the W5500 have no such filter, so the driver drops LLDP frames
unless the chip is in promiscuous mode. Set ``promiscuous: true`` on those devices. The chip then
passes every frame on the port to the ESP32, which costs a little CPU on busy networks; sending
LLDP does not need it, so ``receive: false`` is an alternative.

The component tracks one neighbor, the switch port the device is attached to. If several LLDP
speakers can reach the device (for example through an unmanaged switch in between), the sensors
show whichever spoke last.

See Also
--------

- :doc:`ethernet`
- :doc:`ethernet_info`
- :doc:`mdns`
- :ghedit:`Edit`
