/* A code that proves someone is standing at the printer (see paper_code.h). */
#include <string.h>

#include "paper_code.h"

void paper_code_clear(paper_code_t *pc)
{
    memset(pc, 0, sizeof(*pc));
}

void paper_code_new(paper_code_t *pc, uint32_t (*random32)(void), int64_t now_us)
{
    // Every code from 000000 to 999999 must be as likely as any other. 2^32 is not a multiple of
    // 1,000,000, so the few numbers at the very top are thrown away and drawn again.
    const uint32_t codes = 1000000, fair_below = UINT32_MAX - UINT32_MAX % codes;
    uint32_t r;
    do {
        r = random32();
    } while (r >= fair_below);
    r %= codes;
    for (int i = PAPER_CODE_DIGITS - 1; i >= 0; i--) {
        pc->code[i] = (char)('0' + r % 10);
        r /= 10;
    }
    pc->code[PAPER_CODE_DIGITS] = 0;
    pc->until_us = now_us + (int64_t)PAPER_CODE_LIFE_S * 1000000;
    pc->tries_left = PAPER_CODE_TRIES;
}

int paper_code_seconds_left(const paper_code_t *pc, int64_t now_us)
{
    if (!pc->code[0] || now_us >= pc->until_us) {
        return 0;
    }
    return (int)((pc->until_us - now_us + 999999) / 1000000);
}

bool paper_code_looks_right(const char *typed)
{
    if (!typed || strlen(typed) != PAPER_CODE_DIGITS) {
        return false;
    }
    for (int i = 0; i < PAPER_CODE_DIGITS; i++) {
        if (typed[i] < '0' || typed[i] > '9') {
            return false;
        }
    }
    return true;
}

paper_code_result_t paper_code_check(paper_code_t *pc, const char *typed, int64_t now_us, int *tries_left)
{
    *tries_left = 0;
    if (!pc->code[0] || now_us >= pc->until_us || pc->tries_left <= 0) {
        paper_code_clear(pc);
        return PAPER_CODE_NONE;
    }
    // Every digit is looked at, right or wrong: no stopping at the first one that differs.
    // Something that is not 6 digits is checked as six zero bytes, which no code is made of: a wrong guess.
    char guess[PAPER_CODE_DIGITS] = { 0 };
    if (paper_code_looks_right(typed)) {
        memcpy(guess, typed, PAPER_CODE_DIGITS);
    }
    unsigned char differ = 0;
    for (int i = 0; i < PAPER_CODE_DIGITS; i++) {
        differ |= (unsigned char)(pc->code[i] ^ guess[i]);
    }
    if (differ == 0) {
        paper_code_clear(pc);   // it works once
        return PAPER_CODE_RIGHT;
    }
    if (--pc->tries_left <= 0) {
        paper_code_clear(pc);
        return PAPER_CODE_CANCELLED;
    }
    *tries_left = pc->tries_left;
    return PAPER_CODE_WRONG;
}
