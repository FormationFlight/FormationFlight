#ifndef DISPLAY_H
#define DIPLAY_H
#include "main.h"

void display_off();
void display_on();
void display_init();
void display_draw_status(system_t *sys);
void display_draw_intro();
void display_draw_startup();
void display_draw_scan(system_t *sys);
void display_draw_progressbar(int progress);
void display_draw_peername(int position);

#endif