/* The word wrap, measured rather than admired.
 *
 * This unit decides where text breaks on every flowing surface in the desktop - the terminal's scrollback,
 * the `graphics' report a user pastes, a notification's body - and it decides two other things that matter
 * more than the breaks: how many rows a string needs (which is what a box sizes itself by) and whether the
 * walk terminates.  A row count one too small clips the last line of a diagnostic silently; a loop that
 * stops making progress hangs the compositor with the mouse wheel over it.  Both are invisible on a machine
 * whose text happens to fit, which is most machines, most of the time.
 *
 * Every case below is a shape the desktop actually produces, and the big one is the losslessness property:
 * for generated paragraphs at every width from 1 to 40 columns, the concatenation of the rows must contain
 * every non-space character of the input, in order.  Truncation was the bug being fixed, so a test that only
 * checked "it wraps somewhere" would pass on the code that started this.
 */
#include <stdio.h>
#include <string.h>

#include "textwrap.h"

static int checks, failures;

static void check(int ok, const char *what)
{
    checks++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Rows of `text' as separate strings, so a case can say exactly what each row holds. */
static int rows_of(const char *text, int cols, char (*out)[256])
{
    const char *p = text;
    int n = 0;
    while (p && n < 16) {
        p = s_wrap_row(p, cols, out[n], 256);
        n++;
    }
    return n;
}

static void fed_spaces(const char *text, int cols, char *out, size_t cap)
{
    const char *p = text;
    char row[256];
    size_t n = 0;
    out[0] = 0;
    while (p) {
        p = s_wrap_row(p, cols, row, sizeof row);
        for (int i = 0; row[i] && n + 1 < cap; i++) out[n++] = row[i];
    }
    out[n < cap ? n : cap - 1] = 0;
}

static void keep_only_letters(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *c = in; *c; c++) {
        if (*c == ' ' || *c == '\t' || *c == '\n') continue;
        if (n + 1 < cap) out[n++] = *c;
    }
    out[n] = 0;
}

int main(void)
{
    char rows[16][256], joined[8192], filtered[8192], expected[8192];

    /* ---- the cell arithmetic every caller depends on. ---- */
    check(s_text_cols(0) == 1 && s_text_cols(7) == 1, "a width narrower than one glyph still holds a row");
    check(s_text_cols(FONT_W) == 1 && s_text_cols(FONT_W * 10) == 10,
          "a row is exactly as many characters as the fixed cell allows");
    check(s_text_cols(FONT_W * 10 + 7) == 10, "a part cell is not a character: no rounding up");

    /* ---- nothing, and one row. ---- */
    check(s_wrap_rows("", 40) == 0, "an empty string needs no rows at all");
    check(s_wrap_rows("hello", 40) == 1, "a line that fits is one row");
    check(rows_of("hello", 40, (char(*)[256])rows) == 1 && !strcmp(rows[0], "hello"),
          "and it is drawn whole");

    /* ---- where a row may break. ---- */
    check(s_wrap_rows("hello world", 5) == 2, "one word per row when two will not fit");
    rows_of("hello world", 5, rows);
    check(!strcmp(rows[0], "hello") && !strcmp(rows[1], "world"),
          "the break happens at the space, and the space is not drawn at either end");
    rows_of("a  b", 80, rows);
    check(s_wrap_rows("a  b", 80) == 1 && !strcmp(rows[0], "a  b"),
          "spaces inside a row survive; only a break consumes them");
    check(s_wrap_rows("exact12345", 10) == 1, "a string exactly one row wide does not spill");
    rows_of("twelve123456 x", 12, rows);
    check(s_wrap_rows("twelve123456 x", 12) == 2 && !strcmp(rows[0], "twelve123456"),
          "a row that ends where a word ends is full - the next word starts the next row, whole");
    rows_of("twelve1234567 x", 12, rows);
    check(s_wrap_rows("twelve1234567 x", 12) == 2 && !strcmp(rows[0], "twelve123456") &&
          !strcmp(rows[1], "7 x"),
          "one character more splits the oversized word, and its tail takes the next word with it: a row "
          "is filled, not started afresh for every break");

    /* ---- a word longer than a row: split, never drop. ---- */
    {
        char longword[80];
        for (int i = 0; i < 60; i++) longword[i] = (char)('a' + i % 26);
        longword[60] = 0;
        check(s_wrap_rows(longword, 37) == 2, "a word of 60 at 37 columns is two rows, not one and a half");
        int n = rows_of(longword, 37, rows);
        check(n == 2 && (int)strlen(rows[0]) == 37 && (int)strlen(rows[1]) == 23,
              "the first row is full to the last column and the rest follows on the second");
        fed_spaces(longword, 37, joined, sizeof joined);
        check(!strcmp(joined, longword), "and no character of the long word is lost");
    }

    /* ---- blank lines and newlines inside a string. ---- */
    check(s_wrap_rows("a\n\nb", 80) == 3, "a blank line inside a report is a row of its own");
    rows_of("a\n\nb", 80, rows);
    check(!strcmp(rows[0], "a") && rows[1][0] == 0 && !strcmp(rows[2], "b"),
          "and it is drawn as nothing, between the lines it separates");
    check(s_wrap_rows("one\n", 80) == 1, "a trailing newline does not add a row of its own");
    check(s_wrap_rows("\n", 80) == 1, "a lone newline is one blank row");
    check(s_wrap_rows("line one\nline two", 80) == 2, "two short lines stay two rows");
    check(s_wrap_rows("a\nbbbbbbbbbbbb\nc", 5) == 5,
          "each line wraps on its own: 12 characters with no space in them are three rows at five columns");

    /* ---- the invariant that matters on a real report: no lost text. ---- */
    {
        static const char *samples[] = {
            "GPU acceleration: driver module nvidia (family nvidia) read this chip's own registers and "
            "reports what it found; it offers no engine operation; the CPU compositor still paints every "
            "pixel of this screen",
            "short",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa middle "
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "  leading and trailing   spaces   everywhere  ",
            "tab\there and\t\tthere",
            "one\n\ntwo\n\n\nthree",
            "the quick brown fox jumps over the lazy dog and keeps going well past any reasonable row",
            0 };
        int all_ok = 1, rows_ok = 1;
        for (int i = 0; samples[i]; i++) {
            for (int cols = 1; cols <= 40; cols++) {
                int n = s_wrap_rows(samples[i], cols);
                fed_spaces(samples[i], cols, joined, sizeof joined);
                keep_only_letters(samples[i], filtered, sizeof filtered);
                keep_only_letters(joined, expected, sizeof expected);
                if (strcmp(filtered, expected)) all_ok = 0;
                const char *p = samples[i];
                char row[256];
                int counted = 0;
                while (p && counted <= n) { p = s_wrap_row(p, cols, row, sizeof row); counted++; }
                if (counted != n || (n > 0 && p != 0)) rows_ok = 0;
                /* A row never exceeds the width it was told about. */
                p = samples[i];
                while (p) {
                    p = s_wrap_row(p, cols, row, sizeof row);
                    if ((int)strlen(row) > cols) all_ok = 0;
                }
            }
        }
        check(all_ok, "at every width from 1 to 40 columns, the rows hold exactly the input's characters");
        check(rows_ok, "and s_wrap_rows is the number of rows the walk actually produces, no more and no less");
    }

    /* ---- termination: the property a painter relies on when it loops until the text is gone. ---- */
    {
        char many[512];
        for (int i = 0; i < 400; i++) many[i] = (char)(i % 3 ? 'x' : ' ');
        many[400] = 0;
        const char *p = many;
        char row[256];
        int steps = 0;
        while (p && steps < 5000) { p = s_wrap_row(p, 1, row, sizeof row); steps++; }
        check(p == 0 && steps > 0, "a one-column row still makes progress, so no caller can spin");
    }

    printf("%s: text layout, %d check(s), %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
