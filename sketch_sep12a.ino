#define NFC_INTERFACE_SPI
#include "NdefMessage.h"
#include "PN532_SPI.cpp"
#include "PN532_SPI.h"
#include "SPI.h"
#include "snep.h"
#include <HTTPClient.h>
#include <WiFi.h>

PN532_SPI pn532spi(SPI, 10);
SNEP nfc(pn532spi);
volatile bool interupflag = false;

char uid[4] = "Crx";
char key[30] = "00";
char modelname[10] = "deepseek-v4-pro";

void IRAM_ATTR wake() {
    interupflag = true;
}

uint8_t sendbuf[128];
uint8_t authorisebuf[128];
uint8_t receivebuf[128];
int txtlong;

void setup() {
    // put your setup code here, to run once:
    Serial0.begin(115200);
    SPI.begin();
    pinMode(22, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(22), wake, FALLING);
    // nfc.begin();
    // nfc.SAM_configuration();
    NdefMessage askmessage = NdefMessage();
    askmessage.addTextRecord(uid);
    askmessage.addTextRecord(modelname);
    askmessage.encode(sendbuf);
    txtlong = askmessage.getEncodedSize();
    NdefMessage authormessage = NdefMessage();
    authormessage.addUriRecord("https://api.deepseek.com/anthropic");
}

void loop() {
    // put your main code here, to run repeatedly:
    if (interupflag) {
        interupflag = false;
        if (0 >= nfc.write(sendbuf, txtlong)) {
            Serial0.println("fail");
            return;
        } else {
            Serial0.println("connected");
        }
        int recivelong = nfc.read(receivefbuf, sizeof(receivefbuf));
    }
}
