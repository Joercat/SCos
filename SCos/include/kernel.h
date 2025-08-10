
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


extern uint32_t _kernel_start;
extern uint32_t _kernel_end;


void kernel_panic(const char* message);
void call_constructors();
bool init_subsystems();


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
