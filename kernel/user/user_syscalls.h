/* Shared syscall/event numbers for the CPL3 app server.  Kept in sync with
 * kernel/include/usermode.h by tools/tests/test_usermode.py. */
#ifndef SCOS_USER_SYSCALLS_H
#define SCOS_USER_SYSCALLS_H
#define SC_NR_EXIT        1
#define SC_NR_LOG         2
#define SC_NR_CLOCK       3
#define SC_NR_DATE        4
#define SC_NR_SCREEN      5
#define SC_NR_MOUSE       6
#define SC_NR_APP_ID      7
#define SC_NR_WINDOW      8
#define SC_NR_FOCUSED     9
#define SC_NR_MEMORY      10
#define SC_NR_INTERVAL    11
#define SC_NR_TEXT_WIDTH  12
#define SC_NR_DRAW        13
#define SC_NR_REDRAW      14
#define SC_NR_TITLE       15
#define SC_NR_RESIZE      16
#define SC_NR_MOVE        17
#define SC_NR_MINIMIZE    18
#define SC_NR_NOTIFY      19
#define SC_NR_MESSAGE     20
#define SC_NR_THEME_GET   21
#define SC_NR_THEME_LIST  22
#define SC_NR_THEME_APPLY 23
#define SC_NR_THEME_STATUS 24
#define SC_NR_FILE_EXISTS 25
#define SC_NR_FILE_SIZE   26
#define SC_NR_FILE_READ   27
#define SC_NR_FILE_WRITE  28
#define SC_NR_FILE_REMOVE 29
#define SC_NR_FILE_RENAME 30
#define SC_NR_FILE_LIST   31
#define SC_NR_INFO        32
#define SC_NR_SET_ICON    33
#define SC_NR_FAULT       34
#define SC_NR_MAX         34
/* File-syscall refusals that are not "no such file" and not a capability
 * denial.  The kernel-side host raises the same sentence for each, so a
 * package gets one answer whichever host runs it. */
#define SC_FILE_QUOTA_REFUSED (-3)   /* 16 files / 64 KiB per app id */
#define SC_DRAW_CLEAR     1
#define SC_DRAW_RECT      2
#define SC_DRAW_TEXT      3
#define SC_DRAW_PIXEL     4
#define SC_DRAW_LINE      5
#define SC_DRAW_FRAME     6
#define SC_DRAW_CIRCLE    7
#define SC_DRAW_DISC      8
#define SC_DRAW_GRADIENT  9
#define SC_DRAW_ICON      10
#define SC_DRAW_TEXT_SCALED 11
#define SC_DRAW_TEXT_CLIP 12
#define SC_DRAW_TEXT_WRAP 13
#define SC_DRAW_BUTTON    14
#define SC_DRAW_CHECKBOX  15
#define SC_DRAW_PROGRESS  16
#define USER_EVENT_OPEN   1
#define USER_EVENT_PAINT  2
#define USER_EVENT_KEY    3
#define USER_EVENT_MOUSE  4
#define USER_EVENT_TICK   5
#define USER_EVENT_CLOSE  6
#endif
