#pragma once

#include <Arduino.h>
#include <Crypto.h>
#include <AES.h>
#include <XTS.h>
#include <SHAKE.h>

#ifndef GROUPKEY
#define GROUPKEY "opensesame"
#endif
#define KEY_LENGTH_BYTES 64
#define TWEAK_LENGTH_BYTES 16

class CryptoManager {
public:
    CryptoManager();
    void encrypt(uint8_t *buf, size_t length);
    void decrypt(uint8_t *buf, size_t length);
    static CryptoManager* getSingleton();
    bool getEnabled();
    void setEnabled(bool enabled);
private:
    XTSCommon *cipher;
    bool enabled = true;
};