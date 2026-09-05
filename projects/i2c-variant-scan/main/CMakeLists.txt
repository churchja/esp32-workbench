# REQUIRES must not depend on CONFIG_* -- see templates/idf-base/main/CMakeLists.txt
# for why (requirements expand before sdkconfig loads).
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_driver_i2c
                       INCLUDE_DIRS "")
