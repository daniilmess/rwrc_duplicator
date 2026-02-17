# RWRC Duplicator - State Machine and Flow Diagram

## Main Menu Flow

```
┌─────────────────────────────────────────────────────────────┐
│                        MAIN MENU                             │
│  > Read RW1990                                               │
│    Read RF                                                   │
│    Saved Keys                                                │
│                                                              │
│  Actions:                                                    │
│  - Turn encoder: Move cursor                                 │
│  - Click: Select option                                      │
│  - Hold 15s: Factory Reset                                   │
└─────────────────────────────────────────────────────────────┘
           │              │                │
           │              │                │
           ▼              ▼                ▼
    ┌──────────┐   ┌──────────┐    ┌──────────┐
    │ READ_RW  │   │ READ_RF  │    │   LIST   │
    └──────────┘   └──────────┘    └──────────┘
```

## Continuous Scanning Mode (READ_RW / READ_RF)

```
┌─────────────────────────────────────────────────────────────┐
│                    SCANNING MODE                             │
│                                                              │
│  Scanning RW1990...                                          │
│  Hold 2s to exit                                             │
│                                                              │
│  Behavior:                                                   │
│  - Yellow LED blink + 800Hz beep every 500ms (tick)          │
│  - Continuously scan for key/card                            │
│  - Hold 2s: Exit to MAIN                                     │
│  - Timeout 15s: Auto exit to MAIN                            │
│  - Key detected: Show UID → READ_RESULT                      │
└─────────────────────────────────────────────────────────────┘
                            │
                     Key Detected
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   READ_RESULT                                │
│                                                              │
│  RW1990 ID                                                   │
│  ──────────────────                                          │
│  01 CA C9 AF 02 00 00 C0                                     │
│                                                              │
│  Actions:                                                    │
│  - Green LED + okBeep() on detection                         │
│  - Click: Save key                                           │
│  - Hold: Exit to MAIN                                        │
│  - Timeout 3.5s: Exit to MAIN                                │
└─────────────────────────────────────────────────────────────┘
```

## Saved Keys List Flow

```
┌─────────────────────────────────────────────────────────────┐
│                    SAVED KEYS LIST                           │
│                                                              │
│  > RW home_78              (master key)                      │
│    RW CAC9 AF02 0000       (user key)                        │
│    RF13 04 A1 B2 C3        (RF 13.56MHz)                     │
│                                                              │
│  Actions:                                                    │
│  - Turn encoder: Select key                                  │
│  - Click: Open key action menu                               │
│  - Hold: Exit to MAIN                                        │
└─────────────────────────────────────────────────────────────┘
                            │
                          Click
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                   SAVED_ACTION                               │
│                                                              │
│  Key Action                                                  │
│  > Write                                                     │
│    Delete                                                    │
│                                                              │
│  Actions:                                                    │
│  - Turn encoder: Select action                               │
│  - Click: Execute action                                     │
│  - Hold: Back to LIST                                        │
└─────────────────────────────────────────────────────────────┘
         │                    │
       Write                Delete
         │                    │
         ▼                    ▼
   ┌──────────┐         ┌──────────────┐
   │  WRITE   │         │ CONFIRM_DELETE│
   └──────────┘         └──────────────┘
```

## Write Mode Flow

```
┌─────────────────────────────────────────────────────────────┐
│                      WRITE MODE                              │
│                                                              │
│  Apply key                                                   │
│  RW1990 blank                                                │
│  7 sec timeout                                               │
│                                                              │
│  Behavior:                                                   │
│  - Yellow LED blink (250ms on/off)                           │
│  - 1000Hz beep every 1 second                                │
│  - Wait for device to be present                             │
│  - Timeout: 7 seconds → Back to LIST                         │
└─────────────────────────────────────────────────────────────┘
                            │
                    Device Detected
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    WRITE EXECUTION                           │
│                                                              │
│  - LED off                                                   │
│  - Write data to device                                      │
│  - Verify write                                              │
│  - Show result:                                              │
│    ✓ RW1990 Written OK / Check: PASS (green LED + okBeep)   │
│    ✗ Write failed! / Check: FAIL (yellow LED + errBeep)     │
│  - Delay 2.2s → Back to LIST                                 │
└─────────────────────────────────────────────────────────────┘
```

## Factory Reset Flow

```
┌─────────────────────────────────────────────────────────────┐
│                   FACTORY RESET                              │
│                                                              │
│  Trigger: Hold encoder 15+ seconds from ANY mode             │
│                                                              │
│  Sequence:                                                   │
│  1. Clear EEPROM first-boot flag (set to 0x00)              │
│  2. Display "RESETTING..." message                           │
│  3. Play 5x okBeep() for indication                          │
│  4. Soft restart (ASM jump to 0)                             │
│  5. On next boot: Master keys reload from PROGMEM            │
└─────────────────────────────────────────────────────────────┘
```

## First Boot Sequence

```
┌─────────────────────────────────────────────────────────────┐
│                    BOOT SEQUENCE                             │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
                 ┌─────────────────────┐
                 │ Check EEPROM byte 0  │
                 └─────────────────────┘
                            │
          ┌─────────────────┴─────────────────┐
          │                                   │
   0x00 (First Boot)                   0x01 (Normal Boot)
          │                                   │
          ▼                                   ▼
┌──────────────────────┐            ┌──────────────────────┐
│ Load Master Keys     │            │ Load Keys from       │
│ from PROGMEM         │            │ EEPROM               │
│ - home_78            │            │ - Existing keys      │
│ - office_card        │            │ - User-added keys    │
│ - garage_fob         │            │                      │
│                      │            │                      │
│ Save to EEPROM       │            │                      │
│ Set flag to 0x01     │            │                      │
└──────────────────────┘            └──────────────────────┘
          │                                   │
          └─────────────────┬─────────────────┘
                            │
                            ▼
                   ┌─────────────────┐
                   │  Start Main      │
                   │  Menu            │
                   └─────────────────┘
```

## Key Type Detection

```
┌─────────────────────────────────────────────────────────────┐
│                   KEY TYPE DETECTION                         │
└─────────────────────────────────────────────────────────────┘

READ_RW Mode:
│
├─► RW1990 detected (OneWire)
│   └─► Type: TYPE_RW1990
│       uidLen: 8
│       Display: "RW1990 ID"
│
READ_RF Mode:
│
├─► RFID 13.56MHz detected (MFRC522)
│   └─► Type: TYPE_RFID_13M
│       uidLen: 4-7 (from rfid.uid.size)
│       Display: "RF 13.56 MHz"
│
└─► RFID 125kHz (TODO - hardware not available)
    └─► Type: TYPE_RFID_125K
        uidLen: 5
        Display: "RF 125 kHz"
```

## LED and Buzzer Feedback Summary

```
┌─────────────────────────────────────────────────────────────┐
│                    FEEDBACK PATTERNS                         │
└─────────────────────────────────────────────────────────────┘

Yellow LED:
├─► Scanning tick (500ms interval)
│   └─► ON for 50ms + 800Hz beep
│
├─► Write mode busy
│   └─► Blink 250ms on/off
│
└─► Error indication (errBeep)
    └─► ON during beep sequence

Green LED:
├─► Successful key read
│   └─► ON during okBeep (1.4kHz, 1.9kHz, 2.4kHz sequence)
│
└─► Successful write
    └─► ON during okBeep

Buzzer Patterns:
├─► okBeep(): 1400Hz (90ms) → 1900Hz (90ms) → 2400Hz (140ms)
├─► errBeep(): 380Hz (320ms) → 280Hz (380ms)
├─► tickBeep(): 800Hz (50ms)
└─► Write waiting: 1000Hz (50ms) every 1 second
```

## Memory Layout

```
┌─────────────────────────────────────────────────────────────┐
│                    EEPROM STRUCTURE                          │
└─────────────────────────────────────────────────────────────┘

Byte 0:           First boot flag (0x00 / 0x01)
Byte 1:           Key count (0-10)
Bytes 2+:         Key records (sizeof(KeyRec) each)
                  
KeyRec structure (per key):
  - uint8_t type           (1 byte)
  - uint8_t uid[8]         (8 bytes)
  - uint8_t uidLen         (1 byte)
  - char name[16]          (16 bytes)
  - bool isMaster          (1 byte)
  ────────────────────────────────
  Total: 27 bytes per key

Maximum keys: 10 (MAX_KEYS)
Total EEPROM used: 2 + (10 × 27) = 272 bytes
Available EEPROM: 1024 bytes (Arduino Nano)
```

## Display Format Examples

```
┌─────────────────────────────────────────────────────────────┐
│              DISPLAY FORMAT EXAMPLES                         │
└─────────────────────────────────────────────────────────────┘

RW1990 Read:
┌──────────────────────┐
│ RW1990 ID            │
│ ──────────────────   │
│ 01 CA C9 AF 02 00 00 │
│ C0                   │
└──────────────────────┘

RF 13.56MHz Read:
┌──────────────────────┐
│ RF 13.56 MHz         │
│ ──────────────────   │
│ 04 A1 B2 C3          │
└──────────────────────┘

Saved Keys List:
┌──────────────────────┐
│ Saved Keys           │
│ ──────────────────   │
│ > RW home_78         │ ← Master key (shows name)
│   RW CAC9 AF02 0000  │ ← User RW key (middle 6 bytes)
│   RF13 04 A1 B2 C3   │ ← RF 13.56MHz key
└──────────────────────┘
```
