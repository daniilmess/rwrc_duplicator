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
  TYPE_RW  = 0,
  TYPE_13  = 1,
  TYPE_125 = 2,
};

enum OneWireChipType {
  OW_RW1990_1 = 0,
  OW_RW1990_2,
  OW_DS1990A,
  OW_TM2004,
  OW_TM01,
  OW_UNKNOWN
};

enum RFIDChipType {
  RFID_EM4100 = 0,
  RFID_HID,
  RFID_MIFARE,
  RFID_DESFIRE,
  RFID_T5577,
  RFID_EM4305,
  RFID_UNKNOWN
};

struct KeyRec {
  uint8_t  type;
  uint8_t  uid[RW1990_UID_SIZE];
  uint8_t  uidLen;
  char     name[16];
  bool     isMaster;
  uint8_t  owChip;
  uint8_t  rfidChip;
};


KeyRec keys[MAX_KEYS];
uint8_t keyCnt = 0;

const KeyRec PROGMEM masterKeys[] = {
  {TYPE_RW,  {0x01, 0xCA, 0xC9, 0xAF, 0x02, 0x00, 0x00, 0xC0}, RW1990_UID_SIZE, "home_78",    true, OW_RW1990_1,  RFID_UNKNOWN},
  {TYPE_13,  {0x04, 0xA1, 0xB2, 0xC3, 0x00, 0x00, 0x00, 0x00}, 4,               "office_card", true, OW_UNKNOWN,   RFID_MIFARE },
  {TYPE_125, {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x00, 0x00, 0x00}, 5,               "garage_fob",  true, OW_UNKNOWN,   RFID_EM4100 },
  {TYPE_RW,  {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F}, RW1990_UID_SIZE, "RW_erase",   true, OW_RW1990_1,  RFID_UNKNOWN},
  {TYPE_13,  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 4,               "RF_erase",   true, OW_UNKNOWN,   RFID_MIFARE }
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

uint8_t tempOwChip = OW_UNKNOWN;
uint8_t tempRfidChip = RFID_UNKNOWN;

unsigned long tmStart = 0;
unsigned long lastBeepMs = 0;  // Track last beep time for WRITE mode
unsigned long lastTickMs = 0;   // Track last tick for scanning
unsigned long holdStartMs = 0;  // Track hold start for factory reset
unsigned long scanHoldStartMs = 0;  // Track hold start for scan exit
bool busy = false;
bool inScanMode = false;



void printOwChipNameToDisplay(uint8_t chip) {
  switch (chip) {
    case OW_RW1990_1: display.print(F("RW1990.1")); break;
    case OW_RW1990_2: display.print(F("RW1990.2")); break;
    case OW_DS1990A:  display.print(F("DS1990A"));  break;
    case OW_TM2004:   display.print(F("TM2004"));   break;
    case OW_TM01:     display.print(F("TM01"));     break;
    default:          display.print(F("Unknown"));  break;
  }
}

void printRfidChipNameToDisplay(uint8_t chip) {
  switch (chip) {
    case RFID_EM4100:  display.print(F("EM4100"));  break;
    case RFID_HID:     display.print(F("HID"));     break;
    case RFID_MIFARE:  display.print(F("Mifare"));  break;
    case RFID_DESFIRE: display.print(F("DESFire")); break;
    case RFID_T5577:   display.print(F("T5577"));   break;
    case RFID_EM4305:  display.print(F("EM4305"));  break;
    default:           display.print(F("Unknown")); break;
  }
}

// Probe the OneWire bus to identify the chip type.
// Takes the already-read 8-byte ROM (uid[0] is the family code).
// For family 0x01, sends write-enable and checks acknowledgment to
// distinguish RW1990.1 from RW1990.2.
uint8_t detectOneWireChip(const uint8_t* uid) {
  switch (uid[0]) {
    case 0x70: return OW_TM2004;
    case 0x2D: return OW_TM01;
    case 0x01: {
      // Probe write-enable: RW1990.2 pulls the bus low as acknowledgment
      ow.reset();
      ow.skip();
      ow.write(0xD1);
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(60);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
      delayMicroseconds(200);
      bool ack = !digitalRead(OW_PIN);
      // Re-lock write mode (short pulse = lock)
      ow.reset();
      ow.skip();
      ow.write(0xD1);
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(10);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
      delay(5);
      return ack ? OW_RW1990_2 : OW_RW1990_1;
    }
    default: return OW_UNKNOWN;
  }
}

// Identify the RFID chip using the SAK byte already held in rfid.uid.
// Must be called before rfid.PICC_HaltA().
// Note: ISO 14443-4 cards with 7-byte UIDs are assumed DESFire; with shorter
// UIDs they are heuristically classified as HID. EM4100 is 125 kHz and will
// not be detected here (MFRC522 reads 13.56 MHz only).
uint8_t detectRFIDChip() {
  MFRC522::PICC_Type piccType = rfid.PICC_GetType(rfid.uid.sak);
  switch (piccType) {
    case MFRC522::PICC_TYPE_MIFARE_MINI:
    case MFRC522::PICC_TYPE_MIFARE_1K:
    case MFRC522::PICC_TYPE_MIFARE_4K:
    case MFRC522::PICC_TYPE_MIFARE_UL:   return RFID_MIFARE;
    case MFRC522::PICC_TYPE_MIFARE_DESFIRE: return RFID_DESFIRE;
    case MFRC522::PICC_TYPE_ISO_14443_4:
      return (rfid.uid.size == 7) ? RFID_DESFIRE : RFID_HID;
    default:
      return RFID_UNKNOWN;
  }
}


// Helper function to check Family/CRC errors and return flash string
// Validates RW1990 family byte (buf[0]) and CRC checksum (buf[7])
const __FlashStringHelper* rw1990_check_errors(const uint8_t* buf) {
  bool familyOk = (buf[0] == 0x01);
  bool crcOk = (ow.crc8(buf, 7) == buf[7]);
  if (!familyOk && !crcOk) return F("Family:ERR CRC:ERR");
  if (!familyOk) return F("Family:ERR");
  if (!crcOk) return F("CRC:ERR");
  return F("OK");
}

// Returns true when buf contains a valid RW1990 UID:
// family byte (buf[0]) == 0x01 AND CRC (buf[7]) matches.
// Replaces strcmp(rw1990_check_errors(buf), "OK") comparisons.
static bool rw1990_is_ok(const uint8_t* buf) {
  return (buf[0] == 0x01) && (ow.crc8(buf, 7) == buf[7]);
}

// Read RW1990 key and output diagnostics to serial
// Parameters:
//   buf: Buffer to store 8-byte UID
// Returns:
//   true if key is found (regardless of Family/CRC errors)
//   false if no key is present
// Note: Always outputs diagnostic info to Serial including error status
bool rw1990_read(uint8_t* buf) {
  ow.reset_search();
  bool found = ow.search(buf);
  
  if (!found) {
    Serial.println(F("RW:NODEV"));
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
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    oldID[i] = ow.read();
  }

  // Debug: show id to be written
  Serial.print(F("newID:"));
  for (uint8_t i = 0; i < RW1990_UID_SIZE; i++) {
    Serial.print(id[i] < 16 ? "0" : "");
    Serial.print(id[i], HEX);
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

// Authenticate a MIFARE sector using Key A with the given key.
// sector: 0-15 for MIFARE Classic 1K.
// Returns true on success, false on failure with Serial diagnostic.
bool mifare_auth_sector(byte sector, MFRC522::MIFARE_Key* key) {
  byte blockAddr = sector * 4;
  MFRC522::StatusCode status = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("AUTH:FAIL s="));
    Serial.println(sector);
    return false;
  }
  return true;
}

// Write data to MIFARE block 4 (sector 1, first data block) using default key.
// Selects the card, authenticates sector 1, writes 16 bytes, verifies by read-back.
// Returns true on success.
bool rfid_mifare_write(const uint8_t* data, uint8_t dataLen) {
  Serial.println(F("DEBUG:rfid_mifare_write START"));

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    Serial.println(F("RF:NODEV"));
    return false;
  }
  Serial.println(F("RF:PICC_PRESENT"));

  Serial.print(F("RF:UID:"));
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) Serial.print('0');
    Serial.print(rfid.uid.uidByte[i], HEX);
  }
  Serial.println();

  Serial.print(F("RF:SAK:0x"));
  if (rfid.uid.sak < 0x10) Serial.print('0');
  Serial.println(rfid.uid.sak, HEX);
  MFRC522::PICC_Type piccType = rfid.PICC_GetType(rfid.uid.sak);
  Serial.print(F("RF:PICC_TYPE:"));
  Serial.println((int)piccType);

  Serial.print(F("DATA:"));
  for (byte i = 0; i < dataLen && i < 16; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  Serial.println(F("RF:AUTH_ATTEMPT"));
  byte blockAddr = 4; // sector 1, first data block
  MFRC522::StatusCode authStatus = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(rfid.uid));
  Serial.print(F("RF:AUTH_STATUS:"));
  Serial.println((int)authStatus);
  if (authStatus != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }
  Serial.println(F("RF:AUTH_OK"));

  byte writeData[16] = {0};
  memcpy(writeData, data, min(dataLen, (uint8_t)16));

  Serial.print(F("RF:WRITE_BLOCK:4 LEN:"));
  Serial.println(dataLen);
  MFRC522::StatusCode writeStatus = rfid.MIFARE_Write(4, writeData, 16);
  Serial.print(F("RF:WRITE_STATUS:"));
  Serial.println((int)writeStatus);
  if (writeStatus != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  Serial.println(F("RF:READ_ATTEMPT"));
  byte readBuf[18];
  byte readLen = sizeof(readBuf);
  MFRC522::StatusCode readStatus = rfid.MIFARE_Read(4, readBuf, &readLen);
  Serial.print(F("RF:READ_STATUS:"));
  Serial.println((int)readStatus);
  if (readStatus != MFRC522::STATUS_OK) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return false;
  }

  Serial.print(F("RF:READ_DATA:"));
  for (byte i = 0; i < 16; i++) {
    if (readBuf[i] < 0x10) Serial.print('0');
    Serial.print(readBuf[i], HEX);
    Serial.print(' ');
  }
  Serial.println();

  bool match = (memcmp(readBuf, writeData, 16) == 0);
  Serial.println(match ? F("CMP:OK") : F("CMP:FAIL"));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return match;
}

void toneBeep(int hz, int ms) {
  tone(BUZZ, hz, ms);
  delay(ms + 10);
}

void fatalBlinkYellow() {
  while (1) {
    digitalWrite(LED_Y, HIGH); delay(200);
    digitalWrite(LED_Y, LOW);  delay(200);
  }
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
  
  if (firstBootFlag != 0x02) {
    Serial.print(F("BOOT:FIRST CNT:"));
    Serial.println(MASTER_KEYS_COUNT);
    loadMasterKeys();
    saveEEPROM();
    EEPROM.update(EEPROM_FIRST_BOOT_FLAG, 0x02);
  } else {
    Serial.println(F("EEPROM"));
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

bool addKey(uint8_t tp, const uint8_t* d, uint8_t len, uint8_t owChip, uint8_t rfidChip) {
  if (keyCnt >= MAX_KEYS) return false;
  keys[keyCnt].type = tp;
  memcpy(keys[keyCnt].uid, d, RW1990_UID_SIZE);
  keys[keyCnt].uidLen = len;
  keys[keyCnt].name[0] = '\0';
  keys[keyCnt].isMaster = false;
  keys[keyCnt].owChip = owChip;
  keys[keyCnt].rfidChip = rfidChip;
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

void drawKeyInfo(const char* txt) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(txt);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

void drawKeyInfoAndShow(const char* txt) {
  drawKeyInfo(txt);
  display.display();
}

void drawKeyInfoOwChip(const __FlashStringHelper* prefix, uint8_t chip) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(prefix);
  printOwChipNameToDisplay(chip);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}

void drawKeyInfoRfidChip(const __FlashStringHelper* prefix, uint8_t chip) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(prefix);
  printRfidChipNameToDisplay(chip);
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
  drawKeyInfo("Keys");
  int start = max(0, selKey - 1);
  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= keyCnt) break;
    display.setCursor(4, 12 + i * 8);
    display.print(idx == selKey ? ">" : " ");
    
    if (keys[idx].type == TYPE_RW) display.print("RW ");
    else if (keys[idx].type == TYPE_13) display.print("13 ");
    else if (keys[idx].type == TYPE_125) display.print("125 ");

    if (keys[idx].name[0] != '\0') {
      display.print(keys[idx].name);
    } else {
      formatUID(keys[idx].type, keys[idx].uid, keys[idx].uidLen);
    }
  }
  display.display();
}

void drawSavedKeyDetail() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (keys[selKey].type == TYPE_RW)       display.println(F("RW Key"));
  else if (keys[selKey].type == TYPE_13)  display.println(F("13 MHz Key"));
  else                                    display.println(F("125 kHz Key"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 12);
  formatUID(keys[selKey].type, keys[selKey].uid, keys[selKey].uidLen);
  
  display.setCursor(0, 24);
  if (cursor == 0) {
    display.print(F("> Write  Delete"));
  } else {
    display.print(F("  Write [Delete]"));
  }
  display.display();
}


void drawConfirmDelete() {
  drawKeyInfo("Delete key?");
  display.setCursor(4, 14);
  display.println(deleteConfirm ? "> YES   NO" : "  YES > NO");
  display.display();
}

void showScanning(const __FlashStringHelper* msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 8);
  display.println(msg);
  display.println(F("2s exit"));
  display.display();
}

// Helper function to print hex byte
void printHex(uint8_t b) {
  if (b < 16) display.print('0');
  display.print(b, HEX);
}

void formatUID(uint8_t type, const uint8_t* uid, uint8_t uidLen) {
  if (type == TYPE_RW && uidLen == 8) {
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



void setup() {
  pinMode(LED_Y, OUTPUT);
  pinMode(LED_G, OUTPUT);
  digitalWrite(LED_Y, LOW);
  digitalWrite(LED_G, LOW);

  Serial.begin(115200);
  Serial.println(F("START"));

  Wire.begin();
  Wire.setClock(100000);
  delay(100);

  Wire.beginTransmission(OLED_ADDR);
  uint8_t i2cRC = Wire.endTransmission();
  Serial.print(F("OLED:I2C:RC="));
  Serial.println(i2cRC);
  if (i2cRC != 0) {
    Serial.println(F("OLED NO_3C"));
    fatalBlinkYellow();
  }

  {
    bool oledOk = false;
    for (uint8_t attempt = 1; attempt <= 3 && !oledOk; attempt++) {
      Serial.print(F("OLED TRY "));
      Serial.println(attempt);
      if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
      } else {
        delay(200);
      }
    }
    if (!oledOk) {
      Serial.println(F("OLED FAIL"));
      fatalBlinkYellow();
    }
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 8);
  display.println(F("OLED OK"));
  display.println(F("rwrc_duplicator"));
  display.display();
  delay(300);

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
      if (rw1990_read(tempBuf)) {
        tempOwChip = detectOneWireChip(tempBuf);
        drawKeyInfoOwChip(F("RW: "), tempOwChip);
        display.setCursor(0, 14);
        formatUID(TYPE_RW, tempBuf, RW1990_UID_SIZE);
        display.setCursor(0, 24);
        display.print(F("| "));
        display.print(rw1990_check_errors(tempBuf));
        display.display();
        if (rw1990_is_ok(tempBuf)) okBeep(); else errBeep();
        tempTp = TYPE_RW;
        tempRfidChip = RFID_UNKNOWN;
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
        tempRfidChip = detectRFIDChip();
        rfid.PICC_HaltA();
        drawKeyInfoRfidChip(F("RF 13.56: "), tempRfidChip);
        display.setCursor(0, 14);
        formatUID(TYPE_13, tempBuf, tempUidLen);
        display.display();
        okBeep();
        tempTp = TYPE_13;
        tempOwChip = OW_UNKNOWN;
        
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
        if (addKey(tempTp, tempBuf, tempUidLen, tempOwChip, tempRfidChip)) okBeep();
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
        uint8_t chipType = (tempTp == TYPE_RW) ? keys[selKey].owChip : keys[selKey].rfidChip;
        if (tempTp == TYPE_RW) {
          drawKeyInfoOwChip(F("WR: "), chipType);
        } else {
          drawKeyInfoRfidChip(F("WR RF: "), chipType);
        }
        display.setCursor(0, 14);
        formatUID(tempTp, newID, tempUidLen);
        display.setCursor(0, 24);
        display.print(F("Place key"));
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
        if (tempTp == TYPE_RW) {
          // First read for contact stability check
          ow.reset_search();
          devicePresent = ow.search(tempBuf);
          if (devicePresent) {
            delay(50);
            // Second read to verify stable contact
            ow.reset_search();
            if (!ow.search(oldID) || memcmp(tempBuf, oldID, RW1990_UID_SIZE) != 0) {
              // Unstable contact - break and retry
              break;
            }
          }
        } else {
          devicePresent = rfid.PICC_IsNewCardPresent();
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

      if (tempTp == TYPE_RW) {
        res = rw1990_write(newID);
        
        // After write, read and display the result
        uint8_t readBuf[RW1990_UID_SIZE];
        if (rw1990_read(readBuf)) {
          uint8_t rchip = detectOneWireChip(readBuf);
          drawKeyInfoOwChip(F("RW: "), rchip);
          display.setCursor(0, 14);
          formatUID(TYPE_RW, readBuf, RW1990_UID_SIZE);
          display.setCursor(0, 24);
          display.print(F("| "));
          display.print(rw1990_check_errors(readBuf));
          display.display();
          if (rw1990_is_ok(readBuf)) okBeep(); else errBeep();
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
          display.println(F("WR:FAIL"));
          display.println(F("CHK:FAIL"));
          display.display();
        }
      } else {
        // For MIFARE RF cards: authenticate sector 1, write data, verify
        if (tempTp == TYPE_13) {
          Serial.println(F("WRITE_RF13_START"));
        }
        res = rfid_mifare_write(newID, tempUidLen);
        if (tempTp == TYPE_13) {
          Serial.print(F("WRITE_RF13_RESULT:"));
          Serial.println(res ? "OK" : "FAIL");
        }

        display.clearDisplay();
        display.setCursor(0, 8);
        if (res) {
          okBeep();
          if (tempTp == TYPE_13) display.println(F("13 OK"));
          else if (tempTp == TYPE_125) display.println(F("125 OK"));
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
