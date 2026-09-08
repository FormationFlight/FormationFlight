#pragma once

// src/lib/MSP/MSP.h only ever stores a Stream* / takes a Stream& (never
// calls into it in an inline body) -- an empty stand-in is enough to make
// that header parse natively. Never instantiated: MSP.cpp/MSPManager.cpp
// aren't part of the native Follow test build.
class Stream {
};
