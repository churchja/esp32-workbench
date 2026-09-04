# REQUIRES / PRIV_REQUIRES MUST NOT depend on CONFIG_* symbols.
#
# ESP-IDF expands component requirements BEFORE sdkconfig is loaded
# (tools/cmake/build.cmake: __component_get_requirements() runs before
# __build_import_configs()), and build-system.rst states it explicitly:
# "The values of REQUIRES and PRIV_REQUIRES should not depend on any
# configuration options (CONFIG_xxx macros)."
#
# So a tempting `if(CONFIG_SPIRAM) list(APPEND ... esp_psram) endif()` is
# ALWAYS false on every board, and the PSRAM probe it guards silently never
# works -- failing precisely where it was supposed to help.
#
# Not needed anyway: heap_caps_get_total_size() lives in `heap`, which is
# always linked. PSRAM is detected without requiring esp_psram at all.
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash esp_app_format
                       INCLUDE_DIRS "")
