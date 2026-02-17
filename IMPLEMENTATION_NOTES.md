# RWRC Duplicator - Comprehensive Firmware Upgrade Implementation Notes

## Overview
This document describes the major firmware improvements implemented for the RWRC Duplicator, including support for multiple key types, master keys system, improved UI/UX, and scanning modes.

## Key Changes Implemented

### 1. Master Keys System (Flash Storage)

**What Changed:**
- Added `KeyType` enum with three key types: `TYPE_RW1990`, `TYPE_RFID_13M`, `TYPE_RFID_125K`
- Expanded `KeyRec` struct to include:
  - `uidLen`: Actual UID length (4-8 bytes)
  - `name[16]`: Key name (max 15 characters + null terminator)
  - `isMaster`: Boolean flag to distinguish master keys from user-added keys
- Added master keys array in PROGMEM with sample keys

**Implementation Details:**
```cpp
const KeyRec PROGMEM masterKeys[] = {
  {TYPE_RW1990, {0x01, 0xCA, 0xC9, 0xAF, 0x02, 0x00, 0x00, 0xC0}, 8, "home_78", true},
  {TYPE_RFID_13M, {0x04, 0xA1, 0xB2, 0xC3, 0x00, 0x00, 0x00, 0x00}, 4, "office_card", true},
  {TYPE_RFID_125K, {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x00, 0x00, 0x00}, 5, "garage_fob", true}
};
```

### 2. First Boot Flag System

**What Changed:**
- EEPROM byte 0 now stores a first boot flag (0x00 = not initialized, 0x01 = initialized)
- On first boot after firmware flash, master keys are loaded from PROGMEM to EEPROM
- On subsequent boots, keys are loaded from EEPROM
- This prevents master keys from being overwritten on every boot

**Implementation Details:**
- `loadEEPROM()` checks the flag and calls `loadMasterKeys()` if needed
- `loadMasterKeys()` copies master keys from PROGMEM using `memcpy_P()`
- First boot flag is set after master keys are saved

### 3. Menu Structure Updates

**What Changed:**
- Renamed mode `READ_MF` to `READ_RF`
- Renamed mode `READ_ACTION` to `READ_RESULT`
- Updated menu text from "Read RFID" to "Read RF"
- READ_RF mode now auto-detects between 13.56MHz and 125kHz (125kHz marked as TODO)

### 4. Display Format Improvements

**During RW1990 Read:**
```
RW1990 ID
──────────────────
01 CA C9 AF 02 00 00 C0
```

**During RF Read (13.56MHz):**
```
RF 13.56 MHz
──────────────────
04 A1 B2 C3
```

**In Saved Keys List:**
```
> RW home_78            (master key - shows name)
  RW CAC9 AF02 0000     (user key - shows middle 6 bytes)
  RF13 04 A1 B2 C3      (RF 13.56MHz key)
  RF125 AB CD EF 12 34  (RF 125kHz key)
```

**Implementation Details:**
- `printUID()` updated to accept length parameter and show proper headers
- `drawList()` updated to:
  - Show master key names instead of UIDs when `isMaster` is true
  - Show type-specific prefixes (RW, RF13, RF125)
  - Skip family code and CRC for RW1990 keys
  - Use `uidLen` for RF keys to show correct number of bytes

### 5. Continuous Scanning Mode

**What Changed:**
- READ_RW and READ_RF modes now scan continuously instead of one-shot
- Added tick feedback every 500ms (yellow LED + 50ms beep at 800Hz)
- Exit scanning: hold encoder for 2 seconds
- Auto-exit timeout: 15 seconds without key detection
- Upon successful detection: green LED + okBeep(), then proceed to READ_RESULT

**Implementation Details:**
- Added `inScanMode` flag to track scanning state
- Added `lastTickMs` to track 500ms tick intervals
- Added `scanHoldStartMs` to manually track 2-second hold for exit
- `tickBeep()` function plays 800Hz beep for 50ms with yellow LED
- Scanning displays "Scanning..." message with "Hold 2s to exit" instruction

### 6. LED and Buzzer Improvements

**What Changed:**
- Removed yellow LED blink on encoder click (as per requirements)
- Added green LED + okBeep() on successful key read
- Added tick feedback during scanning (yellow LED + 800Hz beep)

**Implementation Details:**
- Yellow LED now only used for:
  - Scanning tick feedback
  - Write mode busy indicator
- Green LED now used for:
  - Successful key read/write
  - OK beep indication

### 7. Factory Reset Feature

**What Changed:**
- Hold encoder for 15+ seconds from any menu to trigger factory reset
- Reset sequence:
  1. Clear EEPROM first-boot flag (set to 0x00)
  2. Display "RESETTING..." message
  3. Play 5x okBeep() for audio indication
  4. Perform soft restart using ASM jump to address 0
- Master keys automatically reload on next boot

**Implementation Details:**
- Added `holdStartMs` to manually track encoder pressing duration
- Factory reset check runs on every loop iteration
- Uses `enc.pressing()` to detect continuous hold
- ASM reset: `asm volatile ("jmp 0");`

### 8. Updated EEPROM Structure

**Old Structure:**
```
Byte 0: Key count
Bytes 1+: Keys (9 bytes per key: type + 8 bytes UID)
```

**New Structure:**
```
Byte 0: First boot flag (0x00 = not initialized, 0x01 = initialized)
Byte 1: Key count
Bytes 2+: Keys (sizeof(KeyRec) bytes per key)
  - uint8_t type
  - uint8_t uid[8]
  - uint8_t uidLen
  - char name[16]
  - bool isMaster
```

## Code Size Optimizations

To keep the code size manageable for Arduino Nano:
- Used PROGMEM for master keys array
- Used F() macro for constant strings
- Reused existing display and sound functions
- Minimal additional variables in RAM

## Hardware Requirements

- Arduino Nano (or compatible)
- MFRC522 RFID reader (13.56MHz)
- OneWire interface for RW1990 (iButton)
- SSD1306 OLED display (128x32)
- Rotary encoder with button
- Yellow and Green LEDs
- Buzzer
- (Optional) 125kHz RFID reader (not yet implemented)

## Testing Recommendations

1. **First Boot Test**: Flash firmware to device with cleared EEPROM, verify master keys load
2. **Continuous Scanning Test**: Enter READ_RW or READ_RF mode, verify tick feedback every 500ms
3. **2-Second Exit Test**: Hold encoder in scanning mode for 2 seconds, verify exit to main menu
4. **15-Second Timeout Test**: Enter scanning mode without key, verify auto-exit after 15 seconds
5. **Key Detection Test**: Present key during scanning, verify green LED + okBeep() and transition to READ_RESULT
6. **Factory Reset Test**: Hold encoder for 15+ seconds, verify reset sequence and master keys reload
7. **Display Format Test**: Add various key types, verify correct display format in saved keys list
8. **Write Test**: Write a saved key to blank, verify proper type detection and success feedback

## Known Limitations

1. **125kHz Detection**: Marked as TODO - requires additional hardware not currently available
2. **Encoder Hold Timing**: Global encoder timeout is 380ms, so 2-second and 15-second holds are manually tracked
3. **Master Key Customization**: Master keys are hardcoded in PROGMEM - users need to modify source to customize
4. **SRAM Constraints**: Limited to 10 keys total (MAX_KEYS) due to Arduino Nano SRAM limitations

## Migration Notes

When upgrading from previous firmware version:
1. Existing keys in EEPROM will be loaded if first boot flag is already set
2. If EEPROM is corrupted or from incompatible version, perform factory reset to start fresh
3. Old key structure is not compatible - users will need to re-read their keys

## Future Enhancements

Potential improvements for future versions:
1. Implement 125kHz RFID detection when hardware is available
2. Add configurable master keys via serial interface
3. Implement key name editing for user-added keys
4. Add key statistics (times used, last used timestamp)
5. Implement key backup/restore via serial interface
6. Add battery monitoring and low battery warning
