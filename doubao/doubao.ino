/*
   名称: ESP32-S3 DeepSeek 测试
   功能: WiFi 连接 + DeepSeek API 调用
*/

// ESP32-S3 关键：把 Serial 重定向到 Serial0（UART0），否则串口监视器看不到输出
#if ARDUINO_USB_CDC_ON_BOOT
#define Serial Serial0
#endif

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ==================== WiFi 配置 ====================
const char* ssid = "tp-link";
const char* password = "qc13798425457";

// ==================== DeepSeek API 配置 ====================
const char* apiKey = "sk-1a60ba4b6fcd48ee8e6169175065bdf9";
const char* host = "api.deepseek.com";
const char* apiUrl = "/v1/chat/completions";

WiFiClientSecure client;

// ==================== 函数声明 ====================
String callDeepSeek(String prompt);

void setup() {
    Serial.begin(115200);

    // 等待串口监视器连接（ESP32-S3 原生 USB 必须）
    while (!Serial) {
        delay(10);
    }

    delay(1000);
    Serial.println("\n==========================================");
    Serial.println(" Doubao / DeepSeek 测试程序");
    Serial.println("==========================================");

    // ==================== 连接 WiFi ====================
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

    // 跳过证书验证（测试用）
    client.setInsecure();

    Serial.println("\n✅ 系统准备就绪！");
    Serial.println("==========================================\n");
}

void loop() {
    // 检测串口输入，收到回车后调用 DeepSeek
    if (Serial.available()) {
        String userInput = Serial.readStringUntil('\n');
        userInput.trim();

        if (userInput.length() > 0) {
            Serial.print("\n🤔 你说: ");
            Serial.println(userInput);

            String reply = callDeepSeek(userInput);

            Serial.print("🤖 DeepSeek: ");
            Serial.println(reply);
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

    if (!client.connect(host, 443)) {
        return "连接服务器失败";
    }
    Serial.println(" 已连接");

    // 构造请求 JSON
    StaticJsonDocument<512> doc;
    doc["model"] = "deepseek-chat";
    JsonArray messages = doc.createNestedArray("messages");
    JsonObject userMsg = messages.createNestedObject();
    userMsg["role"] = "user";
    userMsg["content"] = prompt;

    String requestBody;
    serializeJson(doc, requestBody);

    // 发送 HTTP POST 请求
    client.println(String("POST ") + apiUrl + " HTTP/1.1");
    client.println(String("Host: ") + host);
    client.println(String("Authorization: Bearer ") + apiKey);
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(requestBody.length());
    client.println();
    client.println(requestBody);

    // 等待响应
    unsigned long timeout = millis();
    while (!client.available()) {
        if (millis() - timeout > 15000) {
            return "请求超时";
        }
        delay(10);
    }

    // 读取状态行
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    Serial.print("状态: ");
    Serial.println(statusLine);

    if (statusLine.indexOf("200") == -1) {
        return "API 错误: " + statusLine;
    }

    // 跳过响应头，找到空行后的 body
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break;
    }

    // 等待 body 数据到达（给网络一点缓冲时间）
    delay(100);
    unsigned long bodyTimeout = millis();
    while (!client.available()) {
        if (millis() - bodyTimeout > 5000) {
            return "响应体等待超时";
        }
        delay(10);
    }

    // 读取完整响应 body（处理 chunked 编码）
    String responseBody = "";
    while (client.available()) {
        responseBody += client.readString();
    }
    client.stop();

    // 调试：打印原始响应（可注释掉）
    Serial.print("原始响应: ");
    Serial.println(responseBody.substring(0, 300));

    // 处理 chunked 编码：去掉开头的 chunk size（如 "214\r\n"）和结尾的 "0\r\n\r\n"
    int firstNewline = responseBody.indexOf('\n');
    if (firstNewline > 0 && firstNewline < 20) {
        // 跳过第一行（chunk size）
        responseBody = responseBody.substring(firstNewline + 1);
        // 去掉尾部的 chunk 结束标记
        int lastZero = responseBody.lastIndexOf("0\r\n\r\n");
        if (lastZero != -1) {
            responseBody = responseBody.substring(0, lastZero);
        }
        lastZero = responseBody.lastIndexOf("0\n\n");
        if (lastZero != -1) {
            responseBody = responseBody.substring(0, lastZero);
        }
    }

    responseBody.trim();

    // 解析 JSON 响应
    StaticJsonDocument<4096> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, responseBody);

    if (error) {
        Serial.print("JSON 错误: ");
        Serial.println(error.c_str());
        return "JSON 解析失败: " + String(error.c_str());
    }

    const char* reply = responseDoc["choices"][0]["message"]["content"];
    if (!reply) {
        return "响应中无内容";
    }
    return String(reply);
}
