Trying board code on a computer, with no board.

    tools/host_test/run.sh rules                      check the free-plan rules (one connector, one switch a day, ...)
    tools/host_test/run.sh code                       check the code that "Reset / Unlink" waits for (6 digits, 2 minutes,
                                                      5 guesses, works once): main/paper_code.c
    tools/host_test/run.sh ticket todoist /full/path/reply.json /full/path/ticket.png
                                                      draw the ticket for an app's saved answer

It compiles the same files the firmware uses (main/connectors.c, conn_ticket.c, conn_<app>.c,
ticket_draw.c) for this computer. The small files in shims/ stand in for the parts of the board
they lean on (saved settings, the lock, the clock).

To get an answer to draw, ask the app yourself, e.g. for Todoist:
    curl -s -H "Authorization: Bearer $TOKEN" \
      "https://api.todoist.com/api/v1/tasks/filter?query=today%20%7C%20overdue&limit=50" > reply.json
Keep tokens and replies out of the repo.
