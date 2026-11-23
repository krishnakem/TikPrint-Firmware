#pragma once

/*
 * Things about the product as a whole that you may want to change in one place.
 */

/* Who to contact about this app. The National Weather Service asks every app that uses its
 * free forecast service to say who it is, so they can get in touch if it misbehaves. It is
 * sent with every weather request as "TikPrint/<firmware version> (<this text>)".
 * See main/weather.c.
 *
 * It must be a real address: the weather service refuses requests (403 Access Denied) whose
 * contact is a made-up one such as something@example.com. */
#define APP_CONTACT "https://github.com/krishnakem/TikPrint-Firmware"

/* Where TikPrint Cloud lives (the site the "TikPrint Cloud" button opens, and the one a linked printer
 * belongs to). No "/" at the end. The page gets it from the board (/api/cloud/status), so this
 * is the only place it is written down. See main/cloud_state.c. */
#define CLOUD_URL "https://tikprint-cloud.vercel.app"

/* For testing only: 1 adds POST /api/cloud/debug-link, which saves a made-up cloud link (or
 * erases the link) so both looks of the TikPrint Cloud button can be tried without the cloud.
 * Keep it 0 in anything that is pushed: builds for real printers must not have it. */
#define CLOUD_DEBUG_FAKE_LINK 0
