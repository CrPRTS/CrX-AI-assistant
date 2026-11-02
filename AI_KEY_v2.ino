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

// 修复点8: TODO: 生产环境请从 NVS / eFuse 读取，避免硬编码密钥随固件泄露。
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

// 修复点7: 保持常量时间比较（diff |=），签名不变
bool hmacVerify(const uint8_t* expected, const uint8_t* received, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= expected[i] ^ received[i];
    return diff == 0;
}

// 修复点2: toHex——二进制转 hex 字符串，输出 2n+1 字节（含结尾 '\0'）
void toHex(const uint8_t* in, size_t n, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

// 修复点2: hexToBytes——严格校验 n == outLen*2 且字符合法
bool hexToBytes(const char* in, size_t n, uint8_t* out, size_t outLen) {
    if (n != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        char hi = in[i * 2];
        char lo = in[i * 2 + 1];
        uint8_t h, l;
        if (hi >= '0' && hi <= '9')      h = (uint8_t)(hi - '0');
        else if (hi >= 'a' && hi <= 'f') h = (uint8_t)(hi - 'a' + 10);
        else if (hi >= 'A' && hi <= 'F') h = (uint8_t)(hi - 'A' + 10);
        else return false;
        if (lo >= '0' && lo <= '9')      l = (uint8_t)(lo - '0');
        else if (lo >= 'a' && lo <= 'f') l = (uint8_t)(lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') l = (uint8_t)(lo - 'A' + 10);
        else return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

PN532_SPI pn532spi(SPI, 10);
SNEP nfc(pn532spi);
volatile bool interupflag = false;

char uid[16] = "Crx";
char aikey[32] = "00";
char sourceurl[50] = "https://api.deepseek.com/anthropic";

// 修复点5: challenge 由 char[16] 改为 uint8_t[16]
uint8_t challenge[16];

typedef struct bank{
   char driver[10];
}userbank;

void IRAM_ATTR wake(){
  interupflag = true;
}

uint8_t idcheck_sendbuf[64];
uint8_t key_sendbuf[64];
uint8_t authorisebuf[128];
uint8_t receivebuf[128];

int txtlong;

// 修复点5: challenge 生成，保持 esp_random() 填 16 字节
void challengegenerator(){
   for (uint8_t i = 0; i < 16; i++)
   {
      challenge[i] = (uint8_t)(esp_random() & 0xFF);
   }
}

// 修复点6: responderCompute 签名调整，buf 越界检查后 return
void responderCompute(const uint8_t* challenge, size_t chLen,
                      const char* deviceId, uint8_t out[32]) {
    // 输入 = challenge || deviceId，绑定上下文防跨设备重放
    uint8_t buf[128];
    size_t idLen = strlen(deviceId);
    if (chLen + idLen > sizeof(buf)) return;
    memcpy(buf, challenge, chLen);
    memcpy(buf + chLen, deviceId, idLen);

    hmacSha256(SHARED_KEY, sizeof(SHARED_KEY), buf, chLen + idLen, out);
}

void setup() {
  // 修复点10: 删除被注释掉的 askmessage / authormessage 块
  Serial0.begin(115200);
  SPI.begin();
  pinMode(22, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(22), wake, FALLING);
  //nfc.begin();
  //nfc.SAM_configuration();
}

void loop() {
  if(interupflag){
    interupflag = false;

    challengegenerator();

    // 修复点5: challenge 生成后立即 hex 编码用于发送（16 字节 -> 32 字符）
    char challengeHex[33];
    toHex(challenge, sizeof(challenge), challengeHex);

    // 修复点9: 协议约定——发送端两条 Text 记录：record0=uid，record1=challengeHex(32字符)
    NdefMessage askmessage = NdefMessage();
    askmessage.addTextRecord(uid);
    askmessage.addTextRecord(challengeHex);
    askmessage.encode(idcheck_sendbuf);
    txtlong = askmessage.getEncodedSize();

    if(0 >= nfc.write(idcheck_sendbuf, txtlong)){
       Serial0.println("fail");
       return;
    } else {
       Serial0.println("connected");
    }

    // 修复点3: 读取前清空 receivebuf
    memset(receivebuf, 0, sizeof(receivebuf));
    int receivelong = nfc.read(receivebuf, sizeof(receivebuf));

    // 修复点3: receivelong <= 0 直接 return
    if (receivelong <= 0) return;

    NdefMessage msg = NdefMessage(receivebuf, receivelong);
    Serial0.println("ok");

    // 修复点4: 记录数量至少为 1
    if (msg.getRecordCount() < 1) {
        Serial0.println("Auth FAILED: malformed response");
        return;
    }

    NdefRecord id_record = msg.getRecord(0);

    // 修复点4: TNF 校验 + Type 校验（getType 返回 uint8_t*，用 getTypeLength/getType 判断）
    if (id_record.getTnf() != TNF_WELL_KNOWN ||
        id_record.getTypeLength() != 1 ||
        id_record.getType()[0] != 'T') {
        Serial0.println("illegal txt");
        return;
    }

    int payloadLength = id_record.getPayloadLength();

    // 修复点4: payload 长度校验前置
    if (payloadLength <= 0) {
        Serial0.println("Auth FAILED: malformed response");
        return;
    }

    uint8_t payload[payloadLength];
    id_record.getPayload(payload);

    int langCodeLength = payload[0] & 0x3F;
    int textStart = 1 + langCodeLength;
    int textLen = payloadLength - textStart;

    // 修复点4: 解出的文本长度必须正好 64（对应 32 字节 HMAC）
    if (textLen != 64) {
        Serial0.println("Auth FAILED: malformed response");
        return;
    }

    // 修复点3: response 缓冲区大小 = payloadLength - textStart + 1（含结尾 '\0'）
    char response[payloadLength - textStart + 1];
    for (int i = textStart; i < payloadLength; i++) {
        response[i - textStart] = (char)payload[i];
    }
    response[payloadLength - textStart] = '\0';

    Serial0.print("response: ");
    Serial0.println(response);

    // 修复点1/2: 64 字符 hex 解码成 uint8_t received[32]，再传入 hmacVerify
    uint8_t received[32];
    if (!hexToBytes(response, textLen, received, sizeof(received))) {
        Serial0.println("Auth FAILED: malformed response");
        return;
    }

    // 修复点5: 本地比较用原始 16 字节 challenge
    uint8_t expected[32];
    responderCompute(challenge, sizeof(challenge), uid, expected);

    // 修复点1: 传入 uint8_t received[32]，不再传 char*
    if (hmacVerify(expected, received, sizeof(received))) {
        Serial0.println("Auth OK: B holds the shared key.");
    } else {
        Serial0.println("Auth FAILED: wrong key or tampered response.");
    }
  }
}
