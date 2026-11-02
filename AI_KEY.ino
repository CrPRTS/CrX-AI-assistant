#define NFC_INTERFACE_SPI
#include "SPI.h"
#include "PN532_SPI.h"
#include "PN532_SPI.cpp"
#include "snep.h"
#include "NdefMessage.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <string.h>
#include <mbedtls/md.h>

static const uint8_t SHARED_KEY[16] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
};



// ========== HMAC 封装 ==========
void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* data, size_t dataLen,
                uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, keyLen);
    mbedtls_md_hmac_update(&ctx, data, dataLen);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

bool hmacVerify(const uint8_t* expected, const uint8_t* received, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= expected[i] ^ received[i];
    return diff == 0;
}




PN532_SPI pn532spi(SPI, 10);
SNEP nfc(pn532spi); 
volatile bool interupflag= false ;

char uid[16]="Crx";
char aikey[32]="00";
char sourceurl[50]="https://api.deepseek.com/anthropic";
char challenge[16];

typedef struct bank{
   char driver[10];
}userbank;



void IRAM_ATTR wake(){
  interupflag= true;
}

uint8_t idcheck_sendbuf[64];
uint8_t key_sendbuf[64];
uint8_t authorisebuf[128];
uint8_t receivebuf[128];

int txtlong;

void challengegenerator(){
   for (uint8_t i = 0; i < 16; i++)
   { 
      challenge[i] = esp_random() & 0xFF;
   }
}


void responderCompute( char* challenge, size_t chLen,
                     char* deviceId, uint8_t out[32]) {
    // 输入 = challenge || deviceId，绑定上下文防跨设备重放
    uint8_t buf[128];
    size_t idLen = strlen(deviceId);
    if (chLen + idLen > sizeof(buf)) return;
    memcpy(buf, challenge, chLen);
    memcpy(buf + chLen, deviceId, idLen);

    hmacSha256(SHARED_KEY, sizeof(SHARED_KEY), buf, chLen + idLen, out);
}




void setup() {
  // put your setup code here, to run once:
 Serial0.begin(115200);
 SPI.begin();  
 pinMode(22,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(22), wake, FALLING);
  //nfc.begin();
  //nfc.SAM_configuration();
  /*NdefMessage askmessage = NdefMessage();
  askmessage.addTextRecord(uid);
  askmessage.addTextRecord(challenge);
  askmessage.encode(sendbuf);
  txtlong =askmessage.getEncodedSize();*/
 // NdefMessage authormessage = NdefMessage();
  //authormessage.addUriRecord(sourceurl);
}

   
void loop() {
  // put your main code here, to run repeatedly:
 if(interupflag){
    interupflag=false;

     challengegenerator();
     NdefMessage askmessage = NdefMessage();
     askmessage.addTextRecord(uid);
      askmessage.addTextRecord(challenge);
     askmessage.encode(idcheck_sendbuf);
     txtlong =askmessage.getEncodedSize();







    if(0>=nfc.write(idcheck_sendbuf,txtlong)){
       Serial0.println("fail");
       return;
    }else{
       Serial0.println("connected");


    }
   int receivelong = nfc.read(receivebuf, sizeof(receivebuf));
   /*if(receivelong){
      NdefMessage msg = NdefMessage(receivebuf, receivelong);
       Serial0.println("ok");
      for(int i=0;i<2;i++){
    NdefRecord record = msg.getRecord(i);
     if(record.getTnf()!= TNF_WELL_KNOWN) Serial0.println("illegle txt"); return;
    int payloadLength = record.getPayloadLength();
    uint8_t payload[payloadLength];
    record.getPayload(payload);
    //uint8_t* txtaim=payload+ (payload[0] & 0x3F +1);
   }*/
   if (receivelong>0) {
    NdefMessage msg = NdefMessage(receivebuf, receivelong);
    Serial0.println("ok");

    if (msg.getRecordCount() > 0) {
        NdefRecord id_record = msg.getRecord(0);

        // 文本记录检查：TNF 和 Type 都要查
        if (id_record.getTnf() != TNF_WELL_KNOWN || id_record.getType() != "T") {
            Serial0.println("illegal txt");
            return;
        }

        int payloadLength = id_record.getPayloadLength();
        uint8_t payload[payloadLength];
        id_record.getPayload(payload);

        // 状态字节低 6 位 = 语言代码长度
        int langCodeLength = payload[0] & 0x3F;
        int textStart = 1 + langCodeLength;
        // 提取文本
        char response[payloadLength - langCodeLength];
        for (int i = textStart; i < payloadLength; i++) {
            response[i - textStart] =(char) payload[i];
        }
        //textBuffer[payloadLength - textStart] = '\0';

        Serial0.print("response: ");
        Serial0.println(response);
        uint8_t expected[32];
        responderCompute(challenge, 16, uid, expected);
        if (hmacVerify(expected, response, 32)) {
        Serial.println("Auth OK: B holds the shared key.");
    } else {
        Serial.println("Auth FAILED: wrong key or tampered response.");
    }
    }
   }
 }     
}