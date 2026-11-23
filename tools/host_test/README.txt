Trying board code on a computer, with no board.

    tools/host_test/run.sh code                       check the code that "Reset / Unlink" waits for (6 digits, 2 minutes,
                                                      5 guesses, works once): main/paper_code.c
    tools/host_test/run.sh qr [folder]                draw QR tickets with main/qr_ticket.c (every size, a caption, a code under
                                                      a picture) and check its limits; the pictures land in the folder as PNGs
    tools/host_test/run.sh ticket todoist /full/path/reply.json /full/path/ticket.png
                                                      draw the ticket for an app's saved answer

"code" compiles the firmware's own file for this computer. The small files in shims/ stand in for
the parts of the board it leans on (the log, the clock, ...).

"ticket" is different. Connectors (Todoist, ...) run in TikPrint Cloud now, not on the printer, so
their code is not in the firmware any more. The files that drew a connector's ticket are kept in
reference/ (conn_ticket.c, conn_todoist.c and their headers). They are host-only: nothing in
reference/ is ever compiled into the firmware. They are kept because the cloud draws these tickets
with its own copy of this code, and its test (tests/render-drift.test.ts in TikPrint Cloud) runs
"run.sh ticket todoist ..." and compares the two, dot for dot. Do not change what reference/ draws
without changing the cloud to match. The letters come from main/ticket_draw.c and
data/ticket_fonts.bin, which the firmware still uses for weather tickets.

To get an answer to draw, ask the app yourself, e.g. for Todoist:
    curl -s -H "Authorization: Bearer $TOKEN" \
      "https://api.todoist.com/api/v1/tasks/filter?query=today%20%7C%20overdue&limit=50" > reply.json
Keep tokens and replies out of the repo.
