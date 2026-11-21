# TikPrint Firmware

Firmware for the Waveshare ESP32-S3 board (32 MB flash, 16 MB PSRAM).
Plug the board into a computer with USB-C and it appears as a small network.
The web UI is at **http://tikprint.local** (or **http://192.168.7.1** if the name does not resolve).

Status: **Milestone 5** (tickets print on the real thermal printer. Wi-Fi setup and the `tikprint.local` name work over USB and Wi-Fi. The board updates itself from GitHub releases: Settings > Check for updates).

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

## Firmware updates

The board gets new firmware from the **releases of this GitHub repository**.

1. Open http://tikprint.local, click the gear, then **Check for updates**.
2. If there is a newer release the row becomes **Update to v0.3.0**. Click it, then **Install**.
3. The board downloads the firmware from GitHub itself (about a minute), restarts, and the page reloads.

- The board also checks by itself once a day. A green dot on the gear means an update is waiting.
  Nothing is ever installed without a click.
- It needs Wi-Fi with internet. Over the USB cable alone the board cannot reach GitHub.
- Over Wi-Fi this is the **only** way in: the board accepts nothing but a release from this
  repository, fetched over HTTPS with GitHub's certificate checked. So whoever can publish a
  release here decides what the printers run, and nobody else on the network can.
- It only moves to a newer version, never back.
- An update is refused while tickets are printing or waiting.

### Publishing a release

1. Change the version number in `CMakeLists.txt`: `set(PROJECT_VER "0.3.0")`.
2. Commit and push to `main`.

That is all. GitHub (`.github/workflows/release.yml`) builds the firmware and publishes a
release called `v0.3.0` with `tikprint_firmware.bin` attached. It takes a few minutes; watch it
under the repository's **Actions** tab. A push that does not change the version number publishes nothing.

### How an update works (`main/fw_update.c`, `main/fw_github.c`, `partitions.csv`)

- The flash has three firmware slots. The cable writes `factory`. An update goes into
  whichever of `ota_0` / `ota_1` is not running, is checked, and the board restarts into it.
- **Safety net:** new firmware is on trial for 15 seconds. If it crashes or restarts before
  that, the board goes back to the firmware it had. The page then says "Update undone".
- **The cable always wins.** `idf.py flash` puts the board back on the `factory` slot, whatever
  was installed before. Use it if an update ever leaves the board unreachable.
- Settings shows what is running, e.g. `v0.2.0 · a1b2c3d`. The second part is different for every build.
- Logs still need the cable (`idf.py monitor`).

### Trying a build that is not a release

Over the **USB cable** a firmware file can be sent straight from the computer, without flashing:
open http://192.168.7.1, click the gear, then **Install from file** (the row only shows over USB), or:

```sh
curl --data-binary @build/tikprint_firmware.bin http://192.168.7.1/api/update
```

Over Wi-Fi this is refused.

## API quick reference

```sh
curl http://tikprint.local/api/status               # includes "print_queue": {"waiting": 2, "printing": true}
                                                    # and "time": {"set": true, "source": "browser", ...} (see Clock)
                                                    # and "printer": {"status": "ok", "last_checked_s_ago": 3} (see Printer status)
curl -d "{\"unix_ms\":$(date +%s)000,\"timezone\":\"America/Detroit\",\"posix_tz\":\"EST5EDT,M3.2.0,M11.1.0\"}" \
     http://tikprint.local/api/time                 # set the board's clock by hand (the page does this by itself)
curl http://tikprint.local/api/wifi/scan
curl -d '{"ssid":"NAME","password":"PASSWORD"}' http://tikprint.local/api/wifi/connect
curl -X POST http://tikprint.local/api/wifi/forget
curl http://tikprint.local/api/last-ticket          # the last ticket that started printing
open http://tikprint.local/api/last-ticket.pbm      # its image, exactly as the printer got it
curl "http://tikprint.local/api/diag/ping?ip=1.1.1.1"   # make the board ping something (troubleshooting)
curl http://tikprint.local/api/diag/printer-status            # ask the printer how it is: raw answer bytes + meaning (see Printer status)
curl http://tikprint.local/api/diag/printer-speed             # print a 4 cm test strip and time how fast light and dark rows print
curl "http://tikprint.local/api/diag/printer-test"            # print one test line ("busy" while tickets are printing or waiting)
curl "http://tikprint.local/api/diag/printer-test?baud=19200" # same, at another serial speed (until reboot)
                                                              # also: pin=N (board pin), invert=1 (flip the signal)
curl -X POST http://tikprint.local/api/update/check     # ask GitHub for the latest release (answer shows up in /api/status)
curl -X POST http://tikprint.local/api/update/install   # download it and restart into it (only if it is newer)
curl --data-binary @build/tikprint_firmware.bin http://192.168.7.1/api/update   # install from a file: USB cable only
```

`/api/status` also reports the firmware: `"boot_id": 123456789` (new after every restart) and
`"update": {"id": "a1b2c3d4e", "slot": "ota_0", "on_trial": false, "state": "idle", "latest": "0.3.0", "available": true, "progress": 0, "error": "", "checked_s_ago": 40, ...}`.
`state` is `idle`, `checking`, `downloading` or `restarting`.

## Clock

The board has no battery clock, so it forgets the time at every power-off (`main/clock.c`).
Two things set it again:

| Source | When | Needs |
| --- | --- | --- |
| Browser | The page opens, then every 5 minutes while it stays open | Nothing: works over USB with no Wi-Fi |
| Internet (SNTP, `pool.ntp.org`) | Wi-Fi gets an address, then every hour | Wi-Fi with internet |

- Internet time is trusted more. For 12 hours after an internet sync, browser time is ignored
  (and the log says so if the two differ by more than 5 seconds).
- The timezone comes from the browser and is saved on the board (NVS), so it survives a reboot.
  The time itself is never saved: after a reboot the clock is "not set" until a source sets it.
- The browser names the zone the IANA way (`America/Detroit`); the board needs a POSIX rule
  (`EST5EDT,M3.2.0,M11.1.0`). The page converts with a small table of common zones (`TZ_RULES`
  in `web/index.html`). A zone that is not in the table gets today's offset with no summer-time
  switch, which corrects itself the next time the page is open after the switch.
- `/api/status` reports the clock:
  `"time": {"set": true, "source": "browser", "now": "2026-11-20T22:05:31-05:00", "zone": "EST", "timezone": "America/Detroit", "last_sync_s_ago": 12}`.
  `source` is `browser`, `internet` or `none`. While not set, `now` and `last_sync_s_ago` are `null`.
- In the UI the board's time is in Settings (the gear), e.g. `22:05 · Detroit`.
- Date and time on the ticket header come from this clock (see Ticket header).
- The timings are named constants at the top of `main/clock.c` and of the "Clock" code in `web/index.html`.
- There is no scheduler yet: the clock is the groundwork for scheduled prints.

## Printing

Tickets go to the thermal printer over a serial wire, as ESC/POS commands
(`main/printer_thermal.c`).

The printer is an EM8026-CUT (80 mm, auto cutter). Use its **TTL** socket:

| Board | Printer TTL socket |
| --- | --- |
| IO17 | pin labelled `TX` (green wire): the board talks to the printer |
| IO18 | pin labelled `RX` (yellow wire): the printer answers. Optional, **3.3 V only** |
| GND | pin labelled `GND` (black wire) |

- The printer labels its pins from the board's point of view: `TX` is where the
  board's sending wire goes, `RX` is where the board listens.
- **Measure the yellow wire before connecting it.** With the printer on and idle, put a
  multimeter between the `RX` pin and `GND`. About 3.3 V is fine. About 5 V will damage the
  board: add a voltage divider first (for example 10 kΩ from the printer pin to IO18, and
  20 kΩ from IO18 to GND).
- The yellow wire is optional. Without it everything prints as before; the board just
  cannot tell how the printer is (see Printer status).
- The printer needs its own power supply. The board cannot power it.
- Do **not** use the board pins labelled TX/RX: those carry the logs.
- The pin, the serial speed (115200 baud, shown on the printer's self-test page) and whether the printer has a cutter
  are named constants at the top of `main/printer_thermal.c`.
- A ticket is sent in the background; a tall image takes a few seconds.
- Every ticket with an image ends in a blank footer about as tall as the top margin
  (`FOOTER_ROWS` at the top of `main/printer_thermal.c`).
- There is a print queue (`main/print_queue.c`). Pressing Print while a ticket is printing
  puts the new one in line, and the page says "Queued 0042, 2nd in line". Tickets print
  in the order they were sent.
- A ticket gets its number when it joins the queue, so numbers follow that order and never repeat.
- The queue holds up to 8 waiting tickets and 4 MB of image dots. Past that, Print answers
  "The print queue is full, try again in a moment". Both limits are named constants at the
  top of `main/print_queue.c`.
- Each waiting ticket is its own copy in PSRAM, freed when it has printed. The queue is
  not saved: tickets still waiting are lost if the board loses power.
- The test line (`/api/diag/printer-test`) answers "busy" while anything is printing or waiting.
  It no longer does the old "pin check" (the board listening to its own sending pin): the
  receive side now listens to the printer.
- The ticket is a small word processor. Select text, then use the toolbar above it:
  font, size (in points, true to size on paper), bold, italic, underline, and
  left / centre / right. One ticket can mix as many fonts and sizes as you like.
- The browser draws that text as dots, exactly as laid out on screen, and sends it as
  the top of the image. The printer's own receipt font is not used.
- The fonts (Inter, Source Serif, Nunito, Caveat, JetBrains Mono; all SIL Open Font
  License) are in `web/fonts/` and embedded in the firmware, so this works with no internet.
  Characters a font lacks (☐, emoji, other alphabets) come from the device's own fonts.
- The size list and how bold the dots come out (`TEXT_CUT`) are named constants at the top of the "Editor" code in `web/index.html`.
- To add a font: put a `.woff2` in `web/fonts/`, then add its name in three places:
  the list in `main/CMakeLists.txt`, `FONT_FILES` in `main/web_server.c`, and `FONTS` in `web/index.html`.
- A ticket sent without the browser (for example with `curl`) still prints in the printer's
  built-in font, which only knows plain characters (☐ becomes `[ ]`, unknown ones print as `?`).
- Every ticket is also written to the log with an ASCII-art picture of any image
  (`idf.py -p /dev/cu.usbmodem5B901739161 monitor`) when it starts printing, and the last
  one that started is remembered (`/api/last-ticket`).
- To run with no printer attached, point `s_driver` in `main/printer.c` at `printer_fake_driver`.
  The queue works the same way with it.

- A file gets onto the ticket by dropping it, by choosing it, or by pasting it (Cmd+V / Ctrl+V):
  a copied picture, a screenshot, a file copied in Finder, or copied SVG code. JPG, PNG, SVG and PDF work.
- A picture goes into the text where the cursor is, as a line of its own, like in a word processor:
  type above it, below it, or between two pictures. Its ✕ or Backspace from the line below removes it.
  A PDF, or a picture taller than 20 cm of paper (`INLINE_MAX_ROWS`), goes below the text instead.
- An SVG has no fixed size, so the page draws it at twice the paper's width before turning it into
  dots (`SVG_PX` in `web/index.html`). A small icon comes out as sharp as a large drawing.
- Files are prepared in the browser: blank margins are trimmed, the content is
  scaled to 576 dots wide and turned into pure black and white dots (a clean
  cut for text and line art, dithering for photos). The ticket on screen shows
  exactly those dots, and exactly those dots are sent. The board never decodes
  JPG, PNG, SVG or PDF.
- PDFs are drawn in the browser by pdf.js 4.10.38 (Apache-2.0), kept in
  `web/vendor/` and embedded in the firmware so it works with no internet.
  Pages are stacked into one strip. One print holds at most 4096 rows (about
  51 cm), so a longer file is split into parts: the ticket shows one part with
  ‹ › buttons to flip through, and each press of Print prints the part shown
  and brings up the next. Under the preview the ticket shows how much paper
  the part and the whole file will use (8 dots = 1 mm).
- The numbers that control the look (trim, contrast, sharpening, brightness)
  are named constants at the top of the "Upload area" code in `web/index.html`.
- The ticket number is stored on the board and survives reboots. It is used in the UI, the log
  and the API, but is not printed on the ticket.

## Ticket header

Every ticket starts with one line, on screen and on paper:

```
TikPrint | Los Angeles                 NOV 20, 2026 | 10:05 PM PST
```

- `TikPrint` is always there. Location, date and time each have a switch in Settings (the gear).
- Location is what you type in Settings, shown as typed. Left empty, it is the city of the timezone.
- To change the header's font: click the header on the ticket (it lights up), then pick a font in
  the toolbar. The other tools fade while the header is selected, since only the font applies to it.
  Click anywhere else to go back to the text.
- Date and time are the board's clock in the board's timezone, never the computer's. They are
  left out while the clock is not set. The page draws them when Print is pressed, so a ticket
  that waits in the queue shows when it was sent.
- The settings are saved on the board (`main/settings.c`, NVS), so every device sees the same.
  `/api/status` reports them as `"settings": {"location": "", "show_location": true, "show_date": true, "show_time": true, "header_font": "Inter"}`.
- To change them without the page (send only what you want to change):
  `curl -d '{"location":"Ann Arbor","show_time":false}' http://tikprint.local/api/settings`
- Tickets sent with `curl` get the same line in the printer's built-in font (which has only one font).
- Its size on paper is `HDR_SIZE` in `web/index.html`.

## Printer status

With the yellow wire connected, the board can ask the printer how it is
(ESC/POS "real-time status", `DLE EOT 1` to `4`, in `main/printer_thermal.c`).

| Status | Meaning | Tickets |
| --- | --- | --- |
| `ok` | All good | print |
| `paper_low` | Roll nearly finished | print |
| `out_of_paper` | No paper | wait in the queue |
| `cover_open` | Cover is open | wait in the queue |
| `offline` | The printer says it is offline, without a reason | print |
| `error` | Cutter jam or another fault | print |
| `unknown` | No answer: no yellow wire, or the printer is off | print |

- The board asks before every ticket, right after it, and otherwise every 10 seconds.
- Out of paper or cover open pauses the queue: waiting tickets are kept, the board asks again
  every 3 seconds, and printing carries on by itself once the problem is fixed.
  `/api/status` then shows `"print_queue": {"waiting": 2, "printing": false, "paused": true}`.
- No answer never stops a ticket. With no yellow wire the firmware behaves as it did before.
- `/api/status` reports `"printer": {"status": "ok", "last_checked_s_ago": 3}`.
- In the UI the menu bar has a "Printer" dot: green = ready, hollow = no answer, amber or red =
  a problem. While there is a problem, a quiet note under the Print button says what it is, what
  to do and how many tickets are waiting ("Out of paper · 2 tickets waiting"). Print still works
  then: the ticket joins the line. The wording is in `PRINTER_PROBLEMS` in `web/index.html`.
- A ticket that was already being sent when the paper ran out is not sent again.
- To check the wire, use the diagnostic endpoint (it answers "busy" while a ticket is being sent):

```sh
curl http://tikprint.local/api/diag/printer-status
```

```json
{"ok":true,"answered":4,"status":"ok",
 "queries":[{"query":"DLE EOT 1","about":"printer","raw":"0x16","valid":true},
            {"query":"DLE EOT 2","about":"offline cause","raw":"0x12","valid":true},
            {"query":"DLE EOT 3","about":"error cause","raw":"0x12","valid":true},
            {"query":"DLE EOT 4","about":"paper sensor","raw":"0x12","valid":true}],
 "decoded":{"online":true,"cover_open":false,"paper_out":false,"paper_low":false,
            "cutter_error":false,"other_error":false}}
```

- A healthy printer answers `0x12` or `0x16` to every question.
- `"raw": null` everywhere and `"status": "unknown"` means no byte came back: the wire is not
  connected, it is on the wrong printer pin, or the printer is off.
- `"valid": false` means a byte came back that cannot be a status byte: usually the wrong
  serial speed or a loose ground wire. Such bytes are ignored.
- The receive pin, the reply timeout and the meaning of each bit are named constants at the
  top of `main/printer_thermal.c`. How often the board asks is at the top of `main/print_queue.c`.

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
| `main/settings.c` | Settings saved on the board (NVS): what the ticket header shows |
| `main/clock.c` | The clock: time from the browser or the internet, timezone saved on the board (NVS) |
| `main/fw_update.c` | Installing new firmware: write to the spare slot, check, restart, go back if it fails |
| `main/fw_github.c` | Finds and downloads new firmware from this repository's GitHub releases |
| `.github/workflows/release.yml` | Builds and publishes a release when the version number changes |
| `main/printer.h`, `main/printer.c` | The printer interface the rest of the code uses |
| `main/print_queue.c` | The print queue: tickets wait in line and print one at a time |
| `main/printer_thermal.c` | Real printer: sends ESC/POS commands over a serial wire and reads its status back |
| `main/printer_fake.c` | Fake printer: logs tickets and keeps the last one in PSRAM |
| `main/web_server.c` | HTTP server on port 80; serves the embedded UI |
| `web/index.html` | The UI. Gzipped and embedded into the firmware at build time |
| `web/fonts/` | Fonts for ticket text, embedded as they are |
| `web/vendor/` | pdf.js (library + worker), embedded the same way. To update: replace both files and change `PDFJS` in `web/index.html` |
| `tools/gzip_file.py` | Build helper that gzips the UI |
| `sdkconfig.defaults` | Board settings (flash, PSRAM, UART logs, USB mode) |
| `partitions.csv` | How the flash is divided up (saved settings, three firmware slots) |

## Notes

- To change the UI, edit `web/index.html`, then build and flash again.
- To change the name, edit `NET_HOSTNAME` in `main/net_name.h`.
- `sdkconfig` is generated from `sdkconfig.defaults`. If you change the
  defaults, delete `sdkconfig` and rebuild.
- Never put Wi-Fi passwords or tokens in this repo. Wi-Fi credentials will be
  stored on the board itself (NVS), not in the code.
