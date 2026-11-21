idf_component_register(
    SRCS "main.c" "usb_net.c" "usb_dhcp.c" "web_server.c" "net_name.c"
         "wifi_mgr.c" "counter.c" "api.c"
         "printer.c" "printer_fake.c" "printer_thermal.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_netif esp_event nvs_flash esp_http_server lwip esp_psram
                  esp_wifi esp_timer esp_app_format esp_driver_uart
)

# --- Web UI: gzip each web file at build time and embed it in the firmware ---
# index.html is the page; the two vendor files are pdf.js, which draws PDFs in the browser.
set(web_dir "${CMAKE_CURRENT_SOURCE_DIR}/../web")
set(web_files "index.html" "vendor/pdf.min.mjs" "vendor/pdf.worker.min.mjs")
idf_build_get_property(python PYTHON)
set(web_gz_all "")
foreach(f ${web_files})
    get_filename_component(name "${f}" NAME)
    set(web_gz "${CMAKE_CURRENT_BINARY_DIR}/${name}.gz")
    add_custom_command(
        OUTPUT "${web_gz}"
        COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py" "${web_dir}/${f}" "${web_gz}"
        DEPENDS "${web_dir}/${f}"
        VERBATIM
    )
    list(APPEND web_gz_all "${web_gz}")
endforeach()
add_custom_target(web_ui_gz DEPENDS ${web_gz_all})
add_dependencies(${COMPONENT_LIB} web_ui_gz)
foreach(web_gz ${web_gz_all})
    target_add_binary_data(${COMPONENT_LIB} "${web_gz}" BINARY DEPENDS "${web_gz}")
endforeach()
