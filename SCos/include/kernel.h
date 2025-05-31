
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Kernel symbols from linker script
extern uint32_t _kernel_start;
extern uint32_t _kernel_end;

// Kernel functions
void kernel_panic(const char* message);
void call_constructors();
bool init_subsystems();

// Application functions
void runTerminal();
void openNotepad(const char* content);
void openCalendar();
void openSettings();
void openAbout();
void openFileManager();

#ifdef __cplusplus
}
#endif

#endif
