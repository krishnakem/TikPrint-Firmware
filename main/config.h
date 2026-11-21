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
