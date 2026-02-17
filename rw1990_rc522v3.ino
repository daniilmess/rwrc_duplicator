#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EncButton.h>
#include <EEPROM.h>
#include <OneWire.h>

#define SCREEN_W  128
#define SCREEN_H   32
#define OLED_ADDR 0x3C          // если не работает → попробуй 0x3D
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

#define SS_PIN     10
#define RST_PIN     9
MFRC522 rfid(SS_PIN, RST_PIN);

#define OW_PIN      8           // RW1990 на D8 + pull-up 4.7k или 2.2k
OneWire ow(OW_PIN);

#define ENC_A       2
#define ENC_B       3
#define ENC_BTN     4
EncButton enc(ENC_A, ENC_B, ENC_BTN);

#define LED_Y       6
#define LED_G       7
#define BUZZ       A1

#define MAX_KEYS   10

enum KeyType {
  TYPE_RW1990 = 0,
  TYPE_RFID_13M = 1,
  TYPE_RFID_125K = 2,
};

struct KeyRec {
  uint8_t  type;      // 0 = RW1990, 1 = RFID 13.56MHz, 2 = RFID 125kHz
  uint8_t  uid[8];
  uint8_t  uidLen;    // Actual UID length (4-8 bytes)
  char     name[16];  // Key name
  bool     isMaster;  // True if this is a master key from PROGMEM
};

KeyRec keys[MAX_KEYS];
uint8_t keyCnt = 0;

// Master keys stored in PROGMEM
const KeyRec PROGMEM masterKeys[] = {
  {TYPE_RW1990, {0x01, 0xCA, 0xC9, 0xAF, 0x02, 0x00, 0x00, 0xC0}, 8, "home_78", true},
  {TYPE_RFID_13M, {0x04, 0xA1, 0xB2, 0xC3, 0x00, 0x00, 0x00, 0x00}, 4, "office_card", true},
  {TYPE_RFID_125K, {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x00, 0x00, 0x00}, 5, "garage_fob", true}
};
const uint8_t MASTER_KEYS_COUNT = sizeof(masterKeys) / sizeof(masterKeys[0]);

enum Mode {
  MAIN,
  READ_RW,
  READ_RF,
  LIST,
  READ_RESULT,
  SAVED_DETAIL,
  CONFIRM_DELETE,
  WRITE
};
Mode mode = MAIN;

int cursor = 0;
int selKey = 0;
bool deleteConfirm = false;

uint8_t tempBuf[8];
uint8_t tempTp = 0;
uint8_t tempUidLen = 0;

unsigned long tmStart = 0;
unsigned long lastBeepMs = 0;  // Track last beep time for WRITE mode
unsigned long lastTickMs = 0;   // Track last tick for scanning
unsigned long holdStartMs = 0;  // Track hold start for factory reset
unsigned long scanHoldStartMs = 0;  // Track hold start for scan exit
bool busy = false;
bool inScanMode = false;

// ────────────────────────────────────────────────
// RW1990 — жёсткая версия с паузой прерываний и устройств
// ────────────────────────────────────────────────

bool rw1990_read(uint8_t* buf) {
  ow.reset_search();  // Reset search state to ensure fresh device detection
  noInterrupts();  // пауза прерываний
  bool res = false;
  if (ow.search(buf)) {
    byte crc = ow.crc8(buf, 7);
    if (crc == buf[7]) res = true;
  } else {
    ow.reset_search();
  }
  interrupts();  // возврат прерываний
  if (res) {
    Serial.print(F("RW1990 read OK: "));
    for (uint8_t i = 0; i < 8; i++) {
      if (buf[i] < 16) Serial.print('0');
      Serial.print(buf[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
  } else {
    Serial.println(F("No RW1990 or CRC error"));
  }
  return res;
}

bool rw1990_write(const uint8_t* newID) {
  // Check if device is present before starting write
  uint8_t dummy[8];
  if (!rw1990_read(dummy)) {
    Serial.println(F("No device at start of write"));
    return false;
  }

  Serial.println(F("Starting write sequence..."));

  // Read current ID (0x33 command)
  ow.skip();
  ow.reset();
  ow.write(0x33);

  Serial.print(F("ID before write: "));
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = ow.read();
    Serial.print(b < 16 ? "0" : "");
    Serial.print(b, HEX);
    Serial.print(' ');
  }
  Serial.println();

  // Enter write mode (0xD1 command)
  ow.skip();
  ow.reset();
  ow.write(0xD1);

  // Send unlock pulse (60µs low)
  noInterrupts();
  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(60);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);
  interrupts();
  delay(10);  // Device setup time after unlock pulse

  // Write data command (0xD5)
  ow.skip();
  ow.reset();
  ow.write(0xD5);

  // Write all 8 bytes
  Serial.print(F("Writing bytes: "));
  for (uint8_t i = 0; i < 8; i++) {
    rw1990_write_byte(newID[i]);
    Serial.print('*');
  }
  Serial.println();

  // Finalize write with 0xD1 command
  ow.skip();
  ow.reset();
  ow.write(0xD1);

  // Final pulse to complete write
  noInterrupts();
  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(60);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);
  interrupts();

  // Wait for write to complete
  delay(200);

  // Single verification
  Serial.println(F("Verifying write..."));
  uint8_t check[8];
  bool success = false;
  
  if (rw1990_read(check)) {
    success = (memcmp(check, newID, 8) == 0);
    Serial.println(success ? F("Verify: MATCH OK") : F("Verify: Mismatch"));
  } else {
    Serial.println(F("Verify: No device"));
  }

  return success;
}

void rw1990_write_byte(uint8_t data) {
  for (uint8_t bit = 0; bit < 8; bit++) {
    // Critical section: only during pulse generation (microseconds)
    noInterrupts();
    if (data & 1) {
      // Write '1': 60µs pulse
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(60);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    } else {
      // Write '0': 5µs pulse
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(5);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    }
    interrupts();
    // 15ms delay allows interrupts - adequate for iButton protocol
    delay(15);
    data >>= 1;
  }
}

// ────────────────────────────────────────────────
// Музыка
// ────────────────────────────────────────────────

void playMusic() {
  int Ab4 = 415; // ~G#4
  int C5  = 523;
  int Eb5 = 622;
  int E5  = 659;

  tone(BUZZ, Ab4, 120); delay(160);
  tone(BUZZ, C5,  120); delay(160);
  tone(BUZZ, Eb5, 120); delay(160);
  tone(BUZZ, E5,  120); delay(160);
  tone(BUZZ, Eb5, 450); delay(180); // длинный Eb5
  tone(BUZZ, C5,  160); delay(160);
  tone(BUZZ, Ab4, 280); delay(400);

  noTone(BUZZ);
}

// ────────────────────────────────────────────────
// Звуки
// ────────────────────────────────────────────────

void toneBeep(int hz, int ms) {
  tone(BUZZ, hz, ms);
  delay(ms + 10);
}

void okBeep() {
  digitalWrite(LED_G, HIGH);
  toneBeep(1400, 100); delay(20);
  toneBeep(1900, 100);
  delay(600);
  digitalWrite(LED_G, LOW);
}

void errBeep() {
  digitalWrite(LED_Y, HIGH);
  toneBeep(1900, 100); delay(20);
  toneBeep(1400, 100);
  delay(1000);
  digitalWrite(LED_Y, LOW);
}

void tickBeep() {
  digitalWrite(LED_Y, HIGH);
  tone(BUZZ, 150, 30);  // 150Hz, 30ms - short Geiger counter click
  delay(30);
  digitalWrite(LED_Y, LOW);
  delay(470);  // Total 500ms cycle
}

// ────────────────────────────────────────────────
// RF Type Detection
// ────────────────────────────────────────────────

uint8_t detectRF() {
  // Try 13.56MHz first (MFRC522)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    return TYPE_RFID_13M;
  }
  
  // TODO: Add 125kHz detection if hardware is available
  // For now, return 0 if no RF detected
  return 255; // No RF detected
}

// ────────────────────────────────────────────────
// Master Keys Management
// ────────────────────────────────────────────────

void loadMasterKeys() {
  Serial.println(F("Loading master keys from PROGMEM..."));
  keyCnt = 0;
  for (uint8_t i = 0; i < MASTER_KEYS_COUNT && keyCnt < MAX_KEYS; i++) {
    KeyRec mk;
    memcpy_P(&mk, &masterKeys[i], sizeof(KeyRec));
    keys[keyCnt++] = mk;
  }
  Serial.print(F("Loaded "));
  Serial.print(keyCnt);
  Serial.println(F(" master keys"));
}

// ────────────────────────────────────────────────
 // EEPROM
// ────────────────────────────────────────────────

#define EEPROM_FIRST_BOOT_FLAG 0
#define EEPROM_KEY_COUNT 1
#define EEPROM_KEYS_START 2

void loadEEPROM() {
  // Check first boot flag
  uint8_t firstBootFlag = EEPROM.read(EEPROM_FIRST_BOOT_FLAG);
  
  if (firstBootFlag != 0x01) {
    // First boot - load master keys from PROGMEM
    Serial.println(F("First boot detected - loading master keys"));
    loadMasterKeys();
    saveEEPROM();
    
    // Set first boot flag
    EEPROM.update(EEPROM_FIRST_BOOT_FLAG, 0x01);
  } else {
    // Normal boot - load from EEPROM
    Serial.println(F("Loading keys from EEPROM"));
    keyCnt = EEPROM.read(EEPROM_KEY_COUNT);
    if (keyCnt > MAX_KEYS) keyCnt = 0;
    
    for (uint8_t i = 0; i < keyCnt; i++) {
      int addr = EEPROM_KEYS_START + i * sizeof(KeyRec);
      EEPROM.get(addr, keys[i]);
    }
  }
  
  Serial.print(F("Loaded "));
  Serial.print(keyCnt);
  Serial.println(F(" keys"));
}

void saveEEPROM() {
  EEPROM.update(EEPROM_KEY_COUNT, keyCnt);
  for (uint8_t i = 0; i < keyCnt; i++) {
    int addr = EEPROM_KEYS_START + i * sizeof(KeyRec);
    EEPROM.put(addr, keys[i]);
  }
}

bool addKey(uint8_t tp, const uint8_t* d, uint8_t len) {
  if (keyCnt >= MAX_KEYS) return false;
  keys[keyCnt].type = tp;
  memcpy(keys[keyCnt].uid, d, 8);
  keys[keyCnt].uidLen = len;
  keys[keyCnt].name[0] = '\0';  // Empty name for user keys
  keys[keyCnt].isMaster = false;
  keyCnt++;
  saveEEPROM();
  return true;
}

void factoryReset() {
  Serial.println(F("Factory reset initiated..."));
  
  // Clear first boot flag
  EEPROM.update(EEPROM_FIRST_BOOT_FLAG, 0x00);
  
  // Display message
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 8);
  display.println(F("RESETTING"));
  display.display();
  
  // Play indication beeps
  for (int i = 0; i < 5; i++) {
    okBeep();
    delay(200);
  }
  
  // Soft restart via watchdog or ASM reset
  asm volatile ("jmp 0");
}

// ────────────────────────────────────────────────
 // Дисплей
// ────────────────────────────────────────────────

void drawHeader(const char* txt) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(txt);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

void drawLogo() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(4, 4);
  display.println(F("DUPLICATOR"));
  display.setTextSize(1);
  display.setCursor(12, 24);
  display.println(F("RW1990 / RFID"));
  display.display();
}

void drawMain() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 4); display.println(cursor == 0 ? "> Read RW1990" : "  Read RW1990");
  display.setCursor(4, 14); display.println(cursor == 1 ? "> Read RF"   : "  Read RF");
  display.setCursor(4, 24); display.println(cursor == 2 ? "> Saved Keys"  : "  Saved Keys");
  display.display();
}

void drawList() {
  drawHeader("Saved Keys");
  int start = max(0, selKey - 1);
  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= keyCnt) break;
    display.setCursor(4, 12 + i * 8);
    display.print(idx == selKey ? ">" : " ");
    
    // Show key based on type and master status
    if (keys[idx].isMaster && keys[idx].name[0] != '\0') {
      // Master key - show type prefix and name
      if (keys[idx].type == TYPE_RW1990) display.print("RW ");
      else if (keys[idx].type == TYPE_RFID_13M) display.print("RF13 ");
      else if (keys[idx].type == TYPE_RFID_125K) display.print("RF125 ");
      display.print(keys[idx].name);
    } else {
      // User key - show type prefix and UID
      if (keys[idx].type == TYPE_RW1990) {
        display.print("RW ");
        // Skip family code (byte 0) and CRC (byte 7), show middle 6 bytes
        for (uint8_t j = 1; j < 7; j++) {
          if (keys[idx].uid[j] < 16) display.print('0');
          display.print(keys[idx].uid[j], HEX);
          if (j < 6 && j % 2 == 0) display.print(' ');
        }
      } else if (keys[idx].type == TYPE_RFID_13M) {
        display.print("RF13 ");
        for (uint8_t j = 0; j < keys[idx].uidLen && j < 4; j++) {
          if (keys[idx].uid[j] < 16) display.print('0');
          display.print(keys[idx].uid[j], HEX);
          if (j < keys[idx].uidLen - 1) display.print(' ');
        }
      } else if (keys[idx].type == TYPE_RFID_125K) {
        display.print("RF125 ");
        for (uint8_t j = 0; j < keys[idx].uidLen && j < 5; j++) {
          if (keys[idx].uid[j] < 16) display.print('0');
          display.print(keys[idx].uid[j], HEX);
          if (j < keys[idx].uidLen - 1) display.print(' ');
        }
      }
    }
  }
  display.display();
}

void drawSavedKeyDetail() {
  // Display header based on key type
  const char* header;
  if (keys[selKey].type == TYPE_RW1990) {
    header = "RW1990 ID";
  } else if (keys[selKey].type == TYPE_RFID_13M) {
    header = "RF 13.56 MHz";
  } else if (keys[selKey].type == TYPE_RFID_125K) {
    header = "RF 125 kHz";
  } else {
    header = "Key ID";
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(header);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  
  // Display key data
  display.setCursor(0, 12);
  
  if (keys[selKey].type == TYPE_RW1990) {
    // For RW1990: show bytes 1-6 only (skip Family byte 0 and CRC byte 7)
    for (uint8_t j = 1; j < 7; j++) {
      if (keys[selKey].uid[j] < 16) display.print('0');
      display.print(keys[selKey].uid[j], HEX);
      // Group bytes in pairs: CAC9 AF02 0000
      if (j < 6 && j % 2 == 1) display.print(' ');
    }
  } else {
    // For RF: show all bytes with spaces
    for (uint8_t j = 0; j < keys[selKey].uidLen && j < 8; j++) {
      if (keys[selKey].uid[j] < 16) display.print('0');
      display.print(keys[selKey].uid[j], HEX);
      if (j < keys[selKey].uidLen - 1) display.print(' ');
    }
  }
  
  // Display menu at bottom
  display.setCursor(0, 24);
  if (cursor == 0) {
    display.print("   [Write]  Delete");
  } else {
    display.print("    Write  [Delete]");
  }
  
  display.display();
}


void drawConfirmDelete() {
  drawHeader("Delete key?");
  display.setCursor(4, 14);
  display.println(deleteConfirm ? "> YES   NO" : "  YES > NO");
  display.display();
}

void showApply(const char* s) {
  drawHeader("Apply key");
  display.setCursor(0, 16);
  display.println(s);
  display.println("7 sec timeout");
  display.display();
}

void printUID(const uint8_t* d, uint8_t len) {
  // Display header based on type
  const char* header;
  if (tempTp == TYPE_RW1990) {
    header = "RW1990 ID";
  } else if (tempTp == TYPE_RFID_13M) {
    header = "RF 13.56 MHz";
  } else if (tempTp == TYPE_RFID_125K) {
    header = "RF 125 kHz";
  } else {
    header = "Key ID";
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(header);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  
  display.setCursor(0, 14);
  
  // Format based on key type
  if (tempTp == TYPE_RW1990 && len == 8) {
    // RW1990: 01 CAC9 AF02 0000 C0
    // Byte 0 (Family code) - standalone
    if (d[0] < 16) display.print('0');
    display.print(d[0], HEX);
    display.print(' ');
    
    // Bytes 1-2 (grouped)
    if (d[1] < 16) display.print('0');
    display.print(d[1], HEX);
    if (d[2] < 16) display.print('0');
    display.print(d[2], HEX);
    display.print(' ');
    
    // Bytes 3-4 (grouped)
    if (d[3] < 16) display.print('0');
    display.print(d[3], HEX);
    if (d[4] < 16) display.print('0');
    display.print(d[4], HEX);
    display.print(' ');
    
    // Bytes 5-6 (grouped)
    if (d[5] < 16) display.print('0');
    display.print(d[5], HEX);
    if (d[6] < 16) display.print('0');
    display.print(d[6], HEX);
    display.print(' ');
    
    // Byte 7 (CRC) - standalone
    if (d[7] < 16) display.print('0');
    display.print(d[7], HEX);
  } else {
    // RF cards: show all bytes with spaces
    for (uint8_t i = 0; i < len; i++) {
      if (d[i] < 16) display.print('0');
      display.print(d[i], HEX);
      if (i < len - 1) display.print(' ');
    }
  }
  
  display.display();
}

// ────────────────────────────────────────────────
 // SETUP
// ────────────────────────────────────────────────

void setup() {
  pinMode(LED_Y, OUTPUT);
  pinMode(LED_G, OUTPUT);
  digitalWrite(LED_Y, LOW);
  digitalWrite(LED_G, LOW);

  Serial.begin(115200);
  Serial.println(F("Duplicator started"));

  delay(500);

  // Initialize display first
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init failed! Try 0x3D"));
    while (1);
  }

  display.clearDisplay();
  display.display();
  delay(100);

  // Show logo
  drawLogo();

  // Initialize other peripherals
  SPI.begin();
  rfid.PCD_Init();

  // Play startup sound after logo display
  playMusic();

  delay(1500);

  enc.setTimeout(380);

  loadEEPROM();
  drawMain();
}

// ────────────────────────────────────────────────
 // LOOP
// ────────────────────────────────────────────────

void loop() {
  enc.tick();

  // Factory reset: hold encoder 15+ seconds from any mode
  if (enc.pressing()) {
    if (holdStartMs == 0) {
      holdStartMs = millis();
    } else if (millis() - holdStartMs >= 15000UL) {
      factoryReset();
    }
  } else {
    holdStartMs = 0;
  }

  // Remove yellow LED blink on encoder click (as per requirements)
  // We only use LED feedback during scanning and other specific states

  if (busy && !inScanMode) {
    digitalWrite(LED_Y, (millis() >> 9) & 1);
  } else if (!inScanMode) {
    digitalWrite(LED_Y, LOW);
  }

  switch (mode) {

    case MAIN:
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 3) % 3;
        drawMain();
      }
      if (enc.click()) {
        if (cursor == 0) { 
          mode = READ_RW; 
          tmStart = millis(); 
          lastTickMs = millis();
          inScanMode = true;
          busy = true;
          
          // Display scanning message
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 8);
          display.println(F("Scanning RW1990..."));
          display.println(F("Hold 2s to exit"));
          display.display();
        }
        if (cursor == 1) { 
          mode = READ_RF; 
          tmStart = millis(); 
          lastTickMs = millis();
          inScanMode = true;
          busy = true;
          
          // Display scanning message
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 8);
          display.println(F("Scanning RF..."));
          display.println(F("Hold 2s to exit"));
          display.display();
        }
        if (cursor == 2) { 
          mode = LIST; 
          selKey = 0; 
          drawList(); 
        }
      }
      break;

    case READ_RW:
      // Check for 2-second hold to exit
      if (enc.pressing()) {
        if (scanHoldStartMs == 0) {
          scanHoldStartMs = millis();
        } else if (millis() - scanHoldStartMs >= 2000UL) {
          mode = MAIN; 
          drawMain(); 
          busy = false; 
          inScanMode = false;
          scanHoldStartMs = 0;
          break;
        }
      } else {
        scanHoldStartMs = 0;
      }
      
      // Check for 15-second timeout
      if (millis() - tmStart > 15000UL) {
        mode = MAIN; 
        drawMain(); 
        busy = false; 
        inScanMode = false;
        break;
      }

      // Tick feedback every 500ms
      if (millis() - lastTickMs >= 500) {
        tickBeep();
        lastTickMs = millis();
      }

      // Try to read RW1990
      if (rw1990_read(tempBuf)) {
        tempTp = TYPE_RW1990;
        tempUidLen = 8;
        
        // Success feedback
        digitalWrite(LED_G, HIGH);
        okBeep();
        digitalWrite(LED_G, LOW);
        
        printUID(tempBuf, tempUidLen);
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
      }
      break;

    case READ_RF:
      // Check for 2-second hold to exit
      if (enc.pressing()) {
        if (scanHoldStartMs == 0) {
          scanHoldStartMs = millis();
        } else if (millis() - scanHoldStartMs >= 2000UL) {
          mode = MAIN; 
          drawMain(); 
          busy = false; 
          inScanMode = false;
          scanHoldStartMs = 0;
          break;
        }
      } else {
        scanHoldStartMs = 0;
      }
      
      // Check for 15-second timeout
      if (millis() - tmStart > 15000UL) {
        mode = MAIN; 
        drawMain(); 
        busy = false; 
        inScanMode = false;
        break;
      }

      // Tick feedback every 500ms
      if (millis() - lastTickMs >= 500) {
        tickBeep();
        lastTickMs = millis();
      }

      // Try to detect RF card
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        tempTp = TYPE_RFID_13M;  // Detected 13.56MHz
        tempUidLen = min(rfid.uid.size, (uint8_t)8);
        memcpy(tempBuf, rfid.uid.uidByte, tempUidLen);
        rfid.PICC_HaltA();
        
        // Success feedback
        digitalWrite(LED_G, HIGH);
        okBeep();
        digitalWrite(LED_G, LOW);
        
        printUID(tempBuf, tempUidLen);
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
      }
      
      // TODO: Add 125kHz detection if hardware available
      
      break;

    case READ_RESULT:
      // Display result for 3 seconds, then return to scanning
      if (millis() - tmStart > 3000UL) {
        // Return to appropriate scanning mode
        if (tempTp == TYPE_RW1990) {
          mode = READ_RW;
          tmStart = millis();  // Reset scan timeout
          lastTickMs = millis();
          inScanMode = true;
          
          // Display scanning message
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 8);
          display.println(F("Scanning RW1990..."));
          display.println(F("Hold 2s to exit"));
          display.display();
        } else {
          mode = READ_RF;
          tmStart = millis();  // Reset scan timeout
          lastTickMs = millis();
          inScanMode = true;
          
          // Display scanning message
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 8);
          display.println(F("Scanning RF..."));
          display.println(F("Hold 2s to exit"));
          display.display();
        }
        break;
      }
      
      // Allow user to save key during display
      if (enc.click()) {
        if (addKey(tempTp, tempBuf, tempUidLen)) okBeep();
        else errBeep();
        
        // Return to scanning after saving
        if (tempTp == TYPE_RW1990) {
          mode = READ_RW;
        } else {
          mode = READ_RF;
        }
        tmStart = millis();  // Reset scan timeout
        lastTickMs = millis();
        inScanMode = true;
        
        // Display scanning message
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 8);
        if (tempTp == TYPE_RW1990) {
          display.println(F("Scanning RW1990..."));
        } else {
          display.println(F("Scanning RF..."));
        }
        display.println(F("Hold 2s to exit"));
        display.display();
      }
      
      // Hold to exit to main menu
      if (enc.hold()) {
        mode = MAIN; 
        drawMain(); 
        busy = false;
        inScanMode = false;
      }
      break;

    case LIST:
      if (enc.turn()) {
        selKey += enc.dir();
        selKey = constrain(selKey, 0, (int)keyCnt - 1);
        drawList();
      }
      if (enc.click()) {
        cursor = 0;
        drawSavedKeyDetail();
        mode = SAVED_DETAIL;
      }
      if (enc.hold()) {
        mode = MAIN; drawMain();
      }
      break;

    case SAVED_DETAIL:
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 2) % 2;
        drawSavedKeyDetail();
      }
      if (enc.click()) {
        if (cursor == 0) {  // Write
          tempTp = keys[selKey].type;
          tempUidLen = keys[selKey].uidLen;
          memcpy(tempBuf, keys[selKey].uid, 8);
          mode = WRITE;
          tmStart = millis();
          lastBeepMs = 0;  // Reset beep tracking
          busy = true;
        } else {            // Delete
          deleteConfirm = false;
          drawConfirmDelete();
          mode = CONFIRM_DELETE;
        }
      }
      if (enc.hold()) {
        mode = LIST; drawList();
      }
      break;

    case CONFIRM_DELETE:
      if (enc.turn()) {
        deleteConfirm = !deleteConfirm;
        drawConfirmDelete();
      }
      if (enc.click()) {
        if (deleteConfirm) {
          if (keyCnt > 0) {
            for (uint8_t i = selKey; i < keyCnt - 1; i++) {
              keys[i] = keys[i + 1];
            }
            keyCnt--;
            saveEEPROM();
            okBeep();
            if (keyCnt == 0) {
              mode = MAIN; drawMain();
            } else {
              selKey = constrain(selKey, 0, keyCnt - 1);
              mode = LIST; drawList();
            }
          }
        } else {
          mode = SAVED_DETAIL;
          drawSavedKeyDetail();
        }
      }
      if (enc.hold()) {
        mode = SAVED_DETAIL;
        drawSavedKeyDetail();
      }
      break;

    case WRITE:
      // This case implements a waiting loop that repeats via the main loop()
      // until a device is detected or timeout occurs (7 seconds)
      {
        const char* msg;
        if (tempTp == TYPE_RW1990) msg = "RW1990 blank";
        else if (tempTp == TYPE_RFID_13M) msg = "RF13 magic card";
        else if (tempTp == TYPE_RFID_125K) msg = "RF125 magic card";
        else msg = "Device";
        showApply(msg);
      }

      // Waiting loop for key/card to be applied
      {
        unsigned long now = millis();
        unsigned long elapsed = now - tmStart;
        
        // Check for timeout
        if (elapsed > 7000UL) {
          digitalWrite(LED_Y, LOW);  // Ensure LED is off on timeout
          mode = LIST; drawList(); busy = false; break;
        }
        
        // Blink yellow LED (toggle every 250ms for visible blinking)
        if ((elapsed % 500) < 250) {
          digitalWrite(LED_Y, HIGH);
        } else {
          digitalWrite(LED_Y, LOW);
        }
        
        // Beep once per second
        if (now - lastBeepMs >= 1000) {
          toneBeep(1000, 50);
          lastBeepMs = now;
        }
        
        // Check for key/card presence
        bool devicePresent = false;
        if (tempTp == TYPE_RW1990) {
          // RW1990: check if device is present
          uint8_t presenceCheckBuf[8];
          devicePresent = rw1990_read(presenceCheckBuf);
        } else {
          // RFID: check if card is present
          devicePresent = rfid.PICC_IsNewCardPresent();
        }
        
        // If no device detected yet, stay in waiting loop by breaking here
        // The main loop() will re-enter this WRITE case on the next iteration
        if (!devicePresent) {
          break;
        }
        
        // Device detected! Turn off LED and proceed with write
        digitalWrite(LED_Y, LOW);
      }
      
      // Perform write operation
      bool res = false;

      if (tempTp == TYPE_RW1990) {
        res = rw1990_write(tempBuf);
      } else {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
          res = (memcmp(rfid.uid.uidByte, tempBuf, min(rfid.uid.size, (size_t)8)) == 0);
          rfid.PICC_HaltA();
        }
      }

      display.clearDisplay();
      display.setCursor(0, 8);
      if (res) {
        okBeep();
        if (tempTp == TYPE_RW1990) display.println("RW1990 Written OK");
        else if (tempTp == TYPE_RFID_13M) display.println("RF13 Written OK");
        else if (tempTp == TYPE_RFID_125K) display.println("RF125 Written OK");
        display.println("Check: PASS");
      } else {
        errBeep();
        display.println("Write failed!");
        display.println("Check: FAIL");
      }
      display.display();
      delay(2200);

      mode = LIST; drawList(); busy = false;
      break;
  }
}
