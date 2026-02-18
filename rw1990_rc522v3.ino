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



enum KeyType {
  TYPE_RW1990 = 0,
  TYPE_RFID_13M = 1,
  TYPE_RFID_125K = 2,
};

struct KeyRec {
  uint8_t  type;
  uint8_t  uid[8];
  uint8_t  uidLen;
  char     name[16];
  bool     isMaster;
};

KeyRec keys[MAX_KEYS];
uint8_t keyCnt = 0;

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
  WRITE,
  DIAGNOSTICS,
  DIAG_RW_CHECK,
  DIAG_RW_RAW,
  DIAG_RW_ERASE_FF,
  DIAG_RF_ERASE
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


bool rw1990_read(uint8_t* buf) {
  ow.reset_search();
  noInterrupts();
  bool res = false;
  if (ow.search(buf)) {
    byte crc = ow.crc8(buf, 7);
    if (crc == buf[7]) res = true;
  } else {
    ow.reset_search();
  }
  interrupts();
  if (res) {
    Serial.print(F("RW:OK "));
    for (uint8_t i = 0; i < 8; i++) {
      if (buf[i] < 16) Serial.print('0');
      Serial.print(buf[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
  } else {
    Serial.println(F("RW:ERR"));
  }
  return res;
}

bool rw1990_write(const uint8_t* newID) {
  uint8_t dummy[8];
  if (!rw1990_read(dummy)) {
    Serial.println(F("RW:NODEV"));
    return false;
  }

  Serial.println(F("RW:WR"));

  ow.skip();
  ow.reset();
  ow.write(0x33);

  Serial.print(F("ID: "));
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = ow.read();
    Serial.print(b < 16 ? "0" : "");
    Serial.print(b, HEX);
    Serial.print(' ');
  }
  Serial.println();

  ow.skip();
  ow.reset();
  ow.write(0xD1);

  noInterrupts();
  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(60);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);
  interrupts();
  delay(10);

  ow.skip();
  ow.reset();
  ow.write(0xD5);

  Serial.print(F("WR: "));
  for (uint8_t i = 0; i < 8; i++) {
    rw1990_write_byte(newID[i]);
    Serial.print('*');
  }
  Serial.println();

  ow.skip();
  ow.reset();
  ow.write(0xD1);

  noInterrupts();
  digitalWrite(OW_PIN, LOW);
  pinMode(OW_PIN, OUTPUT);
  delayMicroseconds(60);
  pinMode(OW_PIN, INPUT);
  digitalWrite(OW_PIN, HIGH);
  interrupts();

  delay(200);

  Serial.println(F("VRF"));
  uint8_t check[8];
  bool success = false;
  
  if (rw1990_read(check)) {
    success = (memcmp(check, newID, 8) == 0);
    Serial.println(success ? F("VRF:OK") : F("VRF:FAIL"));
  } else {
    Serial.println(F("VRF:NODEV"));
  }

  return success;
}

void rw1990_write_byte(uint8_t data) {
  for (uint8_t bit = 0; bit < 8; bit++) {
    noInterrupts();
    if (data & 1) {
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(60);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    } else {
      digitalWrite(OW_PIN, LOW);
      pinMode(OW_PIN, OUTPUT);
      delayMicroseconds(5);
      pinMode(OW_PIN, INPUT);
      digitalWrite(OW_PIN, HIGH);
    }
    interrupts();
    delay(15);
    data >>= 1;
  }
}


bool rw1990_check_presence() {
  ow.reset_search();
  noInterrupts();
  bool present = ow.search(tempBuf);
  if (!present) {
    ow.reset_search();
  }
  interrupts();
  return present;
}

bool rw1990_read_raw(uint8_t* buf) {
  ow.reset_search();
  noInterrupts();
  bool res = false;
  if (ow.search(buf)) {
    res = true;
  } else {
    ow.reset_search();
  }
  interrupts();
  return res;
}

bool rw1990_erase_ff() {
  uint8_t blank[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return rw1990_write(blank);
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


uint8_t detectRF() {
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    return TYPE_RFID_13M;
  }
  
  return 255;
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
  memcpy(keys[keyCnt].uid, d, 8);
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

void diagHeader(const __FlashStringHelper* title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(title);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
}



void drawMain() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 2); display.println(cursor == 0 ? "> Read RW" : "  Read RW");
  display.setCursor(4, 10); display.println(cursor == 1 ? "> Read RF"   : "  Read RF");
  display.setCursor(4, 18); display.println(cursor == 2 ? "> Keys"  : "  Keys");
  display.setCursor(4, 26); display.println(cursor == 3 ? "> Diag" : "  Diag");
  display.display();
}

void drawDiagnostics() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("DIAG"));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  
  int start = max(0, min(cursor, 1));
  for (int i = 0; i < 3; i++) {
    int idx = start + i;
    if (idx >= 4) break;
    display.setCursor(4, 12 + i * 8);
    display.print(idx == cursor ? "> " : "  ");
    
    if (idx == 0) display.print(F("RW Chk"));
    else if (idx == 1) display.print(F("RW Raw"));
    else if (idx == 2) display.print(F("RW Erase"));
    else if (idx == 3) display.print(F("RF Erase"));
  }
  
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
    for (uint8_t j = 0; j < keys[selKey].uidLen && j < 8; j++) {
      printHex(keys[selKey].uid[j]);
      if (j < keys[selKey].uidLen - 1) display.print(' ');
    }
  }
  
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
  display.println("7s timeout");
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

const char* getKeyTypeStr(uint8_t type) {
  if (type == TYPE_RW1990) return "RW1990 ID";
  if (type == TYPE_RFID_13M) return "RF 13.56 MHz";
  if (type == TYPE_RFID_125K) return "RF 125 kHz";
  return "Key ID";
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
        cursor = (cursor + enc.dir() + 4) % 4;
        drawMain();
      }
      if (enc.click()) {
        if (cursor == 0) { 
          mode = READ_RW; 
          tmStart = millis(); 
          lastTickMs = millis();
          inScanMode = true;
          busy = true;
          showScanning(F("Scan RW1990"));
        }
        if (cursor == 1) { 
          mode = READ_RF; 
          tmStart = millis(); 
          lastTickMs = millis();
          inScanMode = true;
          busy = true;
          showScanning(F("Scan RF"));
        }
        if (cursor == 2) { 
          mode = LIST; 
          selKey = 0; 
          drawList(); 
        }
        if (cursor == 3) {
          mode = DIAGNOSTICS;
          cursor = 0;
          drawDiagnostics();
        }
      }
      break;

    case READ_RW: {

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

      // Try to read RW1990
      if (rw1990_read(tempBuf)) {
        tempTp = TYPE_RW1990;
        tempUidLen = 8;
        
        okBeep();
        
        displayKeyUID(tempTp, tempBuf, tempUidLen, true);
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
      }
      break;
    }

    case READ_RF:

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

      // Try to detect RF card
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        tempTp = TYPE_RFID_13M;
        tempUidLen = min(rfid.uid.size, (uint8_t)8);
        memcpy(tempBuf, rfid.uid.uidByte, tempUidLen);
        rfid.PICC_HaltA();
        
        okBeep();
        
        displayKeyUID(tempTp, tempBuf, tempUidLen, true);
        tmStart = millis();
        mode = READ_RESULT;
        inScanMode = false;
        busy = true;
      }
      
      break;

    case READ_RESULT:
      // Display result for 3 seconds, then return to scanning
      if (millis() - tmStart > 3000UL) {

        if (tempTp == TYPE_RW1990) {
          mode = READ_RW;
          tmStart = millis();  // Reset scan timeout
          lastTickMs = millis();
          inScanMode = true;
          showScanning(F("Scan RW1990"));
        } else {
          mode = READ_RF;
          tmStart = millis();
          lastTickMs = millis();
          inScanMode = true;
          showScanning(F("Scan RF"));
        }
        break;
      }
      
      if (enc.click()) {
        if (addKey(tempTp, tempBuf, tempUidLen)) okBeep();
        else errBeep();
        

        if (tempTp == TYPE_RW1990) {
          mode = READ_RW;
        } else {
          mode = READ_RF;
        }
        tmStart = millis();  // Reset scan timeout
        lastTickMs = millis();
        inScanMode = true;
        

        display.clearDisplay();
        if (tempTp == TYPE_RW1990) {
          showScanning(F("Scan RW1990"));
        } else {
          showScanning(F("Scan RF"));
        }
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

    case WRITE: {
      {
        const char* msg;
        if (tempTp == TYPE_RW1990) msg = "RW1990 blank";
        else if (tempTp == TYPE_RFID_13M) msg = "RF13 magic card";
        else if (tempTp == TYPE_RFID_125K) msg = "RF125 magic card";
        else msg = "Device";
        showApply(msg);
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
          uint8_t presenceCheckBuf[8];
          devicePresent = rw1990_read(presenceCheckBuf);
        } else {
          devicePresent = rfid.PICC_IsNewCardPresent();
        }
        
        if (!devicePresent) {
          break;
        }
        
        digitalWrite(LED_Y, LOW);
      }
      
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
        if (tempTp == TYPE_RW1990) display.println("RW OK");
        else if (tempTp == TYPE_RFID_13M) display.println("RF13 OK");
        else if (tempTp == TYPE_RFID_125K) display.println("RF125 OK");
        display.println("CHK:PASS");
      } else {
        errBeep();
        display.println("WR:FAIL");
        display.println("CHK:FAIL");
      }
      display.display();
      delay(1800);

      mode = LIST; drawList(); busy = false;
      break;
    }

    case DIAGNOSTICS: {
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 4) % 4;
        drawDiagnostics();
      }
      if (enc.click()) {
        if (cursor == 0) {
          mode = DIAG_RW_CHECK;
          tmStart = millis();
          busy = true;
        } else if (cursor == 1) {
          mode = DIAG_RW_RAW;
          tmStart = millis();
          busy = true;
        } else if (cursor == 2) {
          mode = DIAG_RW_ERASE_FF;
          tmStart = millis();
          busy = true;
        } else if (cursor == 3) {
          mode = DIAG_RF_ERASE;
          tmStart = millis();
          busy = true;
        }
      }
      if (enc.hold()) {
        mode = MAIN;
        cursor = 0;
        drawMain();
      }
      break;
    }

    case DIAG_RW_CHECK: {
      diagHeader(F("RW Chk"));
      display.setCursor(0, 14);
      display.println(F("Check..."));
      display.display();
      
      delay(300);
      
      bool present = rw1990_check_presence();
      
      diagHeader(F("RW Chk"));
      display.setCursor(0, 16);
      
      if (present) {
        display.println(F("Found!"));
        okBeep();
      } else {
        display.println(F("No Key"));
        errBeep();
      }
      
      display.display();
      delay(2000);
      
      mode = DIAGNOSTICS;
      cursor = 0;
      drawDiagnostics();
      busy = false;
      break;
    }

    case DIAG_RW_RAW: {
      diagHeader(F("RW Raw"));
      display.setCursor(0, 14);
      display.println(F("Read..."));
      display.display();
      
      delay(300);
      
      uint8_t rawBuf[8];
      bool found = rw1990_read_raw(rawBuf);
      
      diagHeader(F("RW Raw"));
      
      if (found) {
        display.setCursor(0, 12);
        display.println(F("(no CRC)"));
        display.setCursor(0, 22);
        for (uint8_t i = 0; i < 8; i++) {
          if (rawBuf[i] < 16) display.print('0');
          display.print(rawBuf[i], HEX);
          if (i < 7) display.print(' ');
        }
        okBeep();
      } else {
        display.setCursor(0, 16);
        display.println(F("No Key"));
        errBeep();
      }
      
      display.display();
      delay(2000);
      
      mode = DIAGNOSTICS;
      cursor = 1;
      drawDiagnostics();
      busy = false;
      break;
    }

    case DIAG_RW_ERASE_FF: {
      diagHeader(F("RW Erase"));
      display.setCursor(0, 14);
      display.println(F("Place key..."));
      display.display();
      
      unsigned long startWait = millis();
      bool keyPresent = false;
      while (millis() - startWait < 5000UL) {
        if (rw1990_check_presence()) {
          keyPresent = true;
          break;
        }
        delay(100);
      }
      
      if (!keyPresent) {
        diagHeader(F("RW Erase"));
        display.setCursor(0, 16);
        display.println(F("Timeout"));
        display.display();
        errBeep();
        delay(2000);
        mode = DIAGNOSTICS;
        cursor = 2;
        drawDiagnostics();
        busy = false;
        break;
      }
      
      diagHeader(F("RW Erase"));
      display.setCursor(0, 14);
      display.println(F("Write FF..."));
      display.display();
      
      delay(500);
      
      bool success = rw1990_erase_ff();
      
      diagHeader(F("RW Erase"));
      display.setCursor(0, 16);
      
      if (success) {
        display.println(F("OK!"));
        okBeep();
      } else {
        display.println(F("Failed"));
        errBeep();
      }
      
      display.display();
      delay(2000);
      
      mode = DIAGNOSTICS;
      cursor = 2;
      drawDiagnostics();
      busy = false;
      break;
    }

    case DIAG_RF_ERASE: {
      diagHeader(F("RF Erase"));
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
        diagHeader(F("RF Erase"));
        display.setCursor(0, 16);
        display.println(F("Timeout"));
        display.display();
        errBeep();
        delay(2000);
        mode = DIAGNOSTICS;
        cursor = 3;
        drawDiagnostics();
        busy = false;
        break;
      }
      
      diagHeader(F("RF Erase"));
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
      
      diagHeader(F("RF Erase"));
      display.setCursor(0, 16);
      
      if (success) {
        display.println(F("OK!"));
        okBeep();
      } else {
        display.println(F("Protect?"));
        errBeep();
      }
      
      display.display();
      delay(2000);
      
      mode = DIAGNOSTICS;
      cursor = 3;
      drawDiagnostics();
      busy = false;
      break;
    }
  }
}
