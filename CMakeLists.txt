set(component_srcs "src/WS2812FX.c" "components/led_strip/src/led_strip_rmt_ws2812.c")

set(include_dirs "include" "components/led_strip/include")

idf_component_register(SRCS "${component_srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       PRIV_REQUIRES ""
                       PRIV_INCLUDE_DIRS "components/led_strip/include"
                       REQUIRES "")

