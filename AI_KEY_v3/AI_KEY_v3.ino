/*
 * AI_KEY_v3.ino  （按本机实际库版本修正后的版本）
 *
 * 目标：只修「编译问题」和「逻辑问题」，安全加固留到下一步。
 *
 * ---------- 本机实际库（已核对头文件） ----------
 *   C:\Users\Dust\Documents\Arduino\libraries\PN532-arduino\src\
 *       PN532.h            提供 PN532::begin() / PN532::getFirmwareVersion()
 *       PN532_SPI.h        只有 begin/wakeup/writeCommand/readResponse，没有 getFirmwareVersion()
 *       snep.h             SNEP 只有 write()/read()，【没有 begin()】；
 *                          write(buf, len, timeout) / read(buf, len, timeout)
 *                          timeout=0 表示永不超时（原来的写法就是这种）
 *                          len 参数是 uint8_t，单包最大 250 字节
 *       mac_link.cpp       activateAsTarget() 内部自己做了 pn532.begin() + SAMConfig()，
 *                          所以不需要也不能再调 nfc.begin()
 *       PN532_SPI.cpp      整个文件包在 #ifdef NFC_INTERFACE_SPI 里！
 *                          库构建时该宏未定义，PN532_SPI.cpp.o 是空的，
 *                          所以必须在 sketch 里 #define 该宏并 #include "PN532_SPI.cpp"，
 *                          否则链接期报 undefined reference to PN532_SPI::...
 *                          （v1/v2 里那两行不是随手加的，是有原因的）
 *   C:\Users\Dust\Documents\Arduino\libraries\NDEF-master\
 *       NdefRecord::getType() 返回 String（不是 uint8_t*）
 *       NdefRecord::getType(byte*) 才是取原始字节
 *       NdefMessage(const byte*, int) / encode(byte*) / getEncodedSize() ...
 *
 * ---------- 编译 / 使用 ----------
 * 1) 同一个 sketch 目录下只能有一个主 .ino（Arduino 会把目录里所有 .ino 合并编译）。
 *    本文件放在独立目录 AI_KEY_v3\ 下；v1/v2 请各自归档到子目录，否则 setup()/loop() 重复定义。
 * 2) 必须保留 #define NFC_INTERFACE_SPI 与 #include "PN532_SPI.cpp"（见上面说明）。
 *
 * ---------- 协议（沿用 v2 语义，手机端按此实现） ----------
 *   设备 -> 手机：NDEF，2 条 Text 记录（共 49 字节）
 *       rec0 = 设备 ID "Crx"
 *       rec1 = challenge 的 32 字符小写 hex（challenge 为 16 字节原始随机数）
 *   手机侧：把 rec1 的 32 个 hex 字符解码回 16 字节 raw，再计算
 *       mac = HMAC-SHA256(SHARED_KEY, rawChallenge(16B) || "Crx"(3B))
 *       —— 直接拼接，无分隔符、无长度前缀。
 *   手机 -> 设备：NDEF，1 条 Text 记录（共 71 字节）
 *       rec0 = mac 的 64 字符小写 hex
 *
 * 注意：MAC 的输入是「原始 16 字节」，不是 hex 字符串；手机端忘记先 hex 解码是本协议最易踩的坑。
 */

#include <SPI.h>
#include <string.h>
#include <mbedtls/md.h>
#include <esp_system.h>           // esp_random()
#if __has_include(<esp_random.h>)  // IDF 4.4+ 把 esp_random() 拆到独立头文件
#include <esp_random.h>
#endif

#define NFC_INTERFACE_SPI          // 必须在包含 PN532_SPI.cpp 之前

#include "PN532_SPI.h"
#include "PN532_SPI.cpp"           // 这个库的接口实现整文件包在 #ifdef NFC_INTERFACE_SPI 里，
                                   // 库自带的 .o 是空的，必须在 sketch 里把它编进来
#include "PN532.h"                 // 仅用于开机自检 PN532 固件版本 
#include "snep.h"
#include "NdefMessage.h"

// ==================== 可调参数 ====================

// 目标板：ESP32-S3。SPI.begin() 不带参数，用的是 S3 的默认 FSPI 引脚：
//     SCK = GPIO12, MISO = GPIO13, MOSI = GPIO11
// SS 由本文件指定，用 GPIO15（S3 上可用，且不是 strapping 脚）。
// 注意：ESP32-S3 没有 GPIO22~25！按键必须换成 0~21 / 26~48 里的空闲脚。
static const uint8_t  PIN_PN532_SS = 15;
static const uint8_t  PIN_TRIGGER  = 4;     // 按键：一端接 GPIO4，一端接 GND（内部上拉）
static const uint32_t DEBOUNCE_MS  = 500;

// SNEP 单次操作的超时（ms）。timeout=0 会永久阻塞，必须给值。
// 注意 read 内部会先 activate 再 waitForConnection，最坏耗时约 2 倍。
static const uint16_t SNEP_WRITE_TIMEOUT_MS = 3000;
static const uint16_t SNEP_READ_TIMEOUT_MS  = 5000;

// ===== 共享密钥（当前是公开测试向量，等于没有认证）=====
// 下面这 16 个字节是 00 01 02 … 0f，任何人拿到固件就能算出正确 MAC；先当占位符看。
// 生产要换成每台设备唯一的 256-bit 随机密钥，并且别把"烧进 eFuse"当成银弹，三种用法要分清：
//   1) eFuse 开读保护 -> 本固件也读不到它，只能交给硬件 HMAC 外设（esp_hmac_calculate）使用；
//   2) 若软件要读出来自己算 HMAC，就只能用写保护，密钥仍会进 RAM，拿到调试口/代码执行即可提取；
//   3) 要做到"设备被拿走也不泄露"，应改非对称：设备只存公钥（公开、不算秘密），
//      私钥留在手机 Keystore 里，签名由手机做、设备只验签。
// 无论走哪条，密钥都必须按设备唯一（与 DEVICE_ID 绑定），否则一台被提取，全系列失守。
static const uint8_t SHARED_KEY[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

typedef struct{
   char sourceurl[64];
   char AI_API_KEY[32];
   int ID_CODE;
}KEYBANK;

static const KEYBANK AI_KEY_BANK[1]={
   {"https//uhabdhasbad","hjashdihcusdnu",0}    
};

static const char DEVICE_ID[] = "Crx";
static const char AI_KEY[32]="bsdubu*7sadw";

enum : uint16_t {
    CHALLENGE_LEN     = 16,                  // 原始随机数长度
    CHALLENGE_HEX_LEN = CHALLENGE_LEN * 2,   // 32
    MAC_LEN           = 32,                  // SHA-256 输出
    MAC_HEX_LEN       = MAC_LEN * 2,         // 64
    PAYLOAD_MAX       = 96,                  // 单条 Text payload 上限（67 + 余量）
    REQUEST_MAX       = 96,                  // 发送 NDEF 缓冲（实际 49 字节）
    RESPONSE_MAX      = 128,                 // 接收 NDEF 缓冲（实际 71 字节）
};

// ==================== 全局对象 ====================

PN532_SPI pn532spi(SPI, PIN_PN532_SS);
SNEP      nfc(pn532spi);

static uint8_t      g_requestBuf[REQUEST_MAX];
static uint8_t      g_responseBuf[RESPONSE_MAX];

static volatile bool g_triggerPending  = false;
static uint32_t      g_lastTriggerMs   = 0;
static bool          g_haveLastTrigger = false;

// ==================== 中断 ====================

void IRAM_ATTR onTrigger() {
    g_triggerPending = true;
}

// ==================== HMAC ====================

static bool hmacSha256(const uint8_t* key, size_t keyLen,
                       const uint8_t* data, size_t dataLen,
                       uint8_t* out, size_t outLen) {
    if (key == nullptr || data == nullptr || out == nullptr || outLen < MAC_LEN) return false;

    memset(out, 0, MAC_LEN);

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) return false;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int rc = mbedtls_md_setup(&ctx, info, 1);          // 1 = HMAC
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, keyLen);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, data, dataLen);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out);

    mbedtls_md_free(&ctx);

    if (rc != 0) {
        memset(out, 0, MAC_LEN);
        return false;
    }
    return true;
}

// 常量时间比较，避免按字节提前返回
static bool hmacEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    if (a == nullptr || b == nullptr || len == 0) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff = (uint8_t)(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

// ==================== hex 编解码 ====================

static bool bytesToHex(const uint8_t* in, size_t n, char* out, size_t outCap) {
    if (in == nullptr || out == nullptr || outCap < n * 2 + 1) return false;
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
    return true;
}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hexToBytes(const char* in, size_t n, uint8_t* out, size_t outLen) {
    if (in == nullptr || out == nullptr || outLen == 0) return false;
    if (n != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        int hi = hexNibble(in[i * 2]);
        int lo = hexNibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// ==================== challenge / MAC ====================

static void generateChallenge(uint8_t out[CHALLENGE_LEN]) {
    for (size_t i = 0; i < CHALLENGE_LEN; i++) {
        out[i] = (uint8_t)(esp_random() & 0xFF);
    }
}

// mac = HMAC-SHA256(SHARED_KEY, rawChallenge || deviceId)
static bool computeExpectedMac(const uint8_t* challenge, size_t chLen,
                               const char* deviceId, uint8_t out[MAC_LEN]) {
    if (challenge == nullptr || deviceId == nullptr) return false;
    memset(out, 0, MAC_LEN);

    uint8_t buf[CHALLENGE_LEN + 32];
    size_t idLen = strlen(deviceId);
    if (chLen > CHALLENGE_LEN || chLen + idLen > sizeof(buf)) return false;

    memcpy(buf, challenge, chLen);
    memcpy(buf + chLen, deviceId, idLen);

    return hmacSha256(SHARED_KEY, sizeof(SHARED_KEY), buf, chLen + idLen, out, MAC_LEN);
}

// ==================== NDEF 收发 ====================
static bool sendNdef(NdefMessage&msg){
    int encodedSize = msg.getEncodedSize();
    if (encodedSize <= 0 ||
        encodedSize > (int)sizeof(g_requestBuf) ||
        encodedSize > 250) {                       // SNEP 单包上限
        Serial.printf("[ERR] request 编码超长: %d\n", encodedSize);
        return false;
    }
    msg.encode(g_requestBuf);
    int8_t sent = nfc.write(g_requestBuf, (uint8_t)encodedSize, SNEP_WRITE_TIMEOUT_MS);
    if (sent <= 0) {
        Serial.printf("[ERR] nfc.write() = %d（对端未就位？）\n", (int)sent);
        return false;
    }
    return true;
}

static bool sendChallenge(const char* challengeHex) {
    NdefMessage request = NdefMessage();
    request.addTextRecord(DEVICE_ID);
    request.addTextRecord(challengeHex);
    return sendNdef(request);
}

// 从对端回写的 NDEF 中取出 32 字节 MAC
static bool extractMacFromNdef(const uint8_t* data, int len, uint8_t out[MAC_LEN]) {
    if (data == nullptr || len < 2) return false;
    memset(out, 0, MAC_LEN);

    // 解析前先看 TNF 字节：MB(0x80) | ME(0x40) | SR(0x10) 必须都置位。
    // 我们的响应固定是"单条、短记录"；这同时挡掉了 NdefMessage 解析器
    // 对 4 字节长度字段（可造出 GB 级 malloc）的处理路径。
    if ((data[0] & 0xD0) != 0xD0) return false;

    NdefMessage msg = NdefMessage(data, len);

    // 协议规定只收 1 条记录；多给的一律拒绝
    if (msg.getRecordCount() != 1) return false;

    NdefRecord rec = msg.getRecord(0);

    if (rec.getTnf() != TNF_WELL_KNOWN) return false;
    if (rec.getTypeLength() != 1) return false;

    // 本库的 getType() 返回 String（另一版本才返回 uint8_t*，v1 的写法在这里编译不过）
    String type = rec.getType();
    if (type != "T") return false;

    int payloadLength = rec.getPayloadLength();
    // 长度必须在使用前校验：payloadLength 来自对端
    if (payloadLength <= 0 || payloadLength > (int)PAYLOAD_MAX) return false;

    uint8_t payload[PAYLOAD_MAX];      // 固定大小，不用 VLA
    rec.getPayload(payload);

    size_t langLen   = payload[0] & 0x3F;      // 状态字节低 6 位 = 语言码长度
    size_t textStart = 1 + langLen;
    if (textStart > (size_t)payloadLength) return false;

    size_t textLen = (size_t)payloadLength - textStart;
    if (textLen != (size_t)MAC_HEX_LEN) return false;

    char hex[MAC_HEX_LEN + 1];
    memcpy(hex, payload + textStart, textLen);
    hex[textLen] = '\0';

    return hexToBytes(hex, textLen, out, (size_t)MAC_LEN);
}
//======================秘钥传输=========================
static bool sendAI_KEY(){
    NdefMessage AIkey = NdefMessage();
    AIkey.addTextRecord(AI_KEY);
    return sendNdef(AIkey);
}


// ==================== 一次认证往返 ====================

static void runAuthTransaction() {
    uint8_t challenge[CHALLENGE_LEN];
    char    challengeHex[CHALLENGE_HEX_LEN + 1];

    generateChallenge(challenge);
    if (!bytesToHex(challenge, sizeof(challenge), challengeHex, sizeof(challengeHex))) {
        Serial.println("[ERR] challenge hex 编码失败");
        return;
    }
    Serial.printf("[TX] uid=%s challenge=%s\n", DEVICE_ID, challengeHex);

    if (!sendChallenge(challengeHex)) return;

    memset(g_responseBuf, 0, sizeof(g_responseBuf));
    int16_t received = nfc.read(g_responseBuf, (uint8_t)sizeof(g_responseBuf), SNEP_READ_TIMEOUT_MS);
    if (received <= 0) {
        Serial.printf("[ERR] nfc.read() = %d（对端未回写或超时）\n", (int)received);
        return;
    }

    uint8_t receivedMac[MAC_LEN];
    if (!extractMacFromNdef(g_responseBuf, received, receivedMac)) {
        Serial.println("[FAIL] 回包不是合法的 MAC 文本记录");
        return;
    }

    uint8_t expectedMac[MAC_LEN];
    if (!computeExpectedMac(challenge, sizeof(challenge), DEVICE_ID, expectedMac)) {
        Serial.println("[ERR] HMAC 计算失败");
        return;
    }

    if (hmacEquals(expectedMac, receivedMac, MAC_LEN)) {
        Serial.println("[OK] AUTH OK: 对端持有共享密钥");
        sendAI_KEY();
    } else {
        Serial.println("[FAIL] AUTH FAILED: 密钥错误或回包被篡改");
    }
}

// ==================== 初始化 / 主循环 ====================

void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("AI_KEY v3 boot");

    SPI.begin();

    // PN532 初始化由 SNEP 内部完成（mac_link.cpp 的 activateAsTarget 里调了
    // pn532.begin() + SAMConfig()），SNEP 类本身没有 begin()。
    // 这里只用 PN532 对象做一次固件版本自检。
    PN532 probe(pn532spi);
    probe.begin();
    uint32_t version = probe.getFirmwareVersion();
    if (version == 0) {
        Serial.println("[ERR] PN532 无响应：查 DIP 是否拨到 SPI、共地、SS=GPIO15，S3 默认脚 SCK12/MISO13/MOSI11");
    } else {
        Serial.printf("[OK] PN532 firmware = 0x%08X\n", (unsigned)version);
    }

    pinMode(PIN_TRIGGER, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_TRIGGER), onTrigger, FALLING);

    Serial.printf("[READY] 按 GPIO%u 触发一次认证\n", (unsigned)PIN_TRIGGER);
}

void loop() {
    if (!g_triggerPending) {
        delay(5);                       // 空闲时不要空转占满 CPU
        return;
    }
    g_triggerPending = false;

    uint32_t now = millis();
    if (g_haveLastTrigger && (now - g_lastTriggerMs) < DEBOUNCE_MS) {
        return;                         // 机械抖动，忽略
    }
    g_haveLastTrigger = true;
    g_lastTriggerMs   = now;

    runAuthTransaction();

    // 事务执行期间的按键不再自动触发下一轮，必须重新按一次
    g_triggerPending = false;
}
