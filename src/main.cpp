#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

#define VERSION            "1.2.3"
#ifndef BOARD_LED_PIN
#define BOARD_LED_PIN      15
#endif
#define LED_PIN            BOARD_LED_PIN
#define MAX_DEVICES        5
#define MAX_CONNECTIONS    2
#define DEVICES_FILE       "/devices.bin"
#define SCAN_BLINK_MS      300
#define PULSE_PERIOD_MS    2000
#define IDLE_BLINK_ON_MS   1000
#define IDLE_BLINK_MS      2000
#define PAIRING_TIMEOUT_MS 60000

// Headpat Ecosystem UUID: B5A47D3E-8C21-4F68-92A0-1E3D5C7B9F04 (little-endian)
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
BLEClientUart  clientUart[MAX_CONNECTIONS];
uint16_t       connHandles[MAX_CONNECTIONS];
ble_gap_addr_t connPeers[MAX_CONNECTIONS];
uint8_t        connCount    = 0;

bool           scanning     = false;
bool           pairingMode  = false;
unsigned long  pairingStart = 0;
char           pendingName[20] = "";
unsigned long  lastBlink    = 0;
bool           ledState     = false;
bool           needScan     = false;

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

int findConnSlot(uint16_t conn_handle) {
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (connHandles[i] == conn_handle) return i;
  return -1;
}

int freeConnSlot() {
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (connHandles[i] == BLE_CONN_HANDLE_INVALID) return i;
  return -1;
}

bool isAlreadyConnected(const ble_gap_addr_t& addr) {
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (connHandles[i] != BLE_CONN_HANDLE_INVALID && addrEqual(addr, connPeers[i])) return true;
  return false;
}

void writeAll(uint8_t b) {
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (connHandles[i] != BLE_CONN_HANDLE_INVALID && clientUart[i].discovered())
      clientUart[i].write(b);
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
  writeAll(0xFD);
}

void startPairingMode() {
  pairingMode  = true;
  pairingStart = millis();
  Serial.println("[PAIR] Pairing mode ON (60s) — scanning for any BLE UART device...");
  if (!scanning && connCount < MAX_CONNECTIONS) {
    scanning = true;
    Bluefruit.Scanner.start(0);
  }
}

// ═══════════════════════════════════════════
//  SCAN
// ═══════════════════════════════════════════
void startScan() {
  if (connCount >= MAX_CONNECTIONS) return;
  if (scanning) return;
  Serial.println("[SCAN] Scanning for known devices...");
  scanning = true;
  Bluefruit.Scanner.start(0);
}

void stopScan() {
  scanning = false;
  Bluefruit.Scanner.stop();
}

// ═══════════════════════════════════════════
//  BLE SCAN CALLBACK
// ═══════════════════════════════════════════
void scan_callback(ble_gap_evt_adv_report_t* report) {
  if (isAlreadyConnected(report->peer_addr)) {
    Bluefruit.Scanner.resume();
    return;
  }

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
  int slot = freeConnSlot();
  if (slot < 0) {
    Bluefruit.disconnect(conn_handle);
    return;
  }

  connHandles[slot] = conn_handle;
  scanning = false;

  BLEConnection* conn = Bluefruit.Connection(conn_handle);
  connPeers[slot] = conn->getPeerAddr();
  connCount++;

  if (addDevice(connPeers[slot], pendingName)) {
    Serial.print("[SYS] New device saved: "); Serial.print(pendingName);
    Serial.print(" (#"); Serial.print(savedCount); Serial.println(")");
  }
  memset(pendingName, 0, sizeof(pendingName));

  Serial.print("[BLE] Connected (slot "); Serial.print(slot); Serial.print("): ");
  for (int i = 0; i < savedCount; i++) {
    if (addrEqual(savedDevices[i].addr, connPeers[slot])) {
      Serial.println(savedDevices[i].name[0] ? savedDevices[i].name : "?");
      break;
    }
  }
  Serial.print("[BLE] Active connections: "); Serial.print(connCount);
  Serial.print("/"); Serial.println(MAX_CONNECTIONS);

  bool ok = clientUart[slot].discover(conn_handle);
  if (ok) {
    bool notif = clientUart[slot].enableTXD();
    Serial.print("[UART] Slot "); Serial.print(slot);
    Serial.print(" Discovery: OK | enableTXD: "); Serial.println(notif ? "YES" : "NO");
  } else {
    Serial.print("[UART] Slot "); Serial.print(slot); Serial.println(" Discovery: FAIL");
  }

  if (connCount < MAX_CONNECTIONS) needScan = true;
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  int slot = findConnSlot(conn_handle);
  if (slot >= 0) {
    Serial.print("[BLE] Disconnected slot "); Serial.print(slot);
    Serial.print(". Reason="); Serial.println(reason);
    connHandles[slot] = BLE_CONN_HANDLE_INVALID;
    memset(&connPeers[slot], 0, sizeof(ble_gap_addr_t));
    connCount--;
  }
  needScan = true;
}

// ═══════════════════════════════════════════
//  UART RX (data from device)
// ═══════════════════════════════════════════
void uart_rx_callback(BLEClientUart& uart) {
  static String buf[MAX_CONNECTIONS];
  int slot = -1;
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (&uart == &clientUart[i]) { slot = i; break; }
  if (slot < 0) return;

  while (uart.available()) {
    char c = (char)uart.read();
    if (c == '\n') {
      buf[slot].trim();
      if (buf[slot].length() > 0) Serial.println(buf[slot]);
      buf[slot] = "";
    } else if (c != '\r') {
      buf[slot] += c;
    }
  }
}

// ═══════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════
void setup() {
  pinMode(LED_PIN, OUTPUT);

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }

  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000) delay(10);

  Serial.println("══════════════════════════════");
  Serial.print  ("  Headpat Dongle v"); Serial.println(VERSION);
  Serial.println("══════════════════════════════");

  loadDevices();

  Serial.println("[SYS] Commands: info, list, reboot, remove, clear, pair, dfu, uptime, meow");

  for (int i = 0; i < MAX_CONNECTIONS; i++)
    connHandles[i] = BLE_CONN_HANDLE_INVALID;

  Bluefruit.begin(0, MAX_CONNECTIONS);
  Bluefruit.setName("Headpat-Dongle");

  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    clientUart[i].begin();
    clientUart[i].setRxCallback(uart_rx_callback);
  }

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

  // ── Deferred scan restart ─────────────────
  if (needScan && !scanning) {
    needScan = false;
    startScan();
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
      Serial.println("Headpat Dongle v" VERSION);
      Serial.println("Board: nRF52840");
      Serial.println("SOC: nrf52840");
      Serial.print  ("Device address: "); printAddr(myAddr); Serial.println();
      Serial.print  ("Connected: ");      Serial.print(connCount); Serial.print("/"); Serial.println(MAX_CONNECTIONS);
      Serial.print  ("Pairing mode: ");   Serial.println(pairingMode ? "YES" : "NO");
      Serial.print  ("Saved devices: ");  Serial.println(savedCount);
      Serial.print  ("Uptime: ");         Serial.println(formatUptime());

    } else if (cmd == "list") {
      Serial.println("list");
      Serial.print("Active connections: "); Serial.print(connCount); Serial.print("/"); Serial.println(MAX_CONNECTIONS);
      Serial.print("Saved devices ("); Serial.print(savedCount); Serial.print("/"); Serial.print(MAX_DEVICES); Serial.println("):");
      if (savedCount == 0) {
        Serial.println("(none)");
      } else {
        for (int i = 0; i < savedCount; i++) {
          Serial.print("#"); Serial.print(i + 1); Serial.print(" ");
          Serial.print(savedDevices[i].name[0] ? savedDevices[i].name : "Unknown");
          Serial.print("  ");
          printAddr(savedDevices[i].addr);
          for (int j = 0; j < MAX_CONNECTIONS; j++) {
            if (connHandles[j] != BLE_CONN_HANDLE_INVALID && addrEqual(savedDevices[i].addr, connPeers[j])) {
              Serial.print("  *connected*");
            }
          }
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
      if (connCount == 0) {
        Serial.println("Error: not connected — no device to remove.");
      } else {
        // Remove all currently connected devices
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
          if (connHandles[i] != BLE_CONN_HANDLE_INVALID) {
            removeDevice(connPeers[i]);
            Bluefruit.disconnect(connHandles[i]);
          }
        }
        Serial.println("Device(s) removed.");
      }

    } else if (cmd == "clear") {
      Serial.println("clear");
      for (int i = 0; i < MAX_CONNECTIONS; i++)
        if (connHandles[i] != BLE_CONN_HANDLE_INVALID) Bluefruit.disconnect(connHandles[i]);
      savedCount = 0;
      memset(savedDevices, 0, sizeof(savedDevices));
      saveDevices();
      Serial.println("All saved devices removed.");

    } else if (cmd == "pairing" || cmd == "pair") {
      Serial.println("pairing");
      startPairingMode();
      writeAll(0xFE);
      if (connCount > 0) Serial.println("Pairing command sent to connected device(s).");

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

    } else if (cmd == "reqbat") {
      if (connCount > 0) {
        writeAll(0xFC);
        Serial.println("[REQBAT] Sent battery request");
      } else {
        Serial.println("[REQBAT] Not connected");
      }

    } else if (cmd == "reqver") {
      if (connCount > 0) {
        writeAll(0xFB);
        Serial.println("[REQVER] Sent version request");
      } else {
        Serial.println("[REQVER] Not connected");
      }

    } else if (cmd == "meow") {
      Serial.println("meow");
      Serial.println("(^=◕ᴥ◕=^)");
      Serial.println("Headpat says: purrrr...");

    } else if (cmd.startsWith("m:")) {
      if (connCount > 0) {
        uint8_t val = (uint8_t)strtol(cmd.substring(2).c_str(), NULL, 16);
        writeAll(val);
        Serial.print("[MOTOR] Sent: 0x"); Serial.println(val, HEX);
      } else {
        Serial.println("[ERROR] Not connected!");
      }
    }
  }

  // ── Poll UART from Headpat (backup to callback) ──
  static String rxBuf[MAX_CONNECTIONS];
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    if (connHandles[i] != BLE_CONN_HANDLE_INVALID && clientUart[i].discovered() && clientUart[i].available()) {
      while (clientUart[i].available()) {
        char c = (char)clientUart[i].read();
        if (c == '\n') {
          rxBuf[i].trim();
          if (rxBuf[i].length() > 0) Serial.println(rxBuf[i]);
          rxBuf[i] = "";
        } else if (c != '\r') {
          rxBuf[i] += c;
        }
      }
    }
  }

  // ── LED: off when connected ──────────────
  if (connCount > 0) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  // ── LED: breathing fade while in pairing mode ──
  if (pairingMode) {
    unsigned long phase = millis() % PULSE_PERIOD_MS;
    uint8_t brightness;
    if (phase < PULSE_PERIOD_MS / 2) {
      brightness = (uint8_t)(phase * 255UL / (PULSE_PERIOD_MS / 2));
    } else {
      brightness = (uint8_t)((PULSE_PERIOD_MS - phase) * 255UL / (PULSE_PERIOD_MS / 2));
    }
    analogWrite(LED_PIN, brightness);
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
