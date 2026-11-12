idf_component_register(SRCS "app_main.cpp" "epd.cpp" "ui.cpp"
                       INCLUDE_DIRS ".")

# Flash the per-device factory partition together with the app (`idf.py flash`)
set(FCTRY_BIN "${PROJECT_DIR}/factory/out/fctry.bin")
if(EXISTS ${FCTRY_BIN})
    esptool_py_flash_to_partition(flash "fctry" "${FCTRY_BIN}")
else()
    message(WARNING "factory/out/fctry.bin not found: run factory/gen_factory.sh, the device can't be paired without it")
endif()
