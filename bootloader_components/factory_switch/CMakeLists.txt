idf_component_register(SRCS "factory_switch.c")

# Force the linker to pull our object file from the static library so the
# strong bootloader_after_init overrides the weak default in the ESP-IDF
# bootloader.  Without this the linker satisfies the weak symbol first and
# never extracts our object, so the hook silently never runs.
target_link_options(${COMPONENT_LIB} INTERFACE -Wl,-u,bootloader_after_init)
