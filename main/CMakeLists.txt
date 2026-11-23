idf_component_register(
    SRCS "main.c" "usb_net.c" "usb_dhcp.c" "web_server.c" "net_name.c"
         "wifi_mgr.c" "counter.c" "clock.c" "settings.c" "api.c"
         "fw_update.c" "fw_github.c"
         "printer.c" "print_queue.c" "printer_fake.c" "printer_thermal.c"
         "zip_table.c" "city_table.c" "weather.c" "weather_ticket.c" "weather_icons.c" "automations.c"
         "ticket_draw.c" "ticket_header.c" "weather_render.c"
         "connectors.c" "conn_http.c" "conn_ticket.c" "conn_todoist.c"
         "cloud_state.c" "cloud_identity.c" "cloud_http.c" "cloud_ticket.c" "cloud_jobs.c" "cloud.c" "paper_code.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_netif esp_event nvs_flash esp_http_server lwip esp_psram
                  esp_wifi esp_timer esp_app_format esp_driver_uart esp_driver_gpio
                  app_update esp_partition bootloader_support
                  esp_http_client esp_https_ota mbedtls
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

# --- Fonts the page draws ticket text with. Embedded as they are (woff2 is already compressed). ---
# To add one: put the file in web/fonts, add its name here, in FONT_FILES in web_server.c,
# and in FONTS in web/index.html. ("Receipt" in FONTS is not a file of its own: it is the printer's
# built-in font, shown on screen with jetbrains-mono as a look-alike.)
foreach(f "inter" "source-serif" "jetbrains-mono" "caveat" "nunito")
    target_add_binary_data(${COMPONENT_LIB} "${web_dir}/fonts/${f}.woff2" BINARY)
endforeach()

# --- ZIP code -> latitude/longitude table for the weather (see main/zip_table.c). ---
# Made by tools/make_zip_table.py from the US Census ZCTA gazetteer (public domain).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../data/zip_table.bin" BINARY)

# --- Weather symbols (sun, cloud, rain, ...) printed on weather tickets (see main/weather_icons.c). ---
# Drawn by tools/make_weather_icons.py.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../data/weather_icons.bin" BINARY)

# --- Town name -> latitude/longitude table, for adding a weather place by name (see main/city_table.c). ---
# Made by tools/make_city_table.py from the US Census places gazetteer (public domain).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../data/city_table.bin" BINARY)

# --- The letters tickets are drawn with when the board draws them itself (see main/ticket_draw.c). ---
# Made by tools/make_ticket_fonts.py from the fonts in web/fonts. Run it again after adding a font.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../data/ticket_fonts.bin" BINARY)
