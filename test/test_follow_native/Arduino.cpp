#include "Arduino.h"

SerialShim Serial;

static unsigned long native_millis_value = 0;

unsigned long millis()
{
    return native_millis_value;
}

void native_millis_set(unsigned long ms)
{
    native_millis_value = ms;
}

void native_millis_advance(unsigned long ms)
{
    native_millis_value += ms;
}
