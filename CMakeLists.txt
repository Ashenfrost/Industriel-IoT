cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_CellCom_app)

target_sources(app PRIVATE
    src/client_id.c
    src/ESP32_temperature.c
    src/main.c
    src/mqtt_client.c
    src/mqtt_payload.c
    src/sparkplug.c
    src/time_sync.c
    src/wifi_network.c
)

# Sparkplug B payload: generate C structs from the .proto with nanopb.
# proto/sparkplug_b.options next to it limits array/string sizes.
# The generated header is included as "proto/sparkplug_b.pb.h".
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app proto/sparkplug_b.proto)
