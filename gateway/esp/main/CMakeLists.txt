set(GATEWAY ${CMAKE_CURRENT_LIST_DIR}/../..)
set(FW ${GATEWAY}/../firmware/src)

idf_component_register(
    SRCS "main.c"
         # The protocol core and application, as the Mac tests build them.
         "${GATEWAY}/core/coap.c" "${GATEWAY}/core/lowpan.c" "${GATEWAY}/core/reasm.c"
         "${GATEWAY}/core/tado.c" "${GATEWAY}/core/session.c" "${GATEWAY}/core/hlc.c"
         "${GATEWAY}/app/gwlink.c" "${GATEWAY}/app/gateway.c" "${GATEWAY}/app/ha.c"
         # The host-link framing, shared with the CC1312 firmware. The gateway
         # opens no frames itself yet, so ccm and mac stay out.
         "${FW}/crc.c" "${FW}/hlframe.c"
    INCLUDE_DIRS "${GATEWAY}/core" "${GATEWAY}/app" "${FW}"
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event esp_timer nvs_flash
)
