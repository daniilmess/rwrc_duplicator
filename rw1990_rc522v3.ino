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
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

#define SS_PIN     10
#define RST_PIN     9
MFRC522 rfid(SS_PIN, RST_PIN);

#define OW_PIN      8
OneWire ow(OW_PIN);

#define ENC_A       2
#define ENC_B       3
#define ENC_BTN     4
EncButton enc(ENC_A, ENC_B, ENC_BTN);

#define LED_Y       6
#define LED_G       7
#define BUZZ       A1

#define MAX_KEYS   10
#define RW1990_UID_SIZE 8



enum KeyType {
  TYPE_RW1990 = 0,
  TYPE_RFID_13M = 1,
  TYPE_RFID_125K = 2,
};

struct KeyRec {
  uint8_t  type;
  uint8_t  uid[RW1990_UID_SIZE];
  uint8_t  uidLen;
  char     name[16];
  bool     isMaster;
};

KeyRec keys[MAX_KEYS];
uint8_t keyCnt = 0;

const KeyRec PROGMEM masterKeys[] = {
  {TYPE_RW1990, {0x01, 0xCA, 0xC9, 0xAF, 0x02, 0x00, 0x00, 0xC0}, RW1990_UID_SIZE, "home_78", true},
  {TYPE_RFID_13M, {0x04, 0xA1, 0xB2, 0xC3, 0x00, 0x00, 0x00, 0x00}, 4, "office_card", true},
  {TYPE_RFID_125K, {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x00, 0x00, 0x00}, 5, "garage_fob", true},
  {TYPE_RW1990, {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F}, RW1990_UID_SIZE, "RW_erase", true}
};
const uint8_t MASTER_KEYS_COUNT = sizeof(masterKeys) / sizeof(masterKeys[0]);

enum Mode {
  MAIN,
  READ_KEY,
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

uint8_t oldID[RW1990_UID_SIZE];
uint8_t newID[RW1990_UID_SIZE];
uint8_t tempBuf[RW1990_UID_SIZE];
uint8_t tempTp = 0;
uint8_t tempUidLen = 0;

unsigned long tmStart = 0;
unsigned long lastBeepMs = 0;  // Track last beep time for WRITE mode
unsigned long lastTickMs = 0;   // Track last tick for scanning
unsigned long holdStartMs = 0;  // Track hold start for factory reset
unsigned long scanHoldStartMs = 0;  // Track hold start for scan exit
bool busy = false;
bool inScanMode = false;


// Helper function to check Family/CRC errors and return error message
// Validates RW1990 family byte (buf[0]) and CRC checksum (buf[7])
// Returns: Error status string ("OK", "Family:ERR", "CRC:ERR", or "Family:ERR CRC:ERR")
const char* rw1990_check_errors(const uint8_t* buf) {
  byte crc = ow.crc8(buf, 7);
  bool familyOk = (buf[0] == 0x01);
  bool crcOk = (crc == buf[7]);
  
  if (!familyOk && !crcOk) {
    return "Family:ERR CRC:ERR";
  } else if (!familyOk) {
    return "Family:ERR";
  } else if (!crcOk) {
    return "CRC:ERR";
  } else {
    return "OK";
  }
}

// Read RW1990 key and output diagnostics to serial
// Parameters:
//   buf: Buffer to store 8-byte UID
// Returns:
//   true if key is found (regardless of Family/CRC errors)
//   false if no key is present
// Note: Always outputs diagnostic info to Serial including error status
bool rw1990_read(uint8_t* buf, bool silent = false) {
  ow.reset_search();
  bool found = ow.search(buf);
  
  if (!found) {
    if (!silent) Serial.println(F("RW:NODEV"));
    return false;
  }
  
  // Output bytes ALWAYS
  Serial.print(F("RW:"));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    if (buf[i] < 16) Serial.print('0');
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.print(F("|"));
  
  // Output error status
  Serial.println(rw1990_check_errors(buf));
  
  // Always return true if key is found (regardless of errors)
  return true;
}

// Unified function: read RW1990 key and display with error status
// Parameters:
//   buf: Buffer to store 8-byte UID
//   clearAndDraw: true to clear screen and draw header, false to add to existing display
// Returns:
//   true if key is found and displayed
//   false if no key is present
// Display: Shows UID at line 14, error status at line 24
bool rw1990_read_and_display(uint8_t* buf, bool clearAndDraw, bool silent = false) {
  if (!rw1990_read(buf, silent)) {
    return false;  // No key found
  }
  
  // Key found - display it with error status
  if (clearAndDraw) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("RW1990 ID"));
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  }
  
  // Display UID
  display.setCursor(0, 14);
  formatUID(TYPE_RW1990, buf, RW1990_UID_SIZE);
  
  // Display error status on next line
  display.setCursor(0, 24);
  display.print(F("| "));
  const char* errorStatus = rw1990_check_errors(buf);
  display.print(errorStatus);
  
  // Play beep based on error status
  if (strcmp(errorStatus, "OK") == 0) {
    okBeep();
  } else {
    errBeep();
  }
  
  display.display();
  return true;
}

bool rw1990_write(const uint8_t* id) {
  uint8_t dummy[RW1990_UID_SIZE];
  // Check if device present (will output diagnostics, may return false for broken keys)
  ow.reset_search();
  bool deviceFound = ow.search(dummy);
  
  if (!deviceFound) {
    Serial.println(F("RW:NODEV"));
    return false;
  }
  
  // Device present, proceed with write (works even for broken keys)
  Serial.println(F("RW:WR"));

  // Read old ID first using 0x33 command (works with all keys, required for broken keys to write successfully)
  ow.skip();
  ow.reset();
  ow.write(0x33);
  
  // Read and store old ID into global oldID buffer
  Serial.print(F("ID: "));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    oldID[i] = ow.read();
    Serial.print(oldID[i] < 16 ? "0" : "");
    Serial.print(oldID[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  // Debug: show newID and tempBuf
  Serial.print(F("newID:"));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    Serial.print(newID[i] < 16 ? "0" : "");
    Serial.print(newID[i], HEX);
    Serial.print(' ');
  }
  Serial.println();
  Serial.print(F("tempBuf:"));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    Serial.print(tempBuf[i] < 16 ? "0" : "");
    Serial.print(tempBuf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  // Now proceed with write sequence
  ow.skip();
  ow.reset();
  ow.write(0xD1);

  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(60);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);
  delay(10);

  ow.skip();
  ow.reset();
  ow.write(0xD5);

  Serial.print(F("WR: "));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    rw1990_write_byte(id[i]);
    Serial.print('*');
  }
  Serial.println();

  ow.skip();
  ow.reset();
  ow.write(0xD1);

  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(10);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);

  delay(200);

  Serial.println(F("VRF"));
  bool success = false;
  
  if (rw1990_read(oldID)) {
    success = (memcmp(oldID, id, RW1990_UID_SIZE) == 0);
    Serial.println(success ? F("VRF:OK") : F("VRF:FAIL"));
  } else {
    Serial.println(F("VRF:NODEV"));
  }

  return success;
}

void rw1990_write_byte(uint8_t data) {
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (data & 1) {
      // Bit 1 → 60 microsecond pulse
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(60);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    } else {
      // Bit 0 → 1 microsecond pulse (very short!)
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(1);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    }
    delay(10);  // Interval between bits
    data >>= 1;
  }
}

// Write UID to MIFARE blocks 0, 1, 2 (sector 0 UID blocks) using default key.
// Card must already be selected (rfid.uid valid). Authenticates sector 0,
// writes UID bytes and BCC, verifies by read-back of block 0.
// Returns true on success.
bool rfid_mifare_write(const uint8_t* data, uint8_t dataLen) {
  Serial.print(F("RF:WR UID:"));
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  // Authenticate sector 0 (blocks 0-2 contain the UID)
  MFRC522::StatusCode authStatus = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, 0, &key, &(rfid.uid));
  if (authStatus != MFRC522::STATUS_OK) {
    Serial.println(F("AUTH:FAIL"));
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  // Calculate BCC byte (XOR of UID bytes)
  uint8_t bcc = 0;
  for (byte i = 0; i < dataLen; i++) bcc ^= data[i];

  // Write block 0: UID bytes 0-3
  byte writeBlock0[16] = {0};
  memcpy(writeBlock0, data, min(dataLen, (uint8_t)4));
  if (rfid.MIFARE_Write(0, writeBlock0, 16) != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  // Write block 1: UID bytes 4-6 + BCC byte
  byte writeBlock1[16] = {0};
  if (dataLen > 4) {
    memcpy(writeBlock1, data + 4, min((uint8_t)(dataLen - 4), (uint8_t)3));
  }
  writeBlock1[3] = bcc;
  if (rfid.MIFARE_Write(1, writeBlock1, 16) != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  // Write block 2: cascade tag byte
  byte writeBlock2[16] = {0};
  writeBlock2[0] = 0x88;
  if (rfid.MIFARE_Write(2, writeBlock2, 16) != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  // Read back block 0 for verification
  byte readBuf[18];
  byte readLen = sizeof(readBuf);
  MFRC522::StatusCode readStatus = rfid.MIFARE_Read(0, readBuf, &readLen);
  bool success = (readStatus == MFRC522::STATUS_OK) &&
                 (memcmp(readBuf, writeBlock0, min(dataLen, (uint8_t)4)) == 0);
  Serial.println(success ? F("RF:VRF:OK") : F("RF:VRF:FAIL"));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return success;
}

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


void loadMasterKeys() {
  Serial.println(F("MKEYS:LOAD"));
  keyCnt = 0;
  for (uint8_t i = 0; i < MASTER_KEYS_COUNT && keyCnt < MAX_KEYS; i++) {
    KeyRec mk;
    memcpy_P(&mk, &masterKeys[i], sizeof(KeyRec));
    keys[keyCnt++] = mk;
  }
  Serial.print(F("CNT:"));
  Serial.println(keyCnt);
}


#define EEPROM_FIRST_BOOT_FLAG 0
#define EEPROM_KEY_COUNT 1
#define EEPROM_KEYS_START 2

void loadEEPROM() {
  uint8_t firstBootFlag = EEPROM.read(EEPROM_FIRST_BOOT_FLAG);
  
  if (firstBootFlag != 0x01) {
    Serial.print(F("BOOT:FIRST CNT:"));
    Serial.println(MASTER_KEYS_COUNT);
    loadMasterKeys();
    saveEEPROM();
    EEPROM.update(EEPROM_FIRST_BOOT_FLAG, 0x01);
  } else {
    Serial.println(F("EEPROM:LOAD"));
    keyCnt = EEPROM.read(EEPROM_KEY_COUNT);
    if (keyCnt > MAX_KEYS) keyCnt = 0;
    
    for (uint8_t i = 0; i < keyCnt; i++) {
      int addr = EEPROM_KEYS_START + i * sizeof(KeyRec);
      EEPROM.get(addr, keys[i]);
    }
  }
  
  Serial.print(F("CNT:"));
  Serial.println(keyCnt);
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
  memcpy(keys[keyCnt].uid, d, RW1990_UID_SIZE);
  keys[keyCnt].uidLen = len;
  keys[keyCnt].name[0] = '\0';
  keys[keyCnt].isMaster = false;
  keyCnt++;
  saveEEPROM();
  return true;
}

void factoryReset() {
  Serial.println(F("HARDRESET"));
  
  EEPROM.update(EEPROM_FIRST_BOOT_FLAG, 0x00);
  
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 8);
  display.println(F("RESET"));
  display.display();
  
  for (int i = 0; i < 3; i++) {
    okBeep();
    delay(150);
  }
  
  asm volatile ("jmp 0");
}

void drawHeader(const char* txt) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(txt);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}




void drawMain() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 2); display.println(cursor == 0 ? "> Read Key" : "  Read Key");
  display.setCursor(4, 10); display.println(cursor == 1 ? "> Keys"  : "  Keys");
  display.display();
}

void drawList() {
  drawHeader("Keys");
  int start = max(0, selKey - 1);
  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= keyCnt) break;
    display.setCursor(4, 12 + i * 8);
    display.print(idx == selKey ? ">" : " ");
    
    if (keys[idx].isMaster && keys[idx].name[0] != '\0') {
      if (keys[idx].type == TYPE_RW1990) display.print("RW ");
      else if (keys[idx].type == TYPE_RFID_13M) display.print("RF13 ");
      else if (keys[idx].type == TYPE_RFID_125K) display.print("RF125 ");
      display.print(keys[idx].name);
    } else {
      if (keys[idx].type == TYPE_RW1990) {
        display.print("RW ");
        for (uint8_t j = 1; j < 7; j++) {
          printHex(keys[idx].uid[j]);
          if (j < 6 && j % 2 == 0) display.print(' ');
        }
      } else if (keys[idx].type == TYPE_RFID_13M) {
        display.print("RF13 ");
        for (uint8_t j = 0; j < keys[idx].uidLen && j < 4; j++) {
          printHex(keys[idx].uid[j]);
          if (j < keys[idx].uidLen - 1) display.print(' ');
        }
      } else if (keys[idx].type == TYPE_RFID_125K) {
        display.print("RF125 ");
        for (uint8_t j = 0; j < keys[idx].uidLen && j < 5; j++) {
          printHex(keys[idx].uid[j]);
          if (j < keys[idx].uidLen - 1) display.print(' ');
        }
      }
    }
  }
  display.display();
}

void drawSavedKeyDetail() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(getKeyTypeStr(keys[selKey].type));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 12);
  
  if (keys[selKey].type == TYPE_RW1990) {
    for (uint8_t j = 1; j < 7; j++) {
      printHex(keys[selKey].uid[j]);
      if (j < 6 && j % 2 == 1) display.print(' ');
    }
  } else {
    for (uint8_t j = 0; j < keys[selKey].uidLen && j < RW1990_UID_SIZE; j++) {
      printHex(keys[selKey].uid[j]);
      if (j < keys[selKey].uidLen - 1) display.print(' ');
    }
  }
  
  display.setCursor(0, 24);
  if (cursor == 0) {
    display.print("   > Write  Delete");
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

void showScanning(const __FlashStringHelper* msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 8);
  display.println(msg);
  display.println(F("Hold 2s exit"));
  display.display();
}

// Helper function to print hex byte
void printHex(uint8_t b) {
  if (b < 16) display.print('0');
  display.print(b, HEX);
}

const __FlashStringHelper* getKeyTypeStr(uint8_t type) {
  if (type == TYPE_RW1990) return F("RW1990 ID");
  if (type == TYPE_RFID_13M) return F("RF 13.56 MHz");
  if (type == TYPE_RFID_125K) return F("RF 125 kHz");
  return F("Key ID");
}

void formatUID(uint8_t type, const uint8_t* uid, uint8_t uidLen) {
  if (type == TYPE_RW1990 && uidLen == 8) {
    printHex(uid[0]); display.print(' ');
    printHex(uid[1]); printHex(uid[2]); display.print(' ');
    printHex(uid[3]); printHex(uid[4]); display.print(' ');
    printHex(uid[5]); printHex(uid[6]); display.print(' ');
    printHex(uid[7]);
  } else {
    for (uint8_t i = 0; i < uidLen; i++) {
      printHex(uid[i]);
      if (i < uidLen - 1) display.print(' ');
    }
  }
}

void displayKeyUID(uint8_t type, const uint8_t* uid, uint8_t uidLen, bool clearAndDraw) {
  if (clearAndDraw) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(getKeyTypeStr(type));
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  }
  display.setCursor(0, 14);
  formatUID(type, uid, uidLen);
  display.display();
}



void setup() {
  pinMode(LED_Y, OUTPUT);
  pinMode(LED_G, OUTPUT);
  digitalWrite(LED_Y, LOW);
  digitalWrite(LED_G, LOW);

  Serial.begin(115200);
  Serial.println(F("START"));

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED:ERR"));
    while (1);
  }

  display.clearDisplay();
  display.display();

  SPI.begin();
  rfid.PCD_Init();

  okBeep();

  enc.setTimeout(380);

  loadEEPROM();
  drawMain();
}

void loop() {
  enc.tick();

  if (enc.pressing()) {
    if (holdStartMs == 0) {
      holdStartMs = millis();
    } else if (millis() - holdStartMs >= 15000UL) {
      factoryReset();
    }
  } else {
    holdStartMs = 0;
  }

  if (busy && !inScanMode) {
    digitalWrite(LED_Y, (millis() >> 9) & 1);
  } else if (!inScanMode) {
    digitalWrite(LED_Y, LOW);
  }

  switch (mode) {

    case MAIN:
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 2) % 2;
        drawMain();
      }
      if (enc.click()) {
        if (cursor == 0) { 
          mode = READ_KEY; 
          Serial.println(F("READ_MODE"));
          tmStart = millis(); 
          lastTickMs = millis();
          inScanMode = true;
          busy = true;
          showScanning(F("Scan Key"));
        }
        if (cursor == 1) { 
          mode = LIST; 
          selKey = 0; 
          drawList(); 
        }
      }
      break;

    case READ_KEY: {

      if (enc.pressing()) {
        if (scanHoldStartMs == 0) {
          scanHoldStartMs = millis();
        } else if (millis() - scanHoldStartMs >= 1500UL) {
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
      

      if (millis() - tmStart > 15000UL) {
        mode = MAIN; 
        drawMain(); 
        busy = false; 
        inScanMode = false;
        break;
      }


      if (millis() - lastTickMs >= 500) {
        tickBeep();
        lastTickMs = millis();
      }

      // Try to read RW1990 first
      if (rw1990_read_and_display(tempBuf, true, true)) {
        tempTp = TYPE_RW1990;
        tempUidLen = RW1990_UID_SIZE;
        
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
        break;
      }

      // If no RW1990, try RF card
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        tempUidLen = min(rfid.uid.size, (uint8_t)RW1990_UID_SIZE);
        memcpy(tempBuf, rfid.uid.uidByte, tempUidLen);
        rfid.PICC_HaltA();
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println(F("RF 13.56 MHz"));
        display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
        display.setCursor(0, 14);
        formatUID(TYPE_RFID_13M, tempBuf, tempUidLen);
        display.display();
        okBeep();
        Serial.print(F("RF:UID:"));
        for (byte i = 0; i < tempUidLen; i++) {
          if (tempBuf[i] < 0x10) Serial.print('0');
          Serial.print(tempBuf[i], HEX);
        }
        Serial.println();
        tempTp = TYPE_RFID_13M;
        
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
      }
      
      break;
    }

    case READ_RESULT:
      // Display result for 3 seconds, then return to scanning
      if (millis() - tmStart > 3000UL) {
        mode = READ_KEY;
        tmStart = millis();  // Reset scan timeout
        lastTickMs = millis();
        inScanMode = true;
        showScanning(F("Scan Key"));
        break;
      }
      
      if (enc.click()) {
        if (addKey(tempTp, tempBuf, tempUidLen)) okBeep();
        else errBeep();
        
        mode = READ_KEY;
        tmStart = millis();  // Reset scan timeout
        lastTickMs = millis();
        inScanMode = true;
        showScanning(F("Scan Key"));
      }
      
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
          memcpy(newID, keys[selKey].uid, RW1990_UID_SIZE);
          mode = WRITE;
          Serial.println(F("WRITE_MODE"));
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

    case WRITE: {
      {
        char hdr[21];  // "Writing RW/RF: " (12) + up to 8 char name + null
        if (tempTp == TYPE_RW1990) strcpy(hdr, "Writing RW: ");
        else strcpy(hdr, "Writing RF: ");
        if (keys[selKey].name[0] != '\0') strncat(hdr, keys[selKey].name, sizeof(hdr) - strlen(hdr) - 1);
        drawHeader(hdr);
        display.setCursor(0, 14);
        formatUID(tempTp, newID, tempUidLen);
        display.setCursor(0, 24);
        display.print(F("Place key..."));
        display.display();
      }

      {
        unsigned long now = millis();
        unsigned long elapsed = now - tmStart;
        
        if (elapsed > 7000UL) {
          digitalWrite(LED_Y, LOW);
          mode = LIST; drawList(); busy = false; break;
        }
        
        if ((elapsed % 500) < 250) {
          digitalWrite(LED_Y, HIGH);
        } else {
          digitalWrite(LED_Y, LOW);
        }
        
        if (now - lastBeepMs >= 1000) {
          toneBeep(1000, 50);
          lastBeepMs = now;
        }
        
        bool devicePresent = false;
        if (tempTp == TYPE_RW1990) {
          // First read for contact stability check
          ow.reset_search();
          devicePresent = ow.search(tempBuf);
          if (devicePresent) {
            delay(150);
            // Second read to verify stable contact
            ow.reset_search();
            if (!ow.search(oldID) || memcmp(tempBuf, oldID, RW1990_UID_SIZE) != 0) {
                Serial.println("Unstable contact - break and retry");
              break;
            }
          }
        } else {
          if (rfid.PICC_IsNewCardPresent()) {
            devicePresent = rfid.PICC_ReadCardSerial();
          }
        }
        
        if (!devicePresent) {
          break;
        }
        
        digitalWrite(LED_Y, LOW);
      }
      
      bool res = false;

      // Show writing progress on line 24
      display.fillRect(0, 24, 128, 8, SSD1306_BLACK);
      display.setCursor(0, 24);
      display.print(F("Writing: * * * * * * * *"));
      display.display();

      if (tempTp == TYPE_RW1990) {
        res = rw1990_write(newID);
        
        // After write, read and display the result
        uint8_t readBuf[RW1990_UID_SIZE];
        if (rw1990_read_and_display(readBuf, true)) {
          // rw1990_read_and_display() has already shown the result and played appropriate beep
          // Just check if data matches what we wrote
          if (memcmp(readBuf, newID, RW1990_UID_SIZE) != 0) {
            // Data mismatch - write failed, show error overlay
            delay(1000);
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(0, 8);
            display.println(F("WR:FAIL"));
            display.println(F("Data mismatch"));
            display.display();
          }
        } else {
          // No device found after write
          errBeep();
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(0, 8);
          display.println("WR:FAIL");
          display.println("CHK:FAIL");
          display.display();
        }
      } else {
        // For MIFARE RF cards: authenticate sector 0, write UID blocks, verify
        res = rfid_mifare_write(newID, tempUidLen);

        display.clearDisplay();
        display.setCursor(0, 8);
        if (res) {
          okBeep();
          if (tempTp == TYPE_RFID_13M) display.println(F("RF13 OK"));
          else if (tempTp == TYPE_RFID_125K) display.println(F("RF125 OK"));
          display.println(F("WR:PASS"));
        } else {
          errBeep();
          display.println(F("WR:FAIL"));
        }
        display.display();
      }
      
      delay(1800);

      mode = LIST; drawList(); busy = false;
      break;
    }
  }
}
