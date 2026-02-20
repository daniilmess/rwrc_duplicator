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
  {TYPE_RW,  {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F}, RW1990_UID_SIZE, "RW_erase",   true, OW_RW1990_1,  RFID_UNKNOWN}
};
const uint8_t MASTER_KEYS_COUNT = sizeof(masterKeys) / sizeof(masterKeys[0]);

enum Mode {
  MAIN,
  READ_KEY,
  LIST,
  READ_RESULT,
  SAVED_DETAIL,
  CONFIRM_DELETE,
  WRITE,
  DIAGNOSTICS,
  DIAG_RF_ERASE
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


const char* owChipName(uint8_t chip) {
  switch (chip) {
    case OW_RW1990_1: return "RW1990.1";
    case OW_RW1990_2: return "RW1990.2";
    case OW_DS1990A:  return "DS1990A";
    case OW_TM2004:   return "TM2004";
    case OW_TM01:     return "TM01";
    default:          return "Unknown";
  }
}

const char* rfidChipName(uint8_t chip) {
  switch (chip) {
    case RFID_EM4100:  return "EM4100";
    case RFID_HID:     return "HID";
    case RFID_MIFARE:  return "Mifare";
    case RFID_DESFIRE: return "DESFire";
    case RFID_T5577:   return "T5577";
    case RFID_EM4305:  return "EM4305";
    default:           return "Unknown";
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

void drawKeyInfoDual(const char* prefix, const char* suffix) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(prefix);
  display.println(suffix);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}



void drawMain() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 2); display.println(cursor == 0 ? "> Read Key" : "  Read Key");
  display.setCursor(4, 10); display.println(cursor == 1 ? "> Keys"  : "  Keys");
  display.setCursor(4, 18); display.println(cursor == 2 ? "> Diag" : "  Diag");
  display.display();
}

void drawDiagnostics() {
  drawKeyInfo("DIAG");
  display.setCursor(4, 12);
  display.print(cursor == 0 ? "> RF Erase" : "  RF Erase");
  display.display();
}

void returnToDiagnostics(uint8_t cursorPos) {
  mode = DIAGNOSTICS;
  cursor = cursorPos;
  drawDiagnostics();
  busy = false;
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
      if (keys[idx].type == TYPE_RW) {
        for (uint8_t j = 1; j < 7; j++) {
          printHex(keys[idx].uid[j]);
          if (j < 6 && j % 2 == 0) display.print(' ');
        }
      } else if (keys[idx].type == TYPE_13) {
        for (uint8_t j = 0; j < keys[idx].uidLen && j < 4; j++) {
          printHex(keys[idx].uid[j]);
          if (j < keys[idx].uidLen - 1) display.print(' ');
        }
      } else {
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
  if (keys[selKey].type == TYPE_RW)       display.println(F("RW Key"));
  else if (keys[selKey].type == TYPE_13)  display.println(F("13 MHz Key"));
  else                                    display.println(F("125 kHz Key"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 12);
  
  if (keys[selKey].type == TYPE_RW) {
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
  display.println(F("Hold 2s exit"));
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
    Serial.println(F("OLED:I2C:NO_3C"));
    fatalBlinkYellow();
  }

  {
    bool oledOk = false;
    for (uint8_t attempt = 1; attempt <= 3 && !oledOk; attempt++) {
      Serial.print(F("OLED:BEGIN:TRY "));
      Serial.println(attempt);
      if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledOk = true;
      } else {
        delay(200);
      }
    }
    if (!oledOk) {
      Serial.println(F("OLED:BEGIN:FAIL"));
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
        cursor = (cursor + enc.dir() + 3) % 3;
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
        if (cursor == 2) {
          mode = DIAGNOSTICS;
          cursor = 0;
          drawDiagnostics();
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
        const char* err = rw1990_check_errors(tempBuf);
        drawKeyInfoDual("RW: ", owChipName(tempOwChip));
        display.setCursor(0, 14);
        formatUID(TYPE_RW, tempBuf, RW1990_UID_SIZE);
        display.setCursor(0, 24);
        display.print("| ");
        display.print(err);
        display.display();
        if (strcmp(err, "OK") == 0) okBeep(); else errBeep();
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
        drawKeyInfoDual("RF 13.56: ", rfidChipName(tempRfidChip));
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
          drawKeyInfoDual("WR: ", owChipName(chipType));
        } else {
          drawKeyInfoDual("WR RF: ", rfidChipName(chipType));
        }
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
          const char* rerr = rw1990_check_errors(readBuf);
          drawKeyInfoDual("RW: ", owChipName(rchip));
          display.setCursor(0, 14);
          formatUID(TYPE_RW, readBuf, RW1990_UID_SIZE);
          display.setCursor(0, 24);
          display.print("| ");
          display.print(rerr);
          display.display();
          if (strcmp(rerr, "OK") == 0) okBeep(); else errBeep();
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
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
          res = (memcmp(rfid.uid.uidByte, newID, min(rfid.uid.size, (size_t)RW1990_UID_SIZE)) == 0);
          rfid.PICC_HaltA();
        }
        
        display.clearDisplay();
        display.setCursor(0, 8);
        if (res) {
          okBeep();
          if (tempTp == TYPE_13) display.println("13 OK");
          else if (tempTp == TYPE_125) display.println("125 OK");
          display.println("CHK:PASS");
        } else {
          errBeep();
          display.println("WR:FAIL");
          display.println("CHK:FAIL");
        }
        display.display();
      }
      
      delay(1800);

      mode = LIST; drawList(); busy = false;
      break;
    }

    case DIAGNOSTICS: {
      if (enc.click()) {
        mode = DIAG_RF_ERASE;
        tmStart = millis();
        busy = true;
      }
      if (enc.hold()) {
        mode = MAIN;
        cursor = 0;
        drawMain();
      }
      break;
    }

    case DIAG_RF_ERASE: {
      drawKeyInfo("RF Erase");
      display.setCursor(0, 14);
      display.println(F("Place card..."));
      display.display();
      
      unsigned long startWait = millis();
      bool cardPresent = false;
      while (millis() - startWait < 5000UL) {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
          cardPresent = true;
          break;
        }
        delay(100);
      }
      
      if (!cardPresent) {
        drawKeyInfo("RF Erase");
        display.setCursor(0, 16);
        display.println(F("Timeout"));
        display.display();
        errBeep();
        delay(2000);
        returnToDiagnostics(0);
        break;
      }
      
      drawKeyInfo("RF Erase");
      display.setCursor(0, 14);
      display.println(F("Erase..."));
      display.display();
      
      delay(500);
      
      bool success = true;
      MFRC522::StatusCode status;
      byte zeros[16] = {0};
      
      MFRC522::MIFARE_Key key;
      for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
      
      for (byte sector = 1; sector < 4 && success; sector++) {
        byte blockAddr = sector * 4;
        
        status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(rfid.uid));
        if (status != MFRC522::STATUS_OK) {
          success = false;
          break;
        }
        
        status = rfid.MIFARE_Write(blockAddr, zeros, 16);
        if (status != MFRC522::STATUS_OK) {
          success = false;
          break;
        }
      }
      
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      
      // After erasing, read and display card state
      delay(200);
      drawKeyInfo("RF Erase");
      display.setCursor(0, 14);
      
      if (success) {
        display.println(F("OK!"));
        
        // Try to read the card again to show final state
        uint8_t cardBuf[RW1990_UID_SIZE];
        uint8_t cardLen = 0;
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
          cardLen = min(rfid.uid.size, (uint8_t)RW1990_UID_SIZE);
          memcpy(cardBuf, rfid.uid.uidByte, cardLen);
          rfid.PICC_HaltA();
          display.setCursor(0, 14);
          formatUID(TYPE_13, cardBuf, cardLen);
          display.display();
          display.setCursor(0, 22);
          display.print(F("Card present"));
        }
        okBeep();
      } else {
        display.println(F("Protect?"));
        errBeep();
      }
      
      display.display();
      delay(2000);
      
      returnToDiagnostics(0);
      break;
    }
  }
}
