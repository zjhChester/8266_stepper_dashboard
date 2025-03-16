#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <AccelStepper.h>
#include <EEPROM.h>
#include <DNSServer.h>  // 添加DNS服务器支持

// 定义步进电机引脚
#define IN1 D1
#define IN2 D2
#define IN3 D3
#define IN4 D4

// 定义电机参数
#define STEPS_PER_REV 4096    // 28BYJ-48 一圈的步数
#define MAX_SPEED_RPM 15      // 最大RPM速度
#define MIN_SPEED_RPM 1       // 最小RPM速度
#define DEFAULT_ACCEL_FACTOR 2 // 加速度因子（加速度会是速度的2倍）

// 定义EEPROM地址
#define EEPROM_SIZE 512
#define WIFI_CONFIG_FLAG 0    // 1 byte
#define SSID_ADDRESS 1        // 32 bytes
#define PASS_ADDRESS 33       // 64 bytes
#define MODE_ADDRESS 100
#define ANGLE_ADDRESS 101
#define ROUNDS_ADDRESS 103
#define SPEED_ADDRESS 105

// AP模式的配置
const char* AP_SSID = "StepperConfig";
const char* AP_PASS = "";  // 移除密码
bool isAPMode = false;

AccelStepper stepper(AccelStepper::HALF4WIRE, IN1, IN3, IN2, IN4);

ESP8266WebServer server(80);

// WiFi配置结构
struct WiFiConfig {
  char ssid[32];
  char password[64];
  bool configured;
} wifiConfig;

// 默认配置
struct Config {
  bool isAngleMode;
  int angle;
  int rounds;
  int speed;
} config;

// 在全局变量区域添加
bool isDriverConnected = false;
unsigned long lastDriverCheckTime = 0;
const unsigned long DRIVER_CHECK_INTERVAL = 5000; // 每5秒检查一次
bool shouldReconnectWiFi = false;
unsigned long reconnectStartTime = 0;
const unsigned long RECONNECT_DELAY = 30000; // 30秒后关闭AP模式

// 添加驱动板连接检测函数
bool checkDriverConnection() {
  // 保存原始状态
  bool originalStates[4];
  originalStates[0] = digitalRead(IN1);
  originalStates[1] = digitalRead(IN2);
  originalStates[2] = digitalRead(IN3);
  originalStates[3] = digitalRead(IN4);
  
  // 测试每个引脚
  for(int pin : {IN1, IN2, IN3, IN4}) {
    digitalWrite(pin, HIGH);
    delayMicroseconds(100);
    if(digitalRead(pin) != HIGH) {
      // 恢复原始状态
      digitalWrite(IN1, originalStates[0]);
      digitalWrite(IN2, originalStates[1]);
      digitalWrite(IN3, originalStates[2]);
      digitalWrite(IN4, originalStates[3]);
      return false;
    }
    digitalWrite(pin, LOW);
    delayMicroseconds(100);
    if(digitalRead(pin) != LOW) {
      // 恢复原始状态
      digitalWrite(IN1, originalStates[0]);
      digitalWrite(IN2, originalStates[1]);
      digitalWrite(IN3, originalStates[2]);
      digitalWrite(IN4, originalStates[3]);
      return false;
    }
  }
  
  // 恢复原始状态
  digitalWrite(IN1, originalStates[0]);
  digitalWrite(IN2, originalStates[1]);
  digitalWrite(IN3, originalStates[2]);
  digitalWrite(IN4, originalStates[3]);
  
  return true;
}

// 添加禁用电机函数
void disableMotor() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void loadConfig() {
  config.isAngleMode = EEPROM.read(MODE_ADDRESS) == 1;
  config.angle = EEPROM.read(ANGLE_ADDRESS) | (EEPROM.read(ANGLE_ADDRESS + 1) << 8);
  config.rounds = EEPROM.read(ROUNDS_ADDRESS) | (EEPROM.read(ROUNDS_ADDRESS + 1) << 8);
  config.speed = EEPROM.read(SPEED_ADDRESS) | (EEPROM.read(SPEED_ADDRESS + 1) << 8);
  
  // 验证数值范围
  config.angle = constrain(config.angle, 0, 360);
  config.rounds = constrain(config.rounds, 1, 50);
  config.speed = constrain(config.speed, MIN_SPEED_RPM, MAX_SPEED_RPM);
}

void saveConfig() {
  EEPROM.write(MODE_ADDRESS, config.isAngleMode ? 1 : 0);
  EEPROM.write(ANGLE_ADDRESS, config.angle & 0xFF);
  EEPROM.write(ANGLE_ADDRESS + 1, (config.angle >> 8) & 0xFF);
  EEPROM.write(ROUNDS_ADDRESS, config.rounds & 0xFF);
  EEPROM.write(ROUNDS_ADDRESS + 1, (config.rounds >> 8) & 0xFF);
  EEPROM.write(SPEED_ADDRESS, config.speed & 0xFF);
  EEPROM.write(SPEED_ADDRESS + 1, (config.speed >> 8) & 0xFF);
  EEPROM.commit();
}

void loadWiFiConfig() {
  wifiConfig.configured = EEPROM.read(WIFI_CONFIG_FLAG) == 1;
  if (wifiConfig.configured) {
    for (int i = 0; i < 32; i++) {
      wifiConfig.ssid[i] = EEPROM.read(SSID_ADDRESS + i);
    }
    for (int i = 0; i < 64; i++) {
      wifiConfig.password[i] = EEPROM.read(PASS_ADDRESS + i);
    }
  }
}

void saveWiFiConfig() {
  EEPROM.write(WIFI_CONFIG_FLAG, 1);
  for (int i = 0; i < 32; i++) {
    EEPROM.write(SSID_ADDRESS + i, wifiConfig.ssid[i]);
  }
  for (int i = 0; i < 64; i++) {
    EEPROM.write(PASS_ADDRESS + i, wifiConfig.password[i]);
  }
  EEPROM.commit();
}

void clearWiFiConfig() {
  EEPROM.write(WIFI_CONFIG_FLAG, 0);
  EEPROM.commit();
  wifiConfig.configured = false;
}

// 添加DNS服务器配置
const byte DNS_PORT = 53;
DNSServer dnsServer;
const IPAddress apIP(192, 168, 4, 1);

void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  
  // 配置AP模式的IP地址
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, subnet);
  
  // 启动DNS服务器
  dnsServer.start(DNS_PORT, "*", apIP);
  
  isAPMode = true;
  Serial.println("AP模式已启动");
  Serial.print("AP IP地址: ");
  Serial.println(WiFi.softAPIP());
}

bool connectWiFi() {
  if (!wifiConfig.configured) {
    return false;
  }
  
  // 如果当前是AP模式，使用AP+STA模式尝试连接
  if (isAPMode) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!isAPMode) {  // 如果不是从AP模式连接的，则设置为纯STA模式
      WiFi.mode(WIFI_STA);
    }
    // 注意：如果是从AP模式连接的，我们保持AP+STA模式，直到计时器到期
    // 这样用户可以继续使用AP模式访问设备，同时也可以通过新的IP地址访问
    
    Serial.println("\nWiFi已连接");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  
  return false;
}

// 添加WiFi扫描函数
String getWiFiScanResult() {
  String result = "";
  
  WiFi.mode(WIFI_AP_STA);  // 设置为AP+STA模式以便扫描
  int n = WiFi.scanNetworks();
  
  if (n == 0) {
    result = "未找到WiFi网络";
  } else {
    for (int i = 0; i < n; ++i) {
      if (i > 0) result += ",";
      result += WiFi.SSID(i);
    }
  }
  
  return result;
}

void handleWiFiConfig() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("reset")) {
      clearWiFiConfig();
      server.send(200, "text/html; charset=UTF-8", "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body><h1>WiFi配置已重置</h1><p>设备将在3秒后重启...</p><script>setTimeout(function(){window.location.href='/';},3000);</script></body></html>");
      delay(3000);
      ESP.restart();
      return;
    }
    
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    
    if (ssid.length() > 0) {
      strncpy(wifiConfig.ssid, ssid.c_str(), sizeof(wifiConfig.ssid) - 1);
      strncpy(wifiConfig.password, password.c_str(), sizeof(wifiConfig.password) - 1);
      wifiConfig.configured = true;
      saveWiFiConfig();
      
      // 尝试连接新的WiFi
      WiFi.mode(WIFI_AP_STA);
      WiFi.begin(wifiConfig.ssid, wifiConfig.password);
      
      int attempts = 0;
      bool connected = false;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
      }
      
      String html = "<html><head>";
      html += "<meta charset='UTF-8'>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
      html += "<style>";
      html += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;text-align:center;}";
      html += ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
      html += "h1{color:#4CAF50;}";
      html += ".success{color:#4CAF50;font-weight:bold;}";
      html += ".error{color:#f44336;font-weight:bold;}";
      html += ".info-box{background:#e3f2fd;padding:15px;border-radius:5px;margin:15px 0;text-align:left;}";
      html += ".countdown{font-size:24px;font-weight:bold;margin:15px 0;}";
      html += ".btn{display:inline-block;background:#4CAF50;color:white;padding:10px 15px;text-decoration:none;border-radius:4px;margin:5px;font-weight:bold;cursor:pointer;}";
      html += ".ip-box{background:#f5f5f5;border:2px dashed #2196F3;border-radius:8px;padding:15px;margin:15px 0;text-align:center;}";
      html += ".ip-address{font-weight:bold;font-size:24px;color:#2196F3;margin:10px 0;display:block;}";
      html += ".control-url{font-weight:bold;font-size:16px;color:#4CAF50;margin:10px 0;word-break:break-all;}";
      html += ".note{font-size:14px;color:#f44336;font-weight:bold;margin-top:10px;}";
      html += "</style>";
      html += "</head><body>";
      html += "<div class='container'>";
      html += "<h1>WiFi配置已保存</h1>";
      
      if (connected) {
        String ipAddress = WiFi.localIP().toString();
        String controlUrl = "http://" + ipAddress + "/control";
        
        html += "<p class='success'>✓ 连接成功!</p>";
        html += "<div class='info-box'>";
        html += "<h2>连接信息</h2>";
        html += "<p><strong>网络名称：</strong>" + String(wifiConfig.ssid) + "</p>";
        
        html += "<div class='ip-box'>";
        html += "<p>请记住以下信息，设备重启后使用此地址访问控制面板：</p>";
        html += "<span class='ip-address'>" + ipAddress + "</span>";
        html += "<p>完整控制面板地址：</p>";
        html += "<span class='control-url'>" + controlUrl + "</span>";
        html += "<p class='note'>请手动记录此IP地址！</p>";
        html += "</div>";
        
        html += "<div style='margin:15px 0;'>";
        html += "<button class='btn' onclick='restartDevice()'>记住IP并重启设备</button>";
        html += "</div>";
        html += "</div>";
        
        // 添加JavaScript重启功能
        html += "<script>";
        html += "function restartDevice() {";
        html += "  if(confirm('确定已记住IP地址 " + ipAddress + " 并重启设备？')) {";
        html += "    window.location.href = '/restart';";
        html += "  }";
        html += "}";
        html += "</script>";
      } else {
        html += "<p class='error'>✗ 连接失败</p>";
        html += "<p>无法连接到WiFi网络，但配置已保存。</p>";
        html += "<p>设备将重启并尝试重新连接。</p>";
      }
      
      html += "<p>热点将在<span id='countdown'>30</span>秒后自动关闭</p>";
      html += "<p>您可以继续使用热点访问设备，或者使用新的IP地址</p>";
      html += "<script>";
      html += "let seconds = 30;";
      html += "const countdownEl = document.getElementById('countdown');";
      html += "const interval = setInterval(() => {";
      html += "  seconds--;";
      html += "  countdownEl.textContent = seconds;";
      html += "  if (seconds <= 0) {";
      html += "    clearInterval(interval);";
      html += "  }";
      html += "}, 1000);";
      html += "</script>";
      html += "</div></body></html>";
      
      server.send(200, "text/html; charset=UTF-8", html);
      
      // 设置延迟关闭AP模式
      shouldReconnectWiFi = true;
      reconnectStartTime = millis();
      return;
    }
  } else if (server.hasArg("scan")) {
    String networks = getWiFiScanResult();
    server.send(200, "text/plain; charset=UTF-8", networks);
    return;
  } else {
    String currentWiFiInfo = "";
    if (!isAPMode && WiFi.status() == WL_CONNECTED) {
      currentWiFiInfo = "<div class='current-wifi'>";
      currentWiFiInfo += "<h2>当前连接</h2>";
      currentWiFiInfo += "<p><strong>网络名称：</strong>" + String(wifiConfig.ssid) + "</p>";
      currentWiFiInfo += "<p><strong>IP地址：</strong>" + WiFi.localIP().toString() + "</p>";
      currentWiFiInfo += "<p><strong>信号强度：</strong>" + String(WiFi.RSSI()) + " dBm</p>";
      currentWiFiInfo += "</div>";
    }

    String html = "<html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;}";
    html += ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
    html += "h1,h2{color:#333;text-align:center;}";
    html += "h2{font-size:18px;margin:15px 0;}";
    html += ".info-box{background:#e8f5e9;padding:10px;border-radius:5px;margin:10px 0;text-align:center;}";
    html += ".notice-box{background:#fff3e0;padding:15px;border-radius:5px;margin:10px 0;}";
    html += ".warning-box{background:#ffebee;padding:15px;border-radius:5px;margin:10px 0;}";
    html += ".current-wifi{background:#e3f2fd;padding:15px;border-radius:5px;margin:10px 0;}";
    html += "input{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
    html += "button{background:#4CAF50;color:white;padding:10px;border:none;border-radius:4px;cursor:pointer;width:100%;margin:5px 0;}";
    html += "button:hover{background:#45a049;}";
    html += ".reset-btn{background:#f44336;}";
    html += ".reset-btn:hover{background:#d32f2f;}";
    html += ".network-list{max-height:200px;overflow-y:auto;margin:10px 0;}";
    html += ".network-item{padding:8px;margin:2px 0;background:#f5f5f5;border-radius:4px;cursor:pointer;}";
    html += ".network-item:hover{background:#e0e0e0;}";
    html += ".back-btn{background:#2196F3;margin-bottom:15px;}";
    html += ".back-btn:hover{background:#1976D2;}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<button class='back-btn' onclick='window.location.href=\"/\"'>返回控制面板</button>";
    html += "<h1>WiFi配置</h1>";
    html += currentWiFiInfo;
    html += "<div class='info-box'>";
    html += "<strong>配置热点信息</strong><br>";
    html += "名称: StepperConfig<br>";
    html += "无需密码<br>";
    html += "IP地址: 192.168.4.1";
    html += "</div>";
    html += "<div class='notice-box'>";
    html += "<strong>配置步骤：</strong><br>";
    html += "1. 连接到「StepperConfig」热点<br>";
    html += "2. 点击「扫描WiFi网络」按钮<br>";
    html += "3. 从列表中选择要连接的网络<br>";
    html += "4. 输入WiFi密码<br>";
    html += "5. 点击「保存并连接」按钮";
    html += "</div>";
    html += "<div class='warning-box'>";
    html += "<strong>故障排除：</strong><br>";
    html += "1. 如果未自动跳转配置页面，请手动访问：<br>http://192.168.4.1<br>";
    html += "2. 如果无法访问配置页面，请尝试：<br>";
    html += "   - 关闭手机移动数据<br>";
    html += "   - 重新连接热点<br>";
    html += "3. 如果连接失败，请检查：<br>";
    html += "   - WiFi名称和密码是否正确<br>";
    html += "   - 路由器是否工作正常<br>";
    html += "4. 如需重置WiFi配置，请点击下方重置按钮";
    html += "</div>";
    html += "<button onclick='scanWiFi()'>扫描WiFi网络</button>";
    html += "<div id='networkList' class='network-list'></div>";
    html += "<form method='post' id='wifiForm'>";
    html += "<label>WiFi名称:</label>";
    html += "<input type='text' name='ssid' id='ssidInput' required>";
    html += "<label>WiFi密码:</label>";
    html += "<input type='password' name='password'>";
    html += "<button type='submit'>保存并连接</button>";
    html += "</form>";
    html += "<form method='post' onsubmit='return confirmReset()'>";
    html += "<input type='hidden' name='reset' value='1'>";
    html += "<button type='submit' class='reset-btn'>重置WiFi配置</button>";
    html += "</form>";
    html += "</div>";
    html += "<script>";
    html += "function confirmReset() {";
    html += "  return confirm('确定要重置WiFi配置吗？设备将重启并清除所有WiFi设置。');";
    html += "}";
    html += "function scanWiFi() {";
    html += "  document.getElementById('networkList').innerHTML = '扫描中...';";
    html += "  fetch('/wifi?scan=1')";
    html += "    .then(response => response.text())";
    html += "    .then(data => {";
    html += "      const networks = data.split(',');";
    html += "      let html = '';";
    html += "      networks.forEach(network => {";
    html += "        if(network && network != '未找到WiFi网络') {";
    html += "          html += `<div class='network-item' onclick='selectNetwork(\"${network}\")'>${network}</div>`;";
    html += "        } else {";
    html += "          html = network;";
    html += "        }";
    html += "      });";
    html += "      document.getElementById('networkList').innerHTML = html;";
    html += "    })";
    html += "    .catch(error => {";
    html += "      document.getElementById('networkList').innerHTML = '扫描失败';";
    html += "    });";
    html += "}";
    html += "function selectNetwork(ssid) {";
    html += "  document.getElementById('ssidInput').value = ssid;";
    html += "}";
    html += "</script>";
    html += "</body></html>";
    server.send(200, "text/html; charset=UTF-8", html);
  }
}

void checkConfigButton() {
  // 移除此函数，不再需要
}

void handleHelp() {
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>使用说明</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;line-height:1.6;}";
  html += ".container{max-width:800px;margin:0 auto;background:white;padding:30px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  html += "h1,h2{color:#2c3e50;}";
  html += "h2{margin-top:30px;padding-bottom:10px;border-bottom:2px solid #eee;}";
  html += ".section{margin:20px 0;padding:20px;border-radius:8px;}";
  html += ".info{background:#e3f2fd;}";
  html += ".warning{background:#fff3e0;}";
  html += ".connection{background:#e8f5e9;}";
  html += ".troubleshoot{background:#ffebee;}";
  html += "table{width:100%;border-collapse:collapse;margin:15px 0;}";
  html += "th,td{padding:12px;text-align:left;border:1px solid #ddd;}";
  html += "th{background:#f5f5f5;}";
  html += ".back-btn{background:#2196F3;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;text-decoration:none;display:inline-block;margin-bottom:20px;}";
  html += ".back-btn:hover{background:#1976D2;}";
  html += "img{max-width:100%;height:auto;margin:15px 0;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<a href='/' class='back-btn'>返回控制面板</a>";
  html += "<h1>ESP8266步进电机控制器使用说明</h1>";
  
  // 设备信息部分
  html += "<div class='section info'>";
  html += "<h2>设备信息</h2>";
  html += "<table>";
  html += "<tr><th>项目</th><th>说明</th></tr>";
  html += "<tr><td>控制器型号</td><td>ESP8266 NodeMCU</td></tr>";
  html += "<tr><td>步进电机型号</td><td>28BYJ-48 (5V)</td></tr>";
  html += "<tr><td>驱动板型号</td><td>ULN2003驱动板</td></tr>";
  html += "<tr><td>工作电压</td><td>5V DC</td></tr>";
  html += "<tr><td>控制方式</td><td>Web界面控制</td></tr>";
  html += "</table>";
  html += "</div>";
  
  // 接线说明部分
  html += "<div class='section connection'>";
  html += "<h2>接线说明</h2>";
  html += "<table>";
  html += "<tr><th>ESP8266引脚</th><th>ULN2003驱动板</th><th>说明</th></tr>";
  html += "<tr><td>D1</td><td>IN1</td><td>步进电机控制信号1</td></tr>";
  html += "<tr><td>D2</td><td>IN2</td><td>步进电机控制信号2</td></tr>";
  html += "<tr><td>D3</td><td>IN3</td><td>步进电机控制信号3</td></tr>";
  html += "<tr><td>D4</td><td>IN4</td><td>步进电机控制信号4</td></tr>";
  html += "<tr><td>GND</td><td>GND</td><td>接地</td></tr>";
  html += "<tr><td>VU (5V)</td><td>VCC</td><td>电源正极</td></tr>";
  html += "</table>";
  html += "</div>";
  
  // 使用说明部分
  html += "<div class='section warning'>";
  html += "<h2>使用说明</h2>";
  html += "<h3>基本操作：</h3>";
  html += "<ol>";
  html += "<li>选择控制模式：";
  html += "<ul>";
  html += "<li>角度控制：可以精确控制转动角度（0-360度）</li>";
  html += "<li>圈数控制：可以控制完整圈数（1-50圈）</li>";
  html += "</ul></li>";
  html += "<li>调整转速：使用滑块或输入框设置转速（1-15 RPM）</li>";
  html += "<li>控制按钮：";
  html += "<ul>";
  html += "<li>正转：顺时针旋转</li>";
  html += "<li>反转：逆时针旋转</li>";
  html += "<li>停止：立即停止当前运动</li>";
  html += "</ul></li>";
  html += "<li>保存配置：点击「保存配置」按钮可以保存当前的控制参数</li>";
  html += "</ol>";
  html += "</div>";
  
  // 故障排除部分
  html += "<div class='section troubleshoot'>";
  html += "<h2>故障排除</h2>";
  html += "<table>";
  html += "<tr><th>问题现象</th><th>可能原因</th><th>解决方法</th></tr>";
  html += "<tr><td>电机不转动</td><td>接线错误或松动</td><td>检查所有连接是否正确且牢固</td></tr>";
  html += "<tr><td>电机转动不顺畅</td><td>速度设置过高</td><td>降低转速，建议从5RPM开始调试</td></tr>";
  html += "<tr><td>无法连接设备</td><td>WiFi配置问题</td><td>重置WiFi配置并重新设置</td></tr>";
  html += "<tr><td>转动角度不准</td><td>负载过重</td><td>减小负载或降低速度</td></tr>";
  html += "<tr><td>网页无响应</td><td>网络连接不稳定</td><td>刷新页面或重新连接WiFi</td></tr>";
  html += "</table>";
  html += "</div>";
  
  // 注意事项部分
  html += "<div class='section warning'>";
  html += "<h2>注意事项</h2>";
  html += "<ul>";
  html += "<li>请确保电源供电稳定，建议使用5V/1A以上的电源</li>";
  html += "<li>首次使用时，建议从低速开始测试</li>";
  html += "<li>如果电机发出异常声音，请立即停止并检查接线</li>";
  html += "<li>长时间运行时注意散热</li>";
  html += "<li>请勿在运行时触摸电机和驱动板</li>";
  html += "</ul>";
  html += "</div>";
  
  html += "</div></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void setup() {
  Serial.begin(115200);
  
  // 初始化步进电机引脚
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  disableMotor();
  
  // 检查驱动板连接
  isDriverConnected = checkDriverConnection();
  Serial.println("驱动板连接状态: " + String(isDriverConnected ? "已连接" : "未连接"));
  
  // 初始化EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();
  loadWiFiConfig();
  
  // 修改WiFi连接逻辑
  if (!connectWiFi()) {
    setupAP();
  } else {
    WiFi.mode(WIFI_STA);  // 如果直接连接成功，使用纯STA模式
  }
  
  // 初始化步进电机
  stepper.setMaxSpeed(1000);
  stepper.setAcceleration(500);
  
  // 配置Web服务器路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_ANY, handleControl);  // 修改为支持GET和POST
  server.on("/save", HTTP_GET, handleSave);
  server.on("/wifi", HTTP_ANY, handleWiFiConfig);
  server.on("/wifi-status", HTTP_GET, handleWiFiStatus);
  server.on("/help", HTTP_GET, handleHelp);  // 添加帮助页面路由
  server.on("/driver-status", HTTP_GET, handleDriverStatus);
  server.on("/restart", HTTP_GET, handleRestart);  // 添加重启设备路由
  
  // Captive Portal相关路由
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);
  
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain; charset=UTF-8", "");
  });
  
  server.begin();
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  
  // 处理WiFi重连和AP模式延迟关闭
  if (shouldReconnectWiFi && millis() - reconnectStartTime >= RECONNECT_DELAY) {
    shouldReconnectWiFi = false;
    Serial.println("延迟时间到，重启设备...");
    ESP.restart();
  }
  
  // 如果处于AP+STA模式且已连接WiFi，更新状态显示
  if (isAPMode && WiFi.status() == WL_CONNECTED) {
    // 已连接WiFi但仍处于AP模式，这是正常的过渡状态
    // 不需要做任何特殊处理，等待定时器到期后重启
  }
  
  if (stepper.distanceToGo() != 0) {
    stepper.run();
  } else {
    disableMotor();
  }
  
  // 定期检查驱动板连接状态
  unsigned long currentTime = millis();
  if (currentTime - lastDriverCheckTime >= DRIVER_CHECK_INTERVAL) {
    isDriverConnected = checkDriverConnection();
    lastDriverCheckTime = currentTime;
  }
}

// 添加新的处理WiFi状态的函数
void handleWiFiStatus() {
  String status = "{";
  status += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  status += "\"ssid\":\"" + String(wifiConfig.ssid) + "\",";
  status += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  status += "\"rssi\":" + String(WiFi.RSSI());
  status += "}";
  server.send(200, "application/json; charset=UTF-8", status);
}

void handleRoot() {
  // 只有在纯AP模式下才重定向到WiFi配置页面
  // 如果已经连接到WiFi（即使同时处于AP模式），则显示控制面板
  if (isAPMode && WiFi.status() != WL_CONNECTED) {
    server.sendHeader("Location", "/wifi");
    server.send(302, "text/plain; charset=UTF-8", "");
    return;
  }
  
  // 直接重定向到控制面板
  server.sendHeader("Location", "/control");
  server.send(302, "text/plain; charset=UTF-8", "");
}

// 添加控制面板处理函数
void handleControl() {
  if (server.method() == HTTP_GET && !server.hasArg("cmd")) {
    // 显示控制面板
    String html = "<html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>"
                "<title>步进电机控制面板</title>"
                "<style>"
                "  :root {"
                "    --primary-color: #2196F3;"
                "    --success-color: #4CAF50;"
                "    --warning-color: #FF9800;"
                "    --danger-color: #f44336;"
                "    --text-color: #2c3e50;"
                "    --bg-color: #f5f7fa;"
                "    --card-bg: #ffffff;"
                "  }"
                "  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }"
                "  body {"
                "    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
                "    background: var(--bg-color);"
                "    color: var(--text-color);"
                "    line-height: 1.6;"
                "    min-height: 100vh;"
                "  }"
                "  .page-container {"
                "    max-width: 800px;"
                "    margin: 0 auto;"
                "    padding: 16px;"
                "  }"
                "  .header {"
                "    background: var(--card-bg);"
                "    padding: 16px;"
                "    border-radius: 12px;"
                "    box-shadow: 0 2px 8px rgba(0,0,0,0.1);"
                "    margin-bottom: 16px;"
                "  }"
                "  .header-content {"
                "    display: flex;"
                "    justify-content: space-between;"
                "    align-items: center;"
                "    flex-wrap: wrap;"
                "    gap: 12px;"
                "  }"
                "  .header h1 {"
                "    font-size: 20px;"
                "    color: var(--text-color);"
                "    margin: 0;"
                "  }"
                "  .nav-buttons {"
                "    display: flex;"
                "    gap: 8px;"
                "  }"
                "  .nav-btn {"
                "    padding: 8px 12px;"
                "    border: none;"
                "    border-radius: 6px;"
                "    cursor: pointer;"
                "    font-size: 14px;"
                "    transition: all 0.3s ease;"
                "    min-width: 80px;"
                "    text-align: center;"
                "  }"
                "  .nav-btn.help {"
                "    background: var(--success-color);"
                "    color: white;"
                "  }"
                "  .nav-btn.wifi {"
                "    background: var(--primary-color);"
                "    color: white;"
                "  }"
                "  .control-panel {"
                "    background: var(--card-bg);"
                "    padding: 20px;"
                "    border-radius: 12px;"
                "    box-shadow: 0 2px 8px rgba(0,0,0,0.1);"
                "    margin-bottom: 16px;"
                "  }"
                "  .control-group {"
                "    margin-bottom: 20px;"
                "    padding: 16px;"
                "    background: #f8f9fa;"
                "    border-radius: 8px;"
                "  }"
                "  .control-group h3 {"
                "    color: var(--text-color);"
                "    margin-bottom: 12px;"
                "    font-size: 16px;"
                "  }"
                "  .radio-group {"
                "    display: flex;"
                "    gap: 16px;"
                "    margin: 12px 0;"
                "  }"
                "  .radio-option {"
                "    display: flex;"
                "    align-items: center;"
                "    gap: 8px;"
                "  }"
                "  .radio-option input[type='radio'] {"
                "    width: 20px;"
                "    height: 20px;"
                "  }"
                "  .slider-container {"
                "    display: flex;"
                "    flex-direction: column;"
                "    gap: 12px;"
                "    margin: 12px 0;"
                "  }"
                "  .slider-row {"
                "    display: flex;"
                "    align-items: center;"
                "    gap: 12px;"
                "  }"
                "  input[type='range'] {"
                "    flex: 1;"
                "    height: 20px;"
                "    border-radius: 10px;"
                "    background: #ddd;"
                "    outline: none;"
                "    -webkit-appearance: none;"
                "  }"
                "  input[type='range']::-webkit-slider-thumb {"
                "    -webkit-appearance: none;"
                "    width: 28px;"
                "    height: 28px;"
                "    background: var(--primary-color);"
                "    border-radius: 50%;"
                "    cursor: pointer;"
                "    box-shadow: 0 2px 4px rgba(0,0,0,0.2);"
                "  }"
                "  input[type='number'] {"
                "    width: 70px;"
                "    padding: 8px;"
                "    border: 1px solid #ddd;"
                "    border-radius: 6px;"
                "    text-align: center;"
                "    font-size: 16px;"
                "  }"
                "  .button-group {"
                "    display: grid;"
                "    grid-template-columns: repeat(3, 1fr);"
                "    gap: 12px;"
                "    margin-top: 20px;"
                "  }"
                "  .control-btn {"
                "    padding: 16px;"
                "    border: none;"
                "    border-radius: 8px;"
                "    color: white;"
                "    font-size: 16px;"
                "    cursor: pointer;"
                "    transition: all 0.3s ease;"
                "    min-height: 50px;"
                "  }"
                "  .control-btn:active {"
                "    transform: scale(0.95);"
                "  }"
                "  .control-btn.forward { background: var(--success-color); }"
                "  .control-btn.stop { background: var(--danger-color); }"
                "  .control-btn.reverse { background: var(--primary-color); }"
                "  .control-btn:disabled {"
                "    opacity: 0.5;"
                "    cursor: not-allowed;"
                "  }"
                "  .save-btn {"
                "    background: var(--warning-color);"
                "    color: white;"
                "    width: 100%;"
                "    padding: 16px;"
                "    border: none;"
                "    border-radius: 8px;"
                "    margin-top: 20px;"
                "    cursor: pointer;"
                "    font-size: 16px;"
                "    transition: all 0.3s ease;"
                "  }"
                "  .save-btn:active {"
                "    transform: scale(0.98);"
                "  }"
                "  .status-box {"
                "    background: var(--card-bg);"
                "    padding: 16px;"
                "    border-radius: 12px;"
                "    box-shadow: 0 2px 8px rgba(0,0,0,0.1);"
                "    margin-bottom: 16px;"
                "  }"
                "  .status-box h3 {"
                "    margin-bottom: 12px;"
                "    color: var(--text-color);"
                "    font-size: 16px;"
                "  }"
                "  .status-item {"
                "    display: flex;"
                "    justify-content: space-between;"
                "    padding: 8px 0;"
                "    border-bottom: 1px solid #eee;"
                "    font-size: 14px;"
                "  }"
                "  .status-item:last-child {"
                "    border-bottom: none;"
                "  }"
                "  @media (max-width: 480px) {"
                "    .button-group {"
                "      grid-template-columns: 1fr;"
                "    }"
                "    .header h1 {"
                "      font-size: 18px;"
                "    }"
                "    .nav-btn {"
                "      padding: 6px 10px;"
                "      font-size: 13px;"
                "    }"
                "    .control-group {"
                "      padding: 12px;"
                "    }"
                "    .control-btn {"
                "      padding: 14px;"
                "      font-size: 15px;"
                "    }"
                "  }"
                "</style>"
                "</head>"
                "<body>"
                "<div class='page-container'>"
                "  <div class='header'>"
                "    <div class='header-content'>"
                "      <h1>步进电机控制面板</h1>"
                "      <div class='nav-buttons'>"
                "        <button class='nav-btn help' onclick='window.location.href=\"/help\"'>使用说明</button>"
                "        <button class='nav-btn wifi' onclick='window.location.href=\"/wifi\"'>WiFi配置</button>"
                "      </div>"
                "    </div>"
                "  </div>"
                "  <div class='control-panel'>"
                "    <div class='control-group'>"
                "      <h3>控制模式</h3>"
                "      <div class='radio-group'>"
                "        <div class='radio-option'>"
                "          <input type='radio' id='angleMode' name='controlMode' value='angle'" + String(config.isAngleMode ? " checked" : "") + " onclick='switchMode(\"angle\")'>"
                "          <label for='angleMode'>角度控制</label>"
                "        </div>"
                "        <div class='radio-option'>"
                "          <input type='radio' id='roundMode' name='controlMode' value='round'" + String(!config.isAngleMode ? " checked" : "") + " onclick='switchMode(\"round\")'>"
                "          <label for='roundMode'>圈数控制</label>"
                "        </div>"
                "      </div>"
                "    </div>"
                "    <div class='control-group' id='angleControl'" + String(config.isAngleMode ? "" : " style='display:none'") + ">"
                "      <h3>旋转角度设置</h3>"
                "      <div class='slider-container'>"
                "        <input type='range' id='angle' min='0' max='360' value='" + String(config.angle) + "' oninput='updateAngle(this.value)'>"
                "        <div class='slider-row'>"
                "          <input type='number' id='angleValue' value='" + String(config.angle) + "' min='0' max='360' onchange='updateAngleSlider(this.value)'>"
                "          <span>度</span>"
                "        </div>"
                "      </div>"
                "    </div>"
                "    <div class='control-group' id='roundControl'" + String(!config.isAngleMode ? "" : " style='display:none'") + ">"
                "      <h3>圈数设置</h3>"
                "      <div class='slider-container'>"
                "        <input type='range' id='rounds' min='1' max='50' value='" + String(config.rounds) + "' oninput='updateRounds(this.value)'>"
                "        <div class='slider-row'>"
                "          <input type='number' id='roundsValue' value='" + String(config.rounds) + "' min='1' max='50' onchange='updateRoundsSlider(this.value)'>"
                "          <span>圈</span>"
                "        </div>"
                "      </div>"
                "    </div>"
                "    <div class='control-group'>"
                "      <h3>转速设置</h3>"
                "      <div class='slider-container'>"
                "        <input type='range' id='speed' min='1' max='15' value='" + String(config.speed) + "' oninput='updateSpeed(this.value)'>"
                "        <div class='slider-row'>"
                "          <input type='number' id='speedValue' value='" + String(config.speed) + "' min='1' max='15' onchange='updateSpeedSlider(this.value)'>"
                "          <span>RPM</span>"
                "        </div>"
                "      </div>"
                "    </div>"
                "    <div class='button-group'>"
                "      <button class='control-btn forward' onclick=\"sendCommand('forward')\">正转</button>"
                "      <button class='control-btn stop' onclick=\"sendCommand('stop')\">停止</button>"
                "      <button class='control-btn reverse' onclick=\"sendCommand('reverse')\">反转</button>"
                "    </div>"
                "    <button class='save-btn' onclick=\"saveConfig()\">保存配置</button>"
                "  </div>"
                "  <div class='status-box'>"
                "    <h3>设备状态</h3>"
                "    <div id='status' class='status-item'>状态: 就绪</div>"
                "    <div class='status-item'>"
                "      <span>控制模式:</span>"
                "      <span id='currentMode'>" + String(config.isAngleMode ? "角度控制" : "圈数控制") + "</span>"
                "    </div>"
                "    <div class='status-item'>"
                "      <span>当前设定:</span>"
                "      <span id='currentValue'>" + String(config.isAngleMode ? String(config.angle) + "度" : String(config.rounds) + "圈") + "</span>"
                "    </div>"
                "    <div class='status-item'>"
                "      <span>当前转速:</span>"
                "      <span id='currentSpeed'>" + String(config.speed) + " RPM</span>"
                "    </div>"
                "  </div>"
                "</div>"
                "<script>"
                "let currentSpeed = " + String(config.speed) + ";"
                "let currentRounds = " + String(config.rounds) + ";"
                "let currentAngle = " + String(config.angle) + ";"
                "let currentMode = '" + String(config.isAngleMode ? "angle" : "round") + "';"

                "function updateConfigDisplay() {"
                "  document.getElementById('currentMode').textContent = currentMode === 'angle' ? '角度控制' : '圈数控制';"
                "  document.getElementById('currentValue').textContent = currentMode === 'angle' ? `${currentAngle}度` : `${currentRounds}圈`;"
                "  document.getElementById('currentSpeed').textContent = `${currentSpeed} RPM`;"
                "}"

                "function switchMode(mode) {"
                "  currentMode = mode;"
                "  if (mode === 'angle') {"
                "    document.getElementById('angleControl').style.display = 'block';"
                "    document.getElementById('roundControl').style.display = 'none';"
                "  } else {"
                "    document.getElementById('angleControl').style.display = 'none';"
                "    document.getElementById('roundControl').style.display = 'block';"
                "  }"
                "  updateConfigDisplay();"
                "}"

                "function updateAngle(value) {"
                "  currentAngle = parseInt(value);"
                "  document.getElementById('angleValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function updateAngleSlider(value) {"
                "  value = Math.min(Math.max(parseInt(value) || 0, 0), 360);"
                "  currentAngle = value;"
                "  document.getElementById('angle').value = value;"
                "  document.getElementById('angleValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function updateRounds(value) {"
                "  currentRounds = parseInt(value);"
                "  document.getElementById('roundsValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function updateRoundsSlider(value) {"
                "  value = Math.min(Math.max(parseInt(value) || 1, 1), 50);"
                "  currentRounds = value;"
                "  document.getElementById('rounds').value = value;"
                "  document.getElementById('roundsValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function updateSpeed(value) {"
                "  currentSpeed = parseInt(value);"
                "  document.getElementById('speedValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function updateSpeedSlider(value) {"
                "  value = Math.min(Math.max(parseInt(value) || 1, 1), 15);"
                "  currentSpeed = value;"
                "  document.getElementById('speed').value = value;"
                "  document.getElementById('speedValue').value = value;"
                "  updateConfigDisplay();"
                "}"

                "function sendCommand(cmd) {"
                "  const statusEl = document.getElementById('status');"
                "  statusEl.textContent = '状态: 执行' + (cmd === 'forward' ? '正转' : cmd === 'reverse' ? '反转' : '停止') + '命令...';"
                "  const url = `/control?cmd=${cmd}&speed=${currentSpeed}&mode=${currentMode}&value=${currentMode === 'angle' ? currentAngle : currentRounds}`;"
                "  fetch(url)"
                "    .then(response => {"
                "      if(response.ok) {"
                "        statusEl.textContent = '状态: 命令执行成功';"
                "      } else {"
                "        statusEl.textContent = '状态: 命令执行失败';"
                "      }"
                "    })"
                "    .catch(error => {"
                "      statusEl.textContent = '状态: 连接错误';"
                "    });"
                "}"

                "function saveConfig() {"
                "  const statusEl = document.getElementById('status');"
                "  statusEl.textContent = '状态: 保存配置中...';"
                "  const url = `/save?mode=${currentMode}&angle=${currentAngle}&rounds=${currentRounds}&speed=${currentSpeed}`;"
                "  fetch(url)"
                "    .then(response => {"
                "      if(response.ok) {"
                "        statusEl.textContent = '状态: 配置保存成功';"
                "      } else {"
                "        statusEl.textContent = '状态: 配置保存失败';"
                "      }"
                "    })"
                "    .catch(error => {"
                "      statusEl.textContent = '状态: 连接错误';"
                "    });"
                "}"
                "</script>"
                "</body></html>";
    server.send(200, "text/html; charset=UTF-8", html);
    return;
  }
  
  // 处理控制命令
  String cmd = server.arg("cmd");
  int speed = server.arg("speed").toInt();
  String mode = server.arg("mode");
  int value = server.arg("value").toInt();
  
  // 限制速度范围
  speed = constrain(speed, MIN_SPEED_RPM, MAX_SPEED_RPM);
  
  // 计算实际的步进速度和加速度
  float stepsPerSecond = (speed * STEPS_PER_REV) / 60.0;  // 将RPM转换为每秒步数
  float acceleration = stepsPerSecond * DEFAULT_ACCEL_FACTOR;  // 加速度设为速度的2倍
  
  // 设置电机速度参数
  stepper.setMaxSpeed(stepsPerSecond);
  stepper.setAcceleration(acceleration);
  
  if (cmd == "stop") {
    stepper.stop();
    disableMotor();
  } else {
    int steps;
    if (mode == "angle") {
      // 角度转换为步数 (4096步/圈 * 角度/360度)
      value = constrain(value, 0, 360);
      steps = (STEPS_PER_REV * value) / 360;
    } else {
      // 圈数转换为步数
      value = constrain(value, 1, 50);
      steps = STEPS_PER_REV * value;
    }
    
    if (cmd == "forward") {
      stepper.move(steps);
    } else if (cmd == "reverse") {
      stepper.move(-steps);
    }
  }
  
  server.send(200, "text/plain; charset=UTF-8", "OK");
}

void handleSave() {
  config.isAngleMode = server.arg("mode") == "angle";
  config.angle = server.arg("angle").toInt();
  config.rounds = server.arg("rounds").toInt();
  config.speed = server.arg("speed").toInt();
  
  // 验证数值范围
  config.angle = constrain(config.angle, 0, 360);
  config.rounds = constrain(config.rounds, 1, 50);
  config.speed = constrain(config.speed, MIN_SPEED_RPM, MAX_SPEED_RPM);
  
  saveConfig();
  server.send(200, "text/plain; charset=UTF-8", "OK");
}

// 添加处理Captive Portal的函数
void handleCaptivePortal() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/wifi", true);
  server.send(302, "text/plain; charset=UTF-8", "");
}

// 添加获取驱动板状态的处理函数
void handleDriverStatus() {
  String status = "{\"connected\":" + String(isDriverConnected ? "true" : "false") + "}";
  server.send(200, "application/json; charset=UTF-8", status);
}

// 添加立即重启设备的处理函数
void handleRestart() {
  String html = "<html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f0f0;text-align:center;}";
  html += ".container{max-width:400px;margin:0 auto;background:white;padding:20px;border-radius:10px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  html += "h1{color:#4CAF50;}";
  html += ".info{color:#2196F3;font-weight:bold;}";
  html += ".success{color:#4CAF50;font-weight:bold;}";
  html += "</style>";
  
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>设备正在重启</h1>";
  
  if (isAPMode) {
    html += "<p class='info'>AP热点模式将关闭</p>";
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    html += "<p class='success'>WiFi已连接</p>";
    html += "<p>请使用您复制的IP地址访问控制面板</p>";
    html += "<p>IP地址: <strong>" + WiFi.localIP().toString() + "</strong></p>";
  } else {
    html += "<p>设备将尝试连接到已保存的WiFi网络</p>";
    html += "<p>请稍后使用路由器分配的IP地址访问设备</p>";
  }
  
  html += "</div></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);  // 给浏览器足够的时间接收响应
  ESP.restart();
}