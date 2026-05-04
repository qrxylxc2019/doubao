/*
   名称: ESP32-S3 DeepSeek + 讯飞TTS 语音助手
   功能: WiFi连接 → NTP时间同步 → DeepSeek对话 → 讯飞TTS语音播报
*/

// ESP32-S3 关键：把 Serial 重定向到 Serial0（UART0），否则串口监视器看不到输出
#if ARDUINO_USB_CDC_ON_BOOT
#define Serial Serial0
#endif

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "mbedtls/md.h"

// ==================== WiFi 配置 ====================
const char* ssid = "tp-link";
const char* password = "qc13798425457";

// ==================== DeepSeek API 配置 ====================
const char* deepseekApiKey = "sk-1a60ba4b6fcd48ee8e6169175065bdf9";
const char* deepseekHost = "api.deepseek.com";
const char* deepseekApiUrl = "/v1/chat/completions";

// ==================== 讯飞TTS 配置 ====================
const char* xfyunAppId = "b42ec0da";
const char* xfyunApiKey = "d41c227f91aa6f4cd40021be0692f0a1";
const char* xfyunApiSecret = "NWE3YWI2Y2JlNjljZWU4MGM0NGIxZDc5";
const char* xfyunHost = "tts-api.xfyun.cn";
const char* xfyunPath = "/v2/tts";

WiFiClientSecure deepseekClient;
WiFiClientSecure ttsClient;

// ==================== 函数声明 ====================
String callDeepSeek(String prompt);
bool callXfyunTTS(String text);
String generateXfyunAuthUrl();
String urlEncode(const char* str);
String base64EncodeText(String text);

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    delay(1000);
    Serial.println("\n==========================================");
    Serial.println(" Doubao / DeepSeek + 讯飞TTS 语音助手");
    Serial.println("==========================================");

    // 连接 WiFi
    Serial.print("正在连接 WiFi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi 连接成功！");
    Serial.print("IP 地址: ");
    Serial.println(WiFi.localIP());

    // 跳过证书验证
    deepseekClient.setInsecure();
    ttsClient.setInsecure();

    // ====== 关键：NTP 时间同步 ======
    // 讯飞TTS鉴权要求date参数与服务器时间偏差不超过300秒
    // 没有NTP同步，time(nullptr)返回1970年，导致403 Forbidden
    Serial.print("正在同步NTP时间...");
    configTime(8 * 3600, 0, "ntp1.aliyun.com", "ntp2.aliyun.com", "pool.ntp.org");
    struct tm timeinfo;
    int ntpRetries = 0;
    while (!getLocalTime(&timeinfo) && ntpRetries < 30) {
        delay(500);
        Serial.print(".");
        ntpRetries++;
    }
    if (ntpRetries < 30) {
        Serial.println(" 同步成功！");
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.print("当前时间: ");
        Serial.println(timeStr);
    } else {
        Serial.println(" 同步失败！讯飞TTS将无法正常工作");
    }

    Serial.println("\n✅ 系统准备就绪！");
    Serial.println("请在串口监视器输入文字，按回车发送\n");
    Serial.println("==========================================\n");
}

void loop() {
    if (Serial.available()) {
        String userInput = Serial.readStringUntil('\n');
        userInput.trim();

        if (userInput.length() > 0) {
            Serial.print("\n🤔 你说: ");
            Serial.println(userInput);

            // 1. 调用 DeepSeek 获取回复
            String reply = callDeepSeek(userInput);
            Serial.print("🤖 DeepSeek: ");
            Serial.println(reply);
            Serial.println();

            // 2. 调用讯飞TTS 播放回复
            Serial.println("🔊 正在语音合成...");
            bool ttsOk = callXfyunTTS(reply);
            if (ttsOk) {
                Serial.println("✅ 语音播放完成");
            } else {
                Serial.println("❌ 语音合成失败");
            }

            Serial.println("\n------------------------\n");
        }
    }
}

// ==================== DeepSeek 调用函数 ====================
String callDeepSeek(String prompt) {
    if (WiFi.status() != WL_CONNECTED) {
        return "WiFi 未连接";
    }

    Serial.print("正在连接 DeepSeek API...");
    if (!deepseekClient.connect(deepseekHost, 443)) {
        return "连接服务器失败";
    }
    Serial.println(" 已连接");

    StaticJsonDocument<512> doc;
    doc["model"] = "deepseek-chat";
    JsonArray messages = doc.createNestedArray("messages");
    JsonObject userMsg = messages.createNestedObject();
    userMsg["role"] = "user";
    userMsg["content"] = prompt;

    String requestBody;
    serializeJson(doc, requestBody);

    deepseekClient.println(String("POST ") + deepseekApiUrl + " HTTP/1.1");
    deepseekClient.println(String("Host: ") + deepseekHost);
    deepseekClient.println(String("Authorization: Bearer ") + deepseekApiKey);
    deepseekClient.println("Content-Type: application/json");
    deepseekClient.print("Content-Length: ");
    deepseekClient.println(requestBody.length());
    deepseekClient.println();
    deepseekClient.println(requestBody);

    unsigned long timeout = millis();
    while (!deepseekClient.available()) {
        if (millis() - timeout > 15000) {
            deepseekClient.stop();
            return "请求超时";
        }
        delay(10);
    }

    // 读取状态行
    String statusLine = deepseekClient.readStringUntil('\n');
    statusLine.trim();
    if (statusLine.indexOf("200") == -1) {
        deepseekClient.stop();
        return "API 错误: " + statusLine;
    }

    // 读取响应头，检查是否为 chunked 编码
    bool isChunked = false;
    String headerLine;
    while (deepseekClient.available()) {
        headerLine = deepseekClient.readStringUntil('\n');
        headerLine.trim();
        if (headerLine.length() == 0) break;  // 空行表示头部结束
        if (headerLine.indexOf("Transfer-Encoding: chunked") >= 0 ||
            headerLine.indexOf("transfer-encoding: chunked") >= 0) {
            isChunked = true;
        }
    }

    // 读取响应体
    delay(200);
    String responseBody = "";
    while (deepseekClient.available()) {
        responseBody += (char)deepseekClient.read();
    }
    deepseekClient.stop();

    // 如果是 chunked 编码，需要解码
    if (isChunked) {
        String decoded = "";
        int pos = 0;
        while (pos < responseBody.length()) {
            // 找到 chunk 大小行（hex数字 + \r\n）
            int crlfPos = responseBody.indexOf("\r\n", pos);
            if (crlfPos == -1) break;
            String sizeStr = responseBody.substring(pos, crlfPos);
            sizeStr.trim();
            int chunkSize = strtol(sizeStr.c_str(), NULL, 16);
            if (chunkSize == 0) break;  // 最后一个 chunk
            pos = crlfPos + 2;  // 跳过 \r\n
            // 读取 chunk 数据
            if (pos + chunkSize <= responseBody.length()) {
                decoded += responseBody.substring(pos, pos + chunkSize);
            }
            pos += chunkSize + 2;  // 跳过 chunk 数据 + 尾部 \r\n
        }
        responseBody = decoded;
    }
    responseBody.trim();

    // 调试输出
    Serial.print("[DEBUG] 响应体前200字: ");
    Serial.println(responseBody.substring(0, 200));

    StaticJsonDocument<4096> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, responseBody);
    if (error) {
        Serial.print("[DEBUG] JSON错误: ");
        Serial.println(error.c_str());
        return "JSON 解析失败";
    }

    const char* reply = responseDoc["choices"][0]["message"]["content"];
    if (!reply) return "响应中无内容";
    return String(reply);
}

// ==================== 讯飞TTS 调用函数 ====================
// 生成鉴权URL（带authorization、date、host参数）
String generateXfyunAuthUrl() {
    // 获取当前GMT时间（RFC1123格式）
    time_t now = time(nullptr);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char dateStr[64];
    strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y %H:%M:%S GMT", &timeinfo);

    Serial.print("[DEBUG] 鉴权时间: ");
    Serial.println(dateStr);

    // 构造签名原文（注意格式：host: xxx\ndate: xxx\nGET xxx HTTP/1.1）
    String signatureOrigin = "host: " + String(xfyunHost) + "\ndate: " + dateStr + "\nGET " + String(xfyunPath) + " HTTP/1.1";

    Serial.print("[DEBUG] 签名原文: ");
    Serial.println(signatureOrigin);

    // HMAC-SHA256 签名
    uint8_t hmacResult[32];
    int ret = mbedtls_md_hmac(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        (const unsigned char*)xfyunApiSecret, strlen(xfyunApiSecret),
        (const unsigned char*)signatureOrigin.c_str(), signatureOrigin.length(),
        hmacResult
    );

    if (ret != 0) {
        Serial.print("[ERROR] HMAC-SHA256 计算失败, ret=");
        Serial.println(ret);
        return "";
    }

    // Base64 编码签名
    char signatureBase64[64];
    size_t sigLen;
    mbedtls_base64_encode((uint8_t*)signatureBase64, sizeof(signatureBase64), &sigLen, hmacResult, 32);
    signatureBase64[sigLen] = '\0';

    Serial.print("[DEBUG] 签名Base64: ");
    Serial.println(signatureBase64);

    // 构造 authorization 原始字符串
    String authorizationOrigin = "api_key=\"" + String(xfyunApiKey) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + String(signatureBase64) + "\"";

    // Base64 编码 authorization
    size_t authLen = authorizationOrigin.length();
    char authorizationBase64[512];
    size_t authBase64Len;
    mbedtls_base64_encode((uint8_t*)authorizationBase64, sizeof(authorizationBase64), &authBase64Len, (const uint8_t*)authorizationOrigin.c_str(), authLen);
    authorizationBase64[authBase64Len] = '\0';

    // 构造最终 URL
    String url = "wss://" + String(xfyunHost) + String(xfyunPath) + "?authorization=" + String(authorizationBase64) + "&date=" + urlEncode(dateStr) + "&host=" + String(xfyunHost);
    return url;
}

// URL 编码辅助函数
String urlEncode(const char* str) {
    String encoded = "";
    char c;
    for (size_t i = 0; i < strlen(str); i++) {
        c = str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char buf[4];
            sprintf(buf, "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}

// 发送 WebSocket 文本帧（客户端必须使用掩码）
void sendWebSocketFrame(WiFiClientSecure& client, const String& payload) {
    size_t len = payload.length();

    // 第一个字节：0x81 = FIN + 文本帧(opcode=0x1)
    uint8_t firstByte = 0x81;
    client.write(&firstByte, 1);

    // 第二个字节：MASK=1 + payload长度
    uint8_t secondByte;
    if (len <= 125) {
        secondByte = 0x80 | (uint8_t)len;
        client.write(&secondByte, 1);
    } else if (len <= 65535) {
        secondByte = 0x80 | 126;
        client.write(&secondByte, 1);
        uint8_t extLen[2] = {(uint8_t)(len >> 8), (uint8_t)(len & 0xFF)};
        client.write(extLen, 2);
    } else {
        secondByte = 0x80 | 127;
        client.write(&secondByte, 1);
        uint8_t extLen[8] = {0};
        uint64_t l = len;
        for (int i = 7; i >= 0; i--) { extLen[i] = l & 0xFF; l >>= 8; }
        client.write(extLen, 8);
    }

    // 4字节掩码（客户端帧必须使用掩码，RFC6455规范）
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    client.write(mask, 4);

    // 发送掩码后的payload
    for (size_t i = 0; i < len; i++) {
        uint8_t b = payload[i] ^ mask[i % 4];
        client.write(&b, 1);
    }
}

// 发送 WebSocket 关闭帧
void sendWebSocketClose(WiFiClientSecure& client) {
    uint8_t closeFrame[4] = {0x88, 0x80, 0x00, 0x00}; // FIN+Close, MASK+0 len, mask
    client.write(closeFrame, 4);
}

// 调用讯飞TTS（WebSocket帧协议版）
bool callXfyunTTS(String text) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi 未连接");
        return false;
    }

    // 生成鉴权URL
    String authUrl = generateXfyunAuthUrl();
    if (authUrl.length() == 0) {
        Serial.println("❌ 鉴权URL生成失败");
        return false;
    }
    Serial.print("TTS 鉴权URL: ");
    Serial.println(authUrl.substring(0, 100) + "...");

    // 解析 wss://host/path?query
    String host = String(xfyunHost);
    int pathStart = authUrl.indexOf('/', 6);
    String pathAndQuery = authUrl.substring(pathStart);

    Serial.print("正在连接讯飞TTS...");
    if (!ttsClient.connect(host.c_str(), 443)) {
        Serial.println(" 连接失败");
        return false;
    }
    Serial.println(" 已连接");

    // 发送 WebSocket 握手请求
    ttsClient.println("GET " + pathAndQuery + " HTTP/1.1");
    ttsClient.println("Host: " + host);
    ttsClient.println("Upgrade: websocket");
    ttsClient.println("Connection: Upgrade");
    ttsClient.println("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==");
    ttsClient.println("Sec-WebSocket-Version: 13");
    ttsClient.println();

    // 等待握手响应
    unsigned long wsTimeout = millis();
    while (!ttsClient.available()) {
        if (millis() - wsTimeout > 10000) {
            Serial.println("WebSocket 握手超时");
            ttsClient.stop();
            return false;
        }
        delay(10);
    }

    // 读取握手响应
    String response = "";
    while (ttsClient.available()) {
        String line = ttsClient.readStringUntil('\n');
        response += line + "\n";
        if (line.length() <= 1) break;
    }
    Serial.print("WebSocket 响应: ");
    Serial.println(response.substring(0, 100));

    if (response.indexOf("101") == -1) {
        Serial.println("WebSocket 握手失败");
        // 打印更多响应信息帮助排查
        Serial.print("[DEBUG] 完整响应: ");
        Serial.println(response);
        ttsClient.stop();
        return false;
    }

    // 握手成功！构造 TTS 请求 JSON
    StaticJsonDocument<1024> ttsDoc;
    ttsDoc["common"]["app_id"] = xfyunAppId;
    ttsDoc["business"]["aue"] = "lame";        // mp3格式
    ttsDoc["business"]["sfl"] = 1;             // 流式返回mp3
    ttsDoc["business"]["auf"] = "audio/L16;rate=16000";
    ttsDoc["business"]["vcn"] = "xiaoyan";     // 发音人：小燕
    ttsDoc["business"]["speed"] = 50;
    ttsDoc["business"]["volume"] = 50;
    ttsDoc["business"]["pitch"] = 50;
    ttsDoc["business"]["tte"] = "UTF8";
    ttsDoc["data"]["status"] = 2;              // 一次性发送
    ttsDoc["data"]["text"] = base64EncodeText(text);

    String ttsRequest;
    serializeJson(ttsDoc, ttsRequest);

    Serial.print("发送TTS请求: ");
    Serial.println(ttsRequest.substring(0, 200));

    // 通过 WebSocket 文本帧发送（必须封装帧格式，不能直接println）
    sendWebSocketFrame(ttsClient, ttsRequest);

    // 接收 WebSocket 响应帧
    bool gotAudio = false;
    String allTextData = "";
    unsigned long ttsTimeout = millis();

    while (millis() - ttsTimeout < 15000) {
        if (ttsClient.available()) {
            // 读取帧头第一个字节
            uint8_t firstByte = ttsClient.read();

            // 等待第二个字节
            unsigned long byteTimeout = millis();
            while (!ttsClient.available()) {
                if (millis() - byteTimeout > 3000) goto recv_done;
                delay(1);
            }
            uint8_t secondByte = ttsClient.read();

            uint8_t opcode = firstByte & 0x0F;
            bool isMasked = (secondByte & 0x80) != 0;
            uint64_t payloadLen = secondByte & 0x7F;

            // 读取扩展长度
            if (payloadLen == 126) {
                unsigned long bt = millis();
                while (ttsClient.available() < 2) { if (millis() - bt > 3000) goto recv_done; delay(1); }
                uint8_t extLen[2];
                ttsClient.read(extLen, 2);
                payloadLen = ((uint64_t)extLen[0] << 8) | extLen[1];
            } else if (payloadLen == 127) {
                unsigned long bt = millis();
                while (ttsClient.available() < 8) { if (millis() - bt > 3000) goto recv_done; delay(1); }
                uint8_t extLen[8];
                ttsClient.read(extLen, 8);
                payloadLen = 0;
                for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | extLen[i];
            }

            // 读取掩码
            uint8_t mask[4] = {0};
            if (isMasked) {
                unsigned long bt = millis();
                while (ttsClient.available() < 4) { if (millis() - bt > 3000) goto recv_done; delay(1); }
                ttsClient.read(mask, 4);
            }

            // 读取payload（限制最大64KB）
            size_t readLen = (payloadLen > 65536) ? 65536 : (size_t)payloadLen;
            String framePayload = "";
            framePayload.reserve(readLen);
            unsigned long bt = millis();
            while (framePayload.length() < readLen) {
                if (ttsClient.available()) {
                    char c = ttsClient.read();
                    if (isMasked) c ^= mask[framePayload.length() % 4];
                    framePayload += c;
                } else if (millis() - bt > 5000) {
                    break;
                } else {
                    delay(1);
                }
            }

            // 文本帧：解析JSON
            if (opcode == 0x01 && framePayload.length() > 0) {
                Serial.print("[DEBUG] 收到帧(len=");
                Serial.print(framePayload.length());
                Serial.print("): ");
                Serial.println(framePayload.substring(0, 200));

                // 检查错误码
                StaticJsonDocument<512> frameDoc;
                DeserializationError err = deserializeJson(frameDoc, framePayload);
                if (!err) {
                    int code = frameDoc["code"] | 0;
                    if (code != 0) {
                        Serial.print("[ERROR] 讯飞TTS错误码: ");
                        Serial.println(code);
                        String message = frameDoc["message"] | "未知错误";
                        Serial.print("[ERROR] 错误信息: ");
                        Serial.println(message);
                        ttsClient.stop();
                        return false;
                    }

                    // 检查是否有音频数据
                    const char* audio = frameDoc["data"]["audio"];
                    if (audio && strlen(audio) > 0) {
                        gotAudio = true;
                    }

                    // 检查是否合成完成（status=2表示最后一块）
                    int status = frameDoc["data"]["status"] | -1;
                    if (status == 2) {
                        Serial.println("✅ TTS合成完成");
                        sendWebSocketClose(ttsClient);
                        delay(100);
                        ttsClient.stop();
                        return gotAudio;
                    }
                }
            }

            // 关闭帧
            if (opcode == 0x08) {
                Serial.println("[INFO] 收到WebSocket关闭帧");
                ttsClient.stop();
                return gotAudio;
            }

            ttsTimeout = millis();  // 收到数据则重置超时
        } else {
            delay(10);
        }
    }

recv_done:
    ttsClient.stop();

    if (gotAudio) {
        Serial.println("✅ 收到TTS数据（超时退出）");
        return true;
    }

    Serial.println("❌ 未收到TTS音频数据");
    return false;
}

// 文本 Base64 编码
String base64EncodeText(String text) {
    uint8_t input[512];
    size_t len = text.length();
    if (len > 500) len = 500;  // 限制长度
    memcpy(input, text.c_str(), len);

    char output[1024];
    size_t outLen;
    mbedtls_base64_encode((uint8_t*)output, sizeof(output), &outLen, input, len);
    output[outLen] = '\0';
    return String(output);
}
