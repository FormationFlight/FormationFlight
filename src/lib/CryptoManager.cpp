#include "CryptoManager.h"
#include "main.h"

CryptoManager::CryptoManager()
{
    // Use SHAKE256 as a key derivation function to determine a key from the groupkey input
    SHAKE256 kdf;
    kdf.reset();
    // Intentionally don't copy the null byte at the end
    char strBuf[strlen(GROUPKEY)];
    memcpy(strBuf, GROUPKEY, strlen(GROUPKEY));
    kdf.update(strBuf, strlen(GROUPKEY));
    // Generate a key from our input
    uint8_t key[KEY_LENGTH_BYTES];
    kdf.extend(key, KEY_LENGTH_BYTES);
    /* Here, we use the XTS mode; this isn't really secure *at all* given we're using a static tweak.
    // The goal of this is to keep disparate groups from seeing each other and to prevent passive snooping, not to prevent coordinated attacks
    // Coordinated attack prevention would necessitate a stream cipher which requires keeping & sending counters and nonces
    // which just aren't worth it for this project.
    */
    cipher = new XTS<AES128>();
    cipher->setKey(key, KEY_LENGTH_BYTES);
    cipher->setSectorSize(sizeof(air_type0_t));
    uint8_t tweak[TWEAK_LENGTH_BYTES];
    memset(tweak, 0, TWEAK_LENGTH_BYTES);
    cipher->setTweak(tweak, TWEAK_LENGTH_BYTES);
}

void CryptoManager::encrypt(uint8_t *buf, size_t length)
{
    cipher->encryptSector(buf, buf);
}

void CryptoManager::decrypt(uint8_t *buf, size_t length)
{
    cipher->decryptSector(buf, buf);
}

CryptoManager *cryptoManager = nullptr;

CryptoManager* CryptoManager::getSingleton()
{
    if (cryptoManager == nullptr)
    {
        cryptoManager = new CryptoManager();
    }
    return cryptoManager;
}