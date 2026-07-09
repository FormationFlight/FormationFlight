#ifndef CONFIGHANDLER_H
#define CONFIGHANDLER_H
#include "main.h"


void handleConfigMessage(Stream& input_source, String message);
void config_save();
void config_init(bool forcedefault = 0);
void config_clear();

#endif