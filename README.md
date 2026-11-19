# TikPrint Firmware

Firmware for the Waveshare ESP32-S3 board (32 MB flash, 16 MB PSRAM).
Plug the board into a computer with USB-C and it appears as a small network.
The web UI is at **http://tikprint.local** (or **http://192.168.7.1** if the name does not resolve).

Status: **Milestone 4** (Print works end to end with a fake printer that writes tickets to the log; Wi-Fi setup and the `tikprint.local` name work over USB and Wi-Fi).

## The one USB-C port is two devices

The board has a small USB hub inside, so one cable gives the Mac two things:

| Device | What it is for |
| --- | --- |
| `/dev/cu.usbmodem5B901739161` (USB-serial chip) | Flashing and logs |
| "TikPrint" network adapter (the ESP32-S3's own USB) | The web UI at tikprint.local / 192.168.7.1 |

## Build, flash, and watch logs

Open a terminal in this folder, then:

```sh
# 1. Load the ESP-IDF tools into this terminal (once per terminal window)
. ~/.espressif/tools/activate_idf_v6.1.1.sh

# 2. Build
idf.py build

# 3. Flash and watch the logs (quit the log view with Ctrl+])
idf.py -p /dev/cu.usbmodem5B901739161 flash monitor
```

In VS Code you can use the ESP-IDF extension's buttons instead: pick the port
`/dev/cu.usbmodem5B901739161`, flash method **UART**, then Build, Flash, Monitor.

## Test

1. After flashing, wait about 5 seconds.
2. System Settings → Network should list a new service called **TikPrint**
   with the address 192.168.7.2.
3. Open http://tikprint.local in a browser. You should see the TikPrint UI.
   http://192.168.7.1 shows the same page.
4. Your normal internet should still work, because the board gives the Mac no
   router and no DNS server.

In the log you should see lines like:

```
usb_net: USB attached: computer connected
usb_dhcp: computer got 192.168.7.2 (...)
web: GET /
```

## Set up Wi-Fi

1. Open http://tikprint.local over USB and click **Set up Wi-Fi** in the menu bar.
2. Pick your network, type the password, and click **Join**.
   Enterprise networks (MWireless, eduroam) are greyed out: they are not supported.
   An iPhone hotspot needs "Maximize Compatibility" switched on (2.4 GHz).
3. On success the popover shows the network name, `tikprint.local`, and the board's Wi-Fi IP.
4. Any device on that same network can now open http://tikprint.local.
   The board remembers the network and rejoins it after a reboot.

## API quick reference

```sh
curl http://tikprint.local/api/status
curl http://tikprint.local/api/wifi/scan
curl -d '{"ssid":"NAME","password":"PASSWORD"}' http://tikprint.local/api/wifi/connect
curl -X POST http://tikprint.local/api/wifi/forget
curl http://tikprint.local/api/last-ticket          # the last ticket the printer received
open http://tikprint.local/api/last-ticket.pbm      # its image, exactly as the printer got it
curl "http://tikprint.local/api/diag/ping?ip=1.1.1.1"   # make the board ping something (troubleshooting)
```

## Printing (fake printer for now)

The real thermal printer has not arrived, so tickets go to a **fake printer**:
it writes the title, body, and an ASCII-art picture of any image to the log
(`idf.py -p /dev/cu.usbmodem5B901739161 monitor`) and remembers the last ticket.

- Files are prepared in the browser: blank margins are trimmed, the content is
  scaled to 576 dots wide and turned into pure black and white dots (a clean
  cut for text and line art, dithering for photos). The ticket on screen shows
  exactly those dots, and exactly those dots are sent. The board never decodes
  JPG, PNG or PDF.
- PDFs are drawn in the browser by pdf.js 4.10.38 (Apache-2.0), kept in
  `web/vendor/` and embedded in the firmware so it works with no internet.
  Pages are stacked into one strip. One print holds at most 4096 rows (about
  51 cm), so a longer file is split into parts: the ticket shows one part with
  ‹ › buttons to flip through, and each press of Print prints the part shown
  and brings up the next. Under the preview the ticket shows how much paper
  the part and the whole file will use (8 dots = 1 mm).
- The numbers that control the look (trim, contrast, sharpening, brightness)
  are named constants at the top of the "Upload area" code in `web/index.html`.
- The ticket number is stored on the board and survives reboots.

To add the real printer later: write `main/printer_thermal.c` that fills in a
`printer_driver_t` (see `main/printer.h`), then change one line in `main/printer.c`.

## How the code is laid out

| File | Job |
| --- | --- |
| `main/main.c` | Starts everything in order |
| `main/usb_net.c` | Makes the built-in USB a network adapter at 192.168.7.1 |
| `main/usb_dhcp.c` | Tiny DHCP server that gives the computer 192.168.7.2 and nothing else |
| `main/net_name.c` | mDNS: the `tikprint.local` name |
| `main/wifi_mgr.c` | Wi-Fi: scan, join, forget, reconnect; credentials saved on the board (NVS) |
| `main/api.c` | The JSON API the UI calls (`/api/...`) |
| `main/counter.c` | Ticket counter saved on the board (NVS) |
| `main/printer.h`, `main/printer.c` | The printer interface the rest of the code uses |
| `main/printer_fake.c` | Fake printer: logs tickets and keeps the last one in PSRAM |
| `main/web_server.c` | HTTP server on port 80; serves the embedded UI |
| `web/index.html` | The UI. Gzipped and embedded into the firmware at build time |
| `web/vendor/` | pdf.js (library + worker), embedded the same way. To update: replace both files and change `PDFJS` in `web/index.html` |
| `tools/gzip_file.py` | Build helper that gzips the UI |
| `sdkconfig.defaults` | Board settings (flash, PSRAM, UART logs, USB mode) |
| `partitions.csv` | How the flash is divided up |

## Notes

- To change the UI, edit `web/index.html`, then build and flash again.
- To change the name, edit `NET_HOSTNAME` in `main/net_name.h`.
- `sdkconfig` is generated from `sdkconfig.defaults`. If you change the
  defaults, delete `sdkconfig` and rebuild.
- Never put Wi-Fi passwords or tokens in this repo. Wi-Fi credentials will be
  stored on the board itself (NVS), not in the code.
