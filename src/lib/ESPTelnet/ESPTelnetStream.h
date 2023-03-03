/* ------------------------------------------------- */

#pragma once
#ifndef ESPTelnetStream_h
#define ESPTelnetStream_h

/* ------------------------------------------------- */

#include "ESPTelnetBase.h"

/* ------------------------------------------------- */

class ESPTelnetStream : public ESPTelnetBase, public Stream {
  public:
    using ESPTelnetBase::ESPTelnetBase;

    int available();
    int read();
    int peek();
    void flush();

    size_t write(uint8_t);

  protected:
    void handleInput();
};

/* ------------------------------------------------- */

  // << operator
//  template<class T> inline ESPTelnetStream &operator <<(ESPTelnetStream &obj, T arg) { obj.print(arg); return obj; } 

/* ------------------------------------------------- */
#endif
/* ------------------------------------------------- */