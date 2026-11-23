#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The "update" command of TikPrint Cloud: the owner pressed Update on the website, and the printer
 * is told "install version X" (docs/device-api.md in the TikPrint Cloud repo, "The update command").
 *
 * AN UPDATE NAMES A VERSION AND NOTHING ELSE. There is never an address in it, and none is looked
 * for: the file comes from this firmware's own GitHub repository, through the same code as
 * "Check for updates" on the printer's page (fw_github.c). It is only ever a newer version.
 *
 * This file decides what to do with a command and keeps track of how the install is going. It makes
 * no network calls itself: the Realtime task (cloud_realtime.c) asks it what there is to report and
 * writes that to the printer's row on the cloud.
 *
 * The command's id and version are saved on the board (NVS namespace "cloud", key "update"), so
 * that after the restart the printer can say "done" (it runs that version) or "rolled_back" (it
 * does not), and so that a command seen twice is carried out once.
 */

#define CLOUD_UPDATE_STATUS_MAX 16    // room for "rolled_back", "downloading", ...
#define CLOUD_UPDATE_ERROR_MAX  104   // room for why it failed (the cloud takes up to 200 characters)

/** Load what was saved. Call once at startup, before fw_github_start(). */
void cloud_update_init(void);

/** An update command was heard on the socket or found at a check-in. Any task may call this. A
 *  command that was seen before, or that arrives while another is installing, is ignored. */
void cloud_update_command(const char *id, const char *version);

/** Look after the command in hand: start it, and after a restart see how it ended. The Realtime
 *  task calls this about once a second. */
void cloud_update_tick(void);

/**
 * Is there a step the cloud has not been told about? If so: its name ("downloading", "installing",
 * "rebooting", "done", "failed", "rolled_back"), why it failed ("" if it did not), and a number to
 * hand back to cloud_update_reported() once the cloud has it.
 */
bool cloud_update_next_report(char status[CLOUD_UPDATE_STATUS_MAX], char error[CLOUD_UPDATE_ERROR_MAX], uint32_t *report);

/** The cloud has been told about that step. */
void cloud_update_reported(uint32_t report);

/** Forget everything (the printer is no longer linked). An install that is under way carries on;
 *  nobody is told how it ends. */
void cloud_update_clear(void);
