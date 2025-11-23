#pragma once
#include <MFRC522.h>
#include "result.h"

#define RST_PIN 14 // Reset pin
#define SS_PIN 10 // Slave Select pin
#define SCK_PIN 12 // Serial Clock pin
#define MOSI_PIN 11 // Master Out Slave In pin
#define MISO_PIN 13 // Master In Slave Out pin

#ifndef DEBUG_PRINT
    #define DEBUG_PRINT(x) do { if(Serial) Serial.println(x); } while(0)
    #define ERROR_PRINT(x) do { if(Serial) Serial.println(x); } while(0)
    #define INFO_PRINT(x) do { if(Serial) Serial.println(x); } while(0)
#endif

class CardReader {
public:
    struct CardSecret {
        byte secret[32];
    };
  
    struct Uid {
        byte size;
        byte uidByte[10];
        byte sak;
    };

    CardReader();
    Result begin(); // Initialize the reader
    Result read(Uid &iUid, bool &isUltralightC, CardSecret &iSecret);
    bool isCardPresent();  // ✅ New method to check for card presence
    void endCard();


private:
    Result getUid(Uid &iUid, bool &isUltralightC);
    Result authenticateUltralightC();
    Result readCardSecret(CardSecret &iSecret);
    //void endCard();

private:
    MFRC522 mMFRC;
    bool mInitialized;
    byte mSecretKey[4] = { 0xFF, 0xFF, 0xFF, 0xFF }; // Default password
};