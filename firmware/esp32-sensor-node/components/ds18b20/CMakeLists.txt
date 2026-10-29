# espressif/onewire_bus comes from idf_component.yml (added as a dependency by
# the IDF component manager).
idf_component_register(SRCS "ds18b20.c" "ds18b20_convert.c"
                       INCLUDE_DIRS "include")
