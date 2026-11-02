cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_CellCom_app)

target_sources(app PRIVATE
    src/client_id.c
    src/ESP32_temperature.c
    src/main.c
    src/mqtt_client.c
    src/mqtt_payload.c
    src/wifi_network.c
)
