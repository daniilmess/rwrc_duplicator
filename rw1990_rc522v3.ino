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

struct KeyRec {
  uint8_t  type;      // 0 = RW1990, 1 = MIFARE
  uint8_t  uid[8];
};

KeyRec keys[MAX_KEYS];
uint8_t keyCnt = 0;

enum Mode {
  MAIN,
  READ_RW,
  READ_MF,
  LIST,
  READ_ACTION,
  SAVED_ACTION,
  CONFIRM_DELETE,
  WRITE
};
Mode mode = MAIN;

int cursor = 0;
int selKey = 0;
bool deleteConfirm = false;

uint8_t tempBuf[8];
uint8_t tempTp = 0;

unsigned long tmStart = 0;
bool busy = false;

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
  toneBeep(1400, 90); delay(50);
  toneBeep(1900, 90); delay(50);
  toneBeep(2400, 140);
  delay(600);
  digitalWrite(LED_G, LOW);
}

void errBeep() {
  digitalWrite(LED_Y, HIGH);
  toneBeep(380, 320); delay(100);
  toneBeep(280, 380);
  delay(1000);
  digitalWrite(LED_Y, LOW);
}

// ────────────────────────────────────────────────
 // EEPROM
// ────────────────────────────────────────────────

void loadEEPROM() {
  keyCnt = EEPROM.read(0);
  if (keyCnt > MAX_KEYS) keyCnt = 0;
  for (uint8_t i = 0; i < keyCnt; i++) {
    int a = 1 + i * 9;
    EEPROM.get(a, keys[i]);
  }
}

void saveEEPROM() {
  EEPROM.update(0, keyCnt);
  for (uint8_t i = 0; i < keyCnt; i++) {
    int a = 1 + i * 9;
    EEPROM.put(a, keys[i]);
  }
}

bool addKey(uint8_t tp, const uint8_t* d) {
  if (keyCnt >= MAX_KEYS) return false;
  keys[keyCnt].type = tp;
  memcpy(keys[keyCnt].uid, d, 8);
  keyCnt++;
  saveEEPROM();
  return true;
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
  display.setCursor(4, 14); display.println(cursor == 1 ? "> Read RFID"   : "  Read RFID");
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
    display.print(keys[idx].type ? "RF " : "RW ");
    for (uint8_t j = 0; j < 4; j++) {
      if (keys[idx].uid[j] < 16) display.print('0');
      display.print(keys[idx].uid[j], HEX);
    }
    display.println("...");
  }
  display.display();
}

void drawSavedAction() {
  drawHeader("Key Action");
  display.setCursor(4, 12); display.println(cursor == 0 ? "> Write"   : "  Write");
  display.setCursor(4, 20); display.println(cursor == 1 ? "> Delete"  : "  Delete");
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

void printUID(const uint8_t* d) {
  drawHeader(tempTp ? "RFID UID" : "RW1990 ID");
  display.setCursor(0, 14);
  for (uint8_t i = 0; i < 8; i++) {
    if (d[i] < 16) display.print('0');
    display.print(d[i], HEX);
    if (i < 7) display.print('-');
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

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 init failed! Try 0x3D"));
    while (1);
  }

  display.clearDisplay();
  display.display();
  delay(100);

  drawLogo();

  playMusic();

  delay(1500);

  SPI.begin();
  rfid.PCD_Init();

  enc.setTimeout(380);

  loadEEPROM();
  drawMain();
}

// ────────────────────────────────────────────────
 // LOOP
// ────────────────────────────────────────────────

void loop() {
  enc.tick();

  if (enc.hasClicks()) {
    digitalWrite(LED_Y, HIGH);
    delay(35);
    digitalWrite(LED_Y, LOW);
  }

  if (busy) {
    digitalWrite(LED_Y, (millis() >> 9) & 1);
  } else {
    digitalWrite(LED_Y, LOW);
  }

  switch (mode) {

    case MAIN:
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 3) % 3;
        drawMain();
      }
      if (enc.click()) {
        if (cursor == 0) { mode = READ_RW; tmStart = millis(); busy = true; }
        if (cursor == 1) { mode = READ_MF; tmStart = millis(); busy = true; }
        if (cursor == 2) { mode = LIST; selKey = 0; drawList(); }
      }
      break;

    case READ_RW:
      if (millis() - tmStart > 7000UL) {
        mode = MAIN; drawMain(); busy = false; break;
      }
      showApply("RW1990");

      if (rw1990_read(tempBuf)) {
        tempTp = 0;
        printUID(tempBuf);
        tmStart = millis();
        mode = READ_ACTION;
      }
      delay(200);
      break;

    case READ_MF:
      if (millis() - tmStart > 7000UL) {
        mode = MAIN; drawMain(); busy = false; break;
      }
      showApply("13.56 RFID");

      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        tempTp = 1;
        memcpy(tempBuf, rfid.uid.uidByte, min(rfid.uid.size, (uint8_t)8));
        printUID(tempBuf);
        rfid.PICC_HaltA();
        tmStart = millis();
        mode = READ_ACTION;
      }
      break;

    case READ_ACTION:
      if (millis() - tmStart > 3500UL) {
        mode = MAIN; drawMain(); busy = false; break;
      }
      if (enc.click()) {
        if (addKey(tempTp, tempBuf)) okBeep();
        else errBeep();
        mode = MAIN; drawMain(); busy = false;
      }
      if (enc.hold()) {
        mode = MAIN; drawMain(); busy = false;
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
        drawSavedAction();
        mode = SAVED_ACTION;
      }
      if (enc.hold()) {
        mode = MAIN; drawMain();
      }
      break;

    case SAVED_ACTION:
      if (enc.turn()) {
        cursor = (cursor + enc.dir() + 2) % 2;
        drawSavedAction();
      }
      if (enc.click()) {
        if (cursor == 0) {  // Write
          tempTp = keys[selKey].type;
          memcpy(tempBuf, keys[selKey].uid, 8);
          mode = WRITE;
          tmStart = millis();
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
          mode = SAVED_ACTION;
          drawSavedAction();
        }
      }
      if (enc.hold()) {
        mode = SAVED_ACTION;
        drawSavedAction();
      }
      break;

    case WRITE:
      showApply(tempTp ? "RFID (magic card)" : "RW1990 blank");

      if (millis() - tmStart > 7000UL) {
        mode = LIST; drawList(); busy = false; break;
      }

      bool res = false;

      if (tempTp == 0) {
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
        display.println(tempTp ? "RFID Written OK" : "RW1990 Written OK");
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
