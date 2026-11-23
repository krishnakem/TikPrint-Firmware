/*
 * Checks the code that proves someone is at the printer (main/paper_code.c) on a computer
 * (see README.txt): what "Reset / Unlink" on the printer's page waits for before it unlinks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paper_code.h"

#define S 1000000LL   // one second, in the board's microseconds

static int s_failed;
#define CHECK(what) do { if (!(what)) { printf("FAILED line %d: %s\n", __LINE__, #what); s_failed++; } } while (0)

/* Pretend random numbers: the ones in s_next, in turn, then whatever rand() gives. */
static uint32_t s_next[4];
static int s_next_n, s_next_at;
static uint32_t pretend_random(void)
{
    return s_next_at < s_next_n ? s_next[s_next_at++] : ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}
static void next_random(uint32_t a, uint32_t b)
{
    s_next[0] = a; s_next[1] = b; s_next_n = 2; s_next_at = 0;
}

/* A code that is surely not pc's: its first digit changed. */
static const char *wrong_for(const paper_code_t *pc)
{
    static char wrong[PAPER_CODE_DIGITS + 1];
    memcpy(wrong, pc->code, sizeof(wrong));
    wrong[0] = wrong[0] == '9' ? '0' : (char)(wrong[0] + 1);
    return wrong;
}

int main(void)
{
    paper_code_t pc;
    int left = -1;
    char right[PAPER_CODE_DIGITS + 1];

    // With no code printed, nothing passes: not an empty code, not six zeros.
    paper_code_clear(&pc);
    CHECK(paper_code_check(&pc, "", 0, &left) == PAPER_CODE_NONE);
    CHECK(paper_code_check(&pc, "000000", 0, &left) == PAPER_CODE_NONE);
    CHECK(paper_code_seconds_left(&pc, 0) == 0);

    // A code is 6 digits, zeros in front included.
    next_random(42, 0);
    paper_code_new(&pc, pretend_random, 10 * S);
    CHECK(strcmp(pc.code, "000042") == 0);
    CHECK(pc.tries_left == PAPER_CODE_TRIES);
    CHECK(paper_code_seconds_left(&pc, 10 * S) == PAPER_CODE_LIFE_S);
    // Random numbers too near the top would make some codes likelier than others: drawn again.
    next_random(4294967295u, 123456);
    paper_code_new(&pc, pretend_random, 0);
    CHECK(strcmp(pc.code, "123456") == 0);
    next_random(4293999999u, 0);   // the biggest one that is still fair
    paper_code_new(&pc, pretend_random, 0);
    CHECK(strcmp(pc.code, "999999") == 0);

    // What is not 6 digits is not a code.
    CHECK(paper_code_looks_right("482905"));
    CHECK(!paper_code_looks_right("48290") && !paper_code_looks_right("4829050") && !paper_code_looks_right(""));
    CHECK(!paper_code_looks_right("48290a") && !paper_code_looks_right("48 905") && !paper_code_looks_right(NULL));

    // The right code works, once.
    paper_code_new(&pc, pretend_random, 0);
    strcpy(right, pc.code);
    CHECK(paper_code_check(&pc, right, 5 * S, &left) == PAPER_CODE_RIGHT);
    CHECK(pc.code[0] == 0);
    CHECK(paper_code_check(&pc, right, 6 * S, &left) == PAPER_CODE_NONE);

    // A wrong code costs a guess; the right one still works afterwards.
    paper_code_new(&pc, pretend_random, 0);
    strcpy(right, pc.code);
    CHECK(paper_code_check(&pc, wrong_for(&pc), S, &left) == PAPER_CODE_WRONG && left == PAPER_CODE_TRIES - 1);
    CHECK(paper_code_check(&pc, "12345", S, &left) == PAPER_CODE_WRONG && left == PAPER_CODE_TRIES - 2);   // too short: wrong, never right
    CHECK(paper_code_check(&pc, right, S, &left) == PAPER_CODE_RIGHT);

    // A digit wrong anywhere is wrong.
    for (int i = 0; i < PAPER_CODE_DIGITS; i++) {
        char typed[PAPER_CODE_DIGITS + 1];
        paper_code_new(&pc, pretend_random, 0);
        strcpy(typed, pc.code);
        typed[i] = typed[i] == '9' ? '0' : (char)(typed[i] + 1);
        CHECK(paper_code_check(&pc, typed, S, &left) == PAPER_CODE_WRONG);
    }

    // The 5th wrong code cancels the code: after that not even the right one works.
    paper_code_new(&pc, pretend_random, 0);
    strcpy(right, pc.code);
    for (int i = 1; i < PAPER_CODE_TRIES; i++) {
        CHECK(paper_code_check(&pc, wrong_for(&pc), S, &left) == PAPER_CODE_WRONG && left == PAPER_CODE_TRIES - i);
    }
    CHECK(paper_code_check(&pc, wrong_for(&pc), S, &left) == PAPER_CODE_CANCELLED && left == 0);
    CHECK(paper_code_check(&pc, right, S, &left) == PAPER_CODE_NONE);

    // After 2 minutes the code is gone, right or not.
    paper_code_new(&pc, pretend_random, 100 * S);
    strcpy(right, pc.code);
    CHECK(paper_code_seconds_left(&pc, (100 + PAPER_CODE_LIFE_S) * S - 1) == 1);
    CHECK(paper_code_seconds_left(&pc, (100 + PAPER_CODE_LIFE_S) * S) == 0);
    CHECK(paper_code_check(&pc, right, (100 + PAPER_CODE_LIFE_S) * S, &left) == PAPER_CODE_NONE);
    paper_code_new(&pc, pretend_random, 100 * S);
    strcpy(right, pc.code);
    CHECK(paper_code_check(&pc, right, (100 + PAPER_CODE_LIFE_S) * S - 1, &left) == PAPER_CODE_RIGHT);

    // A new code takes the place of the old one, with all its guesses back.
    paper_code_new(&pc, pretend_random, 0);
    strcpy(right, pc.code);
    paper_code_check(&pc, wrong_for(&pc), S, &left);
    next_random(right[0] == '7' ? 800000 : 700000, 0);
    paper_code_new(&pc, pretend_random, 2 * S);
    CHECK(pc.tries_left == PAPER_CODE_TRIES);
    CHECK(paper_code_check(&pc, right, 3 * S, &left) == PAPER_CODE_WRONG);

    // Codes are spread over all six digits: in 20,000 of them every digit turns up in every place.
    int seen[PAPER_CODE_DIGITS][10] = { 0 };
    srand(1);
    for (int n = 0; n < 20000; n++) {
        paper_code_new(&pc, pretend_random, 0);
        CHECK(paper_code_looks_right(pc.code));
        for (int i = 0; i < PAPER_CODE_DIGITS; i++) {
            seen[i][pc.code[i] - '0']++;
        }
    }
    for (int i = 0; i < PAPER_CODE_DIGITS; i++) {
        for (int d = 0; d < 10; d++) {
            CHECK(seen[i][d] > 1500 && seen[i][d] < 2500);
        }
    }

    printf(s_failed ? "%d check(s) FAILED\n" : "the code rules hold\n", s_failed);
    return s_failed ? 1 : 0;
}
