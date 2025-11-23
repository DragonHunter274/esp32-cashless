#include "cardreader.h"
#include <SPI.h>

CardReader::CardReader() : mMFRC(SS_PIN, RST_PIN), mInitialized(false) {}

Result CardReader::begin() {
    if (mInitialized) return Result::OK;

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW);
    delay(10);
    digitalWrite(RST_PIN, HIGH);
  
    mMFRC.PCD_Init(SS_PIN, RST_PIN);
    byte version = mMFRC.PCD_ReadRegister(MFRC522::VersionReg);

    if (version == 0x00 || version == 0xFF) {
        ERROR_PRINT("MFRC522 Communication failure");
        return Result::ERROR;
    }

    INFO_PRINT("RFID initialized successfully");
    mInitialized = true;
    return Result::OK;
}

Result CardReader::read(Uid &iUid, bool &isUltralightC, CardSecret &iSecret) {
    if (!mInitialized) {
        if (begin() != Result::OK) {
            ERROR_PRINT("❌ Could not initialize RFID module!");
            return Result::ERROR;
        }
    }

    // 🔹 Step 1: Ensure the card is still present before trying to read
    int retry_count = 3;
    while (!mMFRC.PICC_IsNewCardPresent() && retry_count-- > 0) {
        vTaskDelay(50 / portTICK_PERIOD_MS);  // ✅ Give time for a weak card signal to register
    }

    if (retry_count <= 0) {
        Serial.println("❌ No card detected after retries.");
        return Result::ERROR;
    }

    // 🔹 Step 2: Try reading the card serial number with retries
    retry_count = 3;
    while (!mMFRC.PICC_ReadCardSerial() && retry_count-- > 0) {
        Serial.println("🔄 Read failed. Retrying...");
        vTaskDelay(100 / portTICK_PERIOD_MS);  // ✅ Allow brief gap before retrying
    }

    if (retry_count <= 0) {
        Serial.println("❌ Read failed after multiple retries.");
        return Result::ERROR;
    }

    Serial.println("✅ Card serial number read!");

    // Store UID
    memcpy(iUid.uidByte, mMFRC.uid.uidByte, mMFRC.uid.size);
    iUid.size = mMFRC.uid.size;
    iUid.sak = mMFRC.PICC_GetType(mMFRC.uid.sak);

    // 🔹 Step 3: Properly reset RFID Module for next read
    //mMFRC.PICC_HaltA();        // Halt communication with the card
    //mMFRC.PCD_StopCrypto1();   // Stop encryption (if used)
    //mMFRC.PCD_Reset();         // Reset the MFRC522 module
    vTaskDelay(50 / portTICK_PERIOD_MS);  // ✅ Short delay to allow stable re-detection

    return Result::OK;
}

Result CardReader::getUid(Uid &iUid, bool &isUltralightC) {
    memcpy(iUid.uidByte, mMFRC.uid.uidByte, mMFRC.uid.size);
    iUid.size = mMFRC.uid.size;
    iUid.sak = mMFRC.PICC_GetType(mMFRC.uid.sak);
  
    INFO_PRINT("Card UID detected:");
    for (byte i = 0; i < iUid.size; i++) {
        Serial.print(iUid.uidByte[i], HEX);
        Serial.print(" ");
    }
    Serial.println();

    isUltralightC = (iUid.sak == MFRC522::PICC_TYPE_MIFARE_UL);
    return Result::OK;
}

Result CardReader::authenticateUltralightC() {
    MFRC522::StatusCode status = mMFRC.MIFARE_UL_C_Auth(mSecretKey);
    if (status != MFRC522::STATUS_OK) {
        ERROR_PRINT("Authentication failed!");
        return Result::ERROR;
    }

    INFO_PRINT("Ultralight-C Card Authenticated!");
    return Result::OK;
}

Result CardReader::readCardSecret(CardSecret &iSecret) {
    byte readBuffer[18];
    byte readBufferSize = sizeof(readBuffer);
  
    for (int block = 0x20; block <= 0x24; block += 4) {
        MFRC522::StatusCode status = mMFRC.MIFARE_Read(block, readBuffer, &readBufferSize);
        if (status == MFRC522::STATUS_OK) {
            memcpy(&iSecret.secret[(block - 0x20) * 4], readBuffer, 16);
        } else {
            ERROR_PRINT("Reading card secret failed!");
            return Result::ERROR;
        }
    }

    INFO_PRINT("Card secret read successfully.");
    return Result::OK;
}
bool CardReader::isCardPresent() {
    return mMFRC.PICC_IsNewCardPresent();
}
void CardReader::endCard() {
    Serial.println("🛑 Stopping communication with the card...");
    mMFRC.PICC_HaltA();        // ✅ Stop Communication with Card
    mMFRC.PCD_StopCrypto1();   // ✅ Prevent Interference
}