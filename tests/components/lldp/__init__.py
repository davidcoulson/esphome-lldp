from tests.testing_helpers import ComponentManifestOverride


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # The unit tests only cover the platform-independent LLDPDU codec. The
    # ethernet dependency has no host build, and lldp_component.cpp compiles to
    # nothing without USE_ESP32/USE_ETHERNET.
    manifest.dependencies = []
