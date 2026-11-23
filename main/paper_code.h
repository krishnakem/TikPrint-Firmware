#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * A code that proves someone is standing at the printer.
 *
 * The printer's page is open to everyone on the same Wi-Fi, so a request alone says nothing about
 * who sent it. For the things only the owner may do, the board prints a ticket with a short code
 * and waits for that code to be typed back: only someone who can read the paper can go on.
 *
 * The code is 6 random digits. It is good for a short time, it works once, and after a few wrong
 * guesses it is cancelled, so it cannot be found by trying them all.
 *
 * This file only keeps and checks the code. It has no lock, no clock and no printer of its own
 * (the caller gives the time and the random numbers), so it can be tried on a computer:
 * tools/host_test/run.sh code.
 */

#define PAPER_CODE_DIGITS  6
#define PAPER_CODE_LIFE_S  120   // how long a code is good for
#define PAPER_CODE_TRIES   5     // wrong guesses before the code is cancelled

typedef struct {
    char code[PAPER_CODE_DIGITS + 1];   // "" = there is no code
    int64_t until_us;                   // when it runs out (board uptime)
    int tries_left;
} paper_code_t;

typedef enum {
    PAPER_CODE_RIGHT,       // the code was right. It is used up: it will not work again
    PAPER_CODE_WRONG,       // wrong, and there are guesses left
    PAPER_CODE_CANCELLED,   // wrong once too often: the code is gone, a new one must be printed
    PAPER_CODE_NONE,        // there is no code to check against: none was printed, or it ran out
} paper_code_result_t;

/** Make a new code, in place of the one before. random32 gives 32 random bits a call (esp_random). */
void paper_code_new(paper_code_t *pc, uint32_t (*random32)(void), int64_t now_us);

/** Throw the code away. */
void paper_code_clear(paper_code_t *pc);

/** Seconds the code is still good for; 0 if there is none. */
int paper_code_seconds_left(const paper_code_t *pc, int64_t now_us);

/** Is this what a code looks like: exactly PAPER_CODE_DIGITS digits? Anything else is not checked
 *  at all (and does not cost a guess). */
bool paper_code_looks_right(const char *typed);

/** Check what was typed. It takes the same time whether the first digit is wrong or the last, so
 *  the time an answer takes says nothing about the code. *tries_left = guesses left afterwards. */
paper_code_result_t paper_code_check(paper_code_t *pc, const char *typed, int64_t now_us, int *tries_left);
