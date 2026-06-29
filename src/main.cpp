#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

#define VERSION          "1.0.0"
#define LED_PIN          15
#define MAX_DEVICES      5
#define DEVICES_FILE     "/devices.bin"
#define SCAN_BLINK_MS    300
#define PULSE_PERIOD_MS  1500
#define IDLE_BLINK_ON_MS 1000
#define IDLE_BLINK_MS    2000
#define PAIRING_TIMEOUT_MS 60000

// HeatPett Ecosystem UUID: B5A47D3E-8C21-4F68-92A0-1E3D5C7B9F04 (little-endian)
static const uint8_t ECOSYSTEM_UUID[16] = {
  0x04, 0x9F, 0x7B, 0x5C, 0x3D, 0x1E, 0xA0, 0x92,
  0x68, 0x4F, 0x21, 0x8C, 0x3E, 0x7D, 0xA4, 0xB5
};

// ═══════════════════════════════════════════
//  SAVED DEVICE LIST
// ═══════════════════════════════════════════
struct SavedDevice {
  ble_gap_addr_t addr;
  char           name[20];
};

SavedDevice savedDevices[MAX_DEVICES];
uint8_t     savedCount = 0;

// ═══════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════
BLEClientUart  clientUart;
bool           connected        = false;
bool           scanning         = false;
bool           pairingMode      = false;
unsigned long  pairingStart     = 0;
ble_gap_addr_t currentPeer      = {0};
uint16_t       currentConnHandle = BLE_CONN_HANDLE_INVALID;
char           pendingName[20]  = "";
unsigned long  lastBlink        = 0;
bool           ledState         = false;

// ═══════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════
String formatUptime() {
  unsigned long s = millis() / 1000;
  unsigned long m = s / 60;
  unsigned long h = m / 60;
  char buf[16];
  sprintf(buf, "%02lu:%02lu:%02lu", h, m % 60, s % 60);
  return String(buf);
}

bool addrEqual(const ble_gap_addr_t& a, const ble_gap_addr_t& b) {
  return memcmp(a.addr, b.addr, 6) == 0;
}

void printAddr(const ble_gap_addr_t& addr) {
  for (int i = 5; i >= 0; i--) {
    if (i < 5) Serial.print(":");
    if (addr.addr[i] < 0x10) Serial.print("0");
    Serial.print(addr.addr[i], HEX);
  }
}

// ═══════════════════════════════════════════
//  FLASH PERSISTENCE
// ═══════════════════════════════════════════
void loadDevices() {
  if (!InternalFS.begin()) {
    Serial.println("[SYS] Flash mount failed, formatting...");
    InternalFS.format();
    if (!InternalFS.begin()) { Serial.println("[SYS] Flash failed."); return; }
  }
  File f(InternalFS);
  if (f.open(DEVICES_FILE, FILE_O_READ)) {
    f.read(&savedCount, 1);
    if (savedCount > MAX_DEVICES) savedCount = 0;
    f.read(savedDevices, savedCount * sizeof(SavedDevice));
    f.close();
    Serial.print("[SYS] Loaded "); Serial.print(savedCount); Serial.println(" device(s).");
  }
}

void saveDevices() {
  InternalFS.remove(DEVICES_FILE);
  File f(InternalFS);
  if (f.open(DEVICES_FILE, FILE_O_WRITE)) {
    f.write(&savedCount, 1);
    f.write((uint8_t*)savedDevices, savedCount * sizeof(SavedDevice));
    f.close();
  }
}

bool addDevice(const ble_gap_addr_t& addr, const char* name) {
  for (int i = 0; i < savedCount; i++) {
    if (addrEqual(savedDevices[i].addr, addr)) return false;
  }
  if (savedCount >= MAX_DEVICES) return false;
  savedDevices[savedCount].addr = addr;
  strncpy(savedDevices[savedCount].name, name, 19);
  savedDevices[savedCount].name[19] = '\0';
  savedCount++;
  saveDevices();
  return true;
}

bool removeDevice(const ble_gap_addr_t& addr) {
  for (int i = 0; i < savedCount; i++) {
    if (addrEqual(savedDevices[i].addr, addr)) {
      for (int j = i; j < savedCount - 1; j++) savedDevices[j] = savedDevices[j + 1];
      memset(&savedDevices[--savedCount], 0, sizeof(SavedDevice));
      saveDevices();
      return true;
    }
  }
  return false;
}

// ═══════════════════════════════════════════
//  PAIRING MODE
// ═══════════════════════════════════════════
void stopPairingMode() {
  pairingMode = false;
  Serial.println("[PAIR] Pairing mode OFF");
  if (connected && clientUart.discovered()) clientUart.write((uint8_t)0xFD);
}

void startPairingMode() {
  pairingMode  = true;
  pairingStart = millis();
  Serial.println("[PAIR] Pairing mode ON (60s) — scanning for any BLE UART device...");
  if (!scanning && !connected) {
    scanning = true;
    Bluefruit.Scanner.start(0);
  }
}

// ═══════════════════════════════════════════
//  SCAN
// ═══════════════════════════════════════════
void startScan() {
  if (connected) return;
  Serial.println("[SCAN] Scanning for known devices...");
  scanning = true;
  Bluefruit.Scanner.start(0);
}

void stopScan() {
  scanning = false;
  Bluefruit.Scanner.stop();
  digitalWrite(LED_PIN, LOW);
}

// ═══════════════════════════════════════════
//  BLE SCAN CALLBACK
// ═══════════════════════════════════════════
void scan_callback(ble_gap_evt_adv_report_t* report) {
  for (int i = 0; i < savedCount; i++) {
    if (addrEqual(report->peer_addr, savedDevices[i].addr)) {
      Serial.print("[SCAN] Known device found: ");
      Serial.println(savedDevices[i].name[0] ? savedDevices[i].name : "?");
      strncpy(pendingName, savedDevices[i].name, 19);
      stopScan();
      Bluefruit.Central.connect(report);
      return;
    }
  }

  if (pairingMode) {
    uint8_t uuidBuf[16] = {0};
    bool hasEcoUUID = false;

    if (Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE, uuidBuf, 16)) {
      hasEcoUUID = (memcmp(uuidBuf, ECOSYSTEM_UUID, 16) == 0);
    }
    if (!hasEcoUUID && Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_MORE_AVAILABLE, uuidBuf, 16)) {
      hasEcoUUID = (memcmp(uuidBuf, ECOSYSTEM_UUID, 16) == 0);
    }

    if (hasEcoUUID) {
      uint8_t nameBuf[20] = {0};
      if (!Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, nameBuf, sizeof(nameBuf))) {
        Bluefruit.Scanner.parseReportByType(report, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, nameBuf, sizeof(nameBuf));
      }
      strncpy(pendingName, nameBuf[0] ? (char*)nameBuf : "Unknown", 19);
      pendingName[19] = '\0';

      Serial.print("[PAIR] New device found: "); Serial.println(pendingName);
      stopScan();
      Bluefruit.Central.connect(report);
      return;
    }
  }

  Bluefruit.Scanner.resume();
}

// ═══════════════════════════════════════════
//  BLE CONNECT / DISCONNECT
// ═══════════════════════════════════════════
void connect_callback(uint16_t conn_handle) {
  connected = true;
  scanning  = false;
  digitalWrite(LED_PIN, HIGH);

  currentConnHandle = conn_handle;
  BLEConnection* conn = Bluefruit.Connection(conn_handle);
  currentPeer = conn->getPeerAddr();

  if (addDevice(currentPeer, pendingName)) {
    Serial.print("[SYS] New device saved: "); Serial.print(pendingName);
    Serial.print(" (#"); Serial.print(savedCount); Serial.println(")");
  }
  memset(pendingName, 0, sizeof(pendingName));

  Serial.print("[BLE] Connected: ");
  for (int i = 0; i < savedCount; i++) {
    if (addrEqual(savedDevices[i].addr, currentPeer)) {
      Serial.println(savedDevices[i].name[0] ? savedDevices[i].name : "?");
      break;
    }
  }

  clientUart.discover(conn_handle);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  Serial.print("[BLE] Disconnected. Reason="); Serial.println(reason);
  connected         = false;
  currentConnHandle = BLE_CONN_HANDLE_INVALID;
  memset(&currentPeer, 0, sizeof(currentPeer));
  digitalWrite(LED_PIN, LOW);
  startScan();
}

// ═══════════════════════════════════════════
//  UART RX (data from device)
// ═══════════════════════════════════════════
void uart_rx_callback(BLEClientUart& uart) {
  while (uart.available()) {
    uint8_t bat = uart.read();
    Serial.print("[BAT] "); Serial.print(bat); Serial.println("%");
  }
}

// ═══════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════
void setup() {
  pinMode(LED_PIN, OUTPUT);

  // 3 blinks = firmware started
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }

  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) delay(10);

  Serial.println("══════════════════════════════");
  Serial.print  ("  HeatPett Dongle v"); Serial.println(VERSION);
  Serial.println("══════════════════════════════");

  loadDevices();

  Serial.println("[SYS] Commands: info, list, reboot, remove, clear, pairing, dfu, uptime, meow");

  Bluefruit.begin(0, 1);
  Bluefruit.setName("HeatPett-Dongle");

  clientUart.begin();
  clientUart.setRxCallback(uart_rx_callback);

  Bluefruit.Central.setConnectCallback(connect_callback);
  Bluefruit.Central.setDisconnectCallback(disconnect_callback);

  Bluefruit.Scanner.setRxCallback(scan_callback);
  Bluefruit.Scanner.restartOnDisconnect(false);
  Bluefruit.Scanner.filterRssi(-80);
  Bluefruit.Scanner.useActiveScan(true);

  Serial.println("[SYS] Ready!");
  Serial.println("══════════════════════════════");

  startScan();
}

// ═══════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════
void loop() {

  // ── Pairing mode timeout ─────────────────
  if (pairingMode && (millis() - pairingStart >= PAIRING_TIMEOUT_MS)) {
    stopPairingMode();
  }

  // ── Serial Commands ──────────────────────
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "scan") {
      startScan();

    } else if (cmd == "stop") {
      stopScan();

    } else if (cmd == "info") {
      ble_gap_addr_t myAddr = Bluefruit.getAddr();
      Serial.println("info");
      Serial.println("HeatPett Dongle v" VERSION);
      Serial.println("Board: nRF52840");
      Serial.println("SOC: nrf52840");
      Serial.print  ("Device address: "); printAddr(myAddr); Serial.println();
      Serial.print  ("Connected: ");      Serial.println(connected ? "YES" : "NO");
      Serial.print  ("Pairing mode: ");   Serial.println(pairingMode ? "YES" : "NO");
      Serial.print  ("Saved devices: ");  Serial.println(savedCount);
      Serial.print  ("Uptime: ");         Serial.println(formatUptime());

    } else if (cmd == "list") {
      Serial.println("list");
      Serial.print("Saved devices ("); Serial.print(savedCount); Serial.print("/"); Serial.print(MAX_DEVICES); Serial.println("):");
      if (savedCount == 0) {
        Serial.println("(none)");
      } else {
        for (int i = 0; i < savedCount; i++) {
          Serial.print("#"); Serial.print(i + 1); Serial.print(" ");
          Serial.print(savedDevices[i].name[0] ? savedDevices[i].name : "Unknown");
          Serial.print("  ");
          printAddr(savedDevices[i].addr);
          if (connected && addrEqual(savedDevices[i].addr, currentPeer)) Serial.print("  *connected*");
          Serial.println();
        }
      }

    } else if (cmd == "reboot") {
      Serial.println("reboot");
      Serial.println("Rebooting...");
      delay(200);
      NVIC_SystemReset();

    } else if (cmd == "remove") {
      Serial.println("remove");
      if (!connected) {
        Serial.println("Error: not connected — no device to remove.");
      } else if (removeDevice(currentPeer)) {
        Serial.println("Device removed.");
        Bluefruit.disconnect(currentConnHandle);
      } else {
        Serial.println("Error: device not in saved list.");
      }

    } else if (cmd == "clear") {
      Serial.println("clear");
      if (connected) Bluefruit.disconnect(currentConnHandle);
      savedCount = 0;
      memset(savedDevices, 0, sizeof(savedDevices));
      saveDevices();
      Serial.println("All saved devices removed.");

    } else if (cmd == "pairing" || cmd == "pair") {
      Serial.println("pairing");
      startPairingMode();
      if (connected && clientUart.discovered()) {
        clientUart.write((uint8_t)0xFE);
        Serial.println("Pairing command sent to connected device.");
      }

    } else if (cmd == "exit") {
      Serial.println("exit");
      stopPairingMode();

    } else if (cmd == "dfu") {
      Serial.println("dfu");
      Serial.println("Entering UF2 bootloader — drag firmware.uf2 onto the drive...");
      delay(200);
      NRF_POWER->GPREGRET = 0x57;
      NVIC_SystemReset();

    } else if (cmd == "uptime") {
      Serial.println("uptime");
      Serial.print("Uptime: "); Serial.println(formatUptime());

    } else if (cmd == "meow") {
      Serial.println("meow");
      Serial.println("(^=◕ᴥ◕=^)");
      Serial.println("HeatPett says: purrrr...");

    } else if (cmd.startsWith("m:")) {
      if (connected && clientUart.discovered()) {
        uint8_t val = (uint8_t)strtol(cmd.substring(2).c_str(), NULL, 16);
        clientUart.write(val);
        Serial.print("[MOTOR] Sent: 0x"); Serial.println(val, HEX);
      } else {
        Serial.println("[ERROR] Not connected!");
      }
    }
  }

  // ── LED: off when connected ──────────────
  if (connected) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  // ── LED: slow pulse (on/off) while in pairing mode ──
  if (pairingMode) {
    unsigned long now    = millis();
    unsigned long period = PULSE_PERIOD_MS;
    unsigned long phase  = now % period;
    digitalWrite(LED_PIN, phase < period / 2 ? HIGH : LOW);
    return;
  }

  // ── LED: fast blink while scanning ───────
  if (scanning) {
    unsigned long now = millis();
    if (now - lastBlink >= SCAN_BLINK_MS) {
      lastBlink = now;
      ledState  = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
    return;
  }

  // ── LED: slow blink when idle ─────────────
  unsigned long now    = millis();
  unsigned long period = IDLE_BLINK_ON_MS + IDLE_BLINK_MS;
  unsigned long phase  = now % period;
  digitalWrite(LED_PIN, phase < IDLE_BLINK_ON_MS ? HIGH : LOW);
}
