#ifndef CONFIGHANDLER_H
#define CONFIGHANDLER_H
#include "main.h"
#include "MSP.h"


void handleConfigMessage(Stream& input_source, String message);
void config_save(config_t *cfg);
void config_init(bool forcedefault = 0);
void config_clear();

#endif