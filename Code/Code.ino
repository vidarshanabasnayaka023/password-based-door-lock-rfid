#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <string.h>

//================ RFID =================

constexpr uint8_t SS_PIN  = 10;
constexpr uint8_t RST_PIN = 14;

MFRC522 rfid(SS_PIN, RST_PIN);

// Authorized RFID UID
const byte authorizedUID[4] = {0x9C, 0x15, 0x2E, 0x06};

//================ LCD ==================

LiquidCrystal_I2C lcd(0x27, 16, 2);

//================ Relay ================

constexpr uint8_t RELAY_PIN = 20; // Active LOW

//================ Buzzer ===============

constexpr uint8_t BUZZER_PIN = 40;

//============== Ultrasonic =============

constexpr uint8_t TRIG_PIN = 1;
constexpr uint8_t ECHO_PIN = 2;
constexpr float   DETECT_DISTANCE_CM = 50.0f;
constexpr unsigned long ECHO_TIMEOUT_US = 30000UL;

//================ Keypad ===============

constexpr byte ROWS = 4;
constexpr byte COLS = 4;

// NOTE: original layout had '5' duplicated in two rows (a bug).
// Fixed to a standard 4x4 layout.
char keys[ROWS][COLS] = {
  {'1', '4', '7', '*'},
  {'2', '5', '8', '0'},
  {'3', '6', '9', '#'},
  {'A', 'B', 'C', 'D'}
};

byte rowPins[ROWS] = {4, 5, 6, 7};
byte colPins[COLS] = {15, 16, 17, 18};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

//================ Security =============

// Fixed-size buffer instead of String: avoids heap fragmentation
// on long-running embedded devices.
const char CORRECT_PIN[5] = "1234"; // 4 digits + null terminator
constexpr uint8_t PIN_LENGTH = 4;

constexpr uint8_t MAX_ATTEMPTS = 3;
int failedAttempts = 0;

// Timeouts so the system doesn't hang forever waiting for input
// if a person walks away mid-entry.
constexpr unsigned long PIN_ENTRY_TIMEOUT_MS  = 15000UL;
constexpr unsigned long RFID_SCAN_TIMEOUT_MS  = 15000UL;

//=======================================
// LCD helper (removes repeated clear/setCursor/print blocks)
//=======================================

void lcdMessage(const char* line0, const char* line1 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line0);
  if (line1[0] != '\0') {
    lcd.setCursor(0, 1);
    lcd.print(line1);
  }
}

//=======================================
// Buzzer Functions (ESP32 Core 3.x)
//=======================================

void beepSuccess() {
  ledcWriteTone(BUZZER_PIN, 2000);
  delay(300);
  ledcWriteTone(BUZZER_PIN, 0);
}

void beepError() {
  for (int i = 0; i < 2; i++) {
    ledcWriteTone(BUZZER_PIN, 1000);
    delay(150);
    ledcWriteTone(BUZZER_PIN, 0);
    delay(150);
  }
}

void beepDetected() {
  ledcWriteTone(BUZZER_PIN, 1500);
  delay(100);
  ledcWriteTone(BUZZER_PIN, 0);
}

void alarm() {
  // NOTE: this is intentionally a hard lock requiring a physical reset,
  // matching the original design. Consider adding an admin override
  // (e.g. a hidden reset button) if that behavior isn't desired.
  while (true) {
    ledcWriteTone(BUZZER_PIN, 2500);
    delay(500);
    ledcWriteTone(BUZZER_PIN, 0);
    delay(500);
  }
}

//=======================================
// Ultrasonic Distance
//=======================================

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);

  if (duration == 0) {
    return 999;
  }

  return duration * 0.0343f / 2.0f;
}

//=======================================
// Lock System
//=======================================

void lockSystem() {
  lcdMessage("SYSTEM LOCKED", "3 FAIL ATTEMPTS");
  Serial.println("SYSTEM LOCKED");
  alarm();
}

//=======================================
// Shared "access denied" handling
// (removes duplicated logic between PIN and RFID failure paths)
//=======================================

void denyAccess(const char* reason) {
  failedAttempts++;

  lcdMessage(reason, "Access Denied");
  Serial.println(reason);
  Serial.println("Access Denied");

  delay(2000);

  if (failedAttempts >= MAX_ATTEMPTS) {
    lockSystem(); // never returns
  }
}

//=======================================
// PIN Verification
//=======================================

// Returns: 1 = correct, 0 = wrong, -1 = timed out
int verifyPIN() {
  char enteredPIN[PIN_LENGTH + 1] = {0};
  uint8_t idx = 0;

  lcdMessage("Enter PIN");
  lcd.setCursor(0, 1);

  unsigned long startTime = millis();

  while (idx < PIN_LENGTH) {
    if (millis() - startTime > PIN_ENTRY_TIMEOUT_MS) {
      Serial.println("PIN entry timed out");
      return -1;
    }

    char key = keypad.getKey();

    if (key && key >= '0' && key <= '9') {
      enteredPIN[idx++] = key;
      lcd.print("*");
      Serial.print("*");
      beepDetected();
    }
  }
  enteredPIN[PIN_LENGTH] = '\0';

  Serial.println();

  if (strncmp(enteredPIN, CORRECT_PIN, PIN_LENGTH) == 0) {
    Serial.println("PIN Correct");
    beepSuccess();
    return 1;
  }

  Serial.println("Wrong PIN");
  beepError();
  return 0;
}

//=======================================
// RFID Verification
//=======================================

// Returns: 1 = authorized, 0 = rejected, -1 = timed out
int verifyRFID() {
  lcdMessage("Scan RFID");
  Serial.println("Waiting RFID...");

  unsigned long startTime = millis();

  while (!rfid.PICC_IsNewCardPresent()) {
    if (millis() - startTime > RFID_SCAN_TIMEOUT_MS) {
      Serial.println("RFID scan timed out");
      return -1;
    }
    delay(20);
  }

  while (!rfid.PICC_ReadCardSerial()) {
    if (millis() - startTime > RFID_SCAN_TIMEOUT_MS) {
      Serial.println("RFID scan timed out");
      return -1;
    }
    delay(20);
  }

  Serial.print("UID: ");
  for (byte i = 0; i < rfid.uid.size; i++) {
    Serial.print(rfid.uid.uidByte[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  bool authorized = (rfid.uid.size == 4) &&
                     (memcmp(rfid.uid.uidByte, authorizedUID, 4) == 0);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (authorized) {
    Serial.println("RFID Accepted");
    beepSuccess();
  } else {
    Serial.println("RFID Rejected");
    beepError();
  }

  return authorized ? 1 : 0;
}

//=======================================
// Door Unlock Function
//=======================================

void unlockDoor() {
  lcdMessage("Access Granted", "Door Opening");
  Serial.println("Door Unlock");

  digitalWrite(RELAY_PIN, LOW); // Active LOW relay
  beepSuccess();

  delay(5000);

  digitalWrite(RELAY_PIN, HIGH); // Lock again

  lcdMessage("Door Locked");
  Serial.println("Door Locked");

  delay(2000);
}

//=======================================
// SETUP
//=======================================

void setup() {
  Serial.begin(115200);

  Wire.begin(8, 9);              // LCD I2C
  SPI.begin(12, 13, 11, 10);     // RFID SPI
  rfid.PCD_Init();

  lcd.init();
  lcd.backlight();

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Active LOW relay, start locked

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  ledcAttach(BUZZER_PIN, 2000, 8); // ESP32 Core 3.x
  ledcWriteTone(BUZZER_PIN, 0);

  lcdMessage("Door Security", "System Ready");
  Serial.println("System Ready");

  delay(2000);
}

//=======================================
// LOOP
//=======================================

void loop() {
  float distance = getDistance();

  // Waiting for person
  if (distance > DETECT_DISTANCE_CM) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Waiting Person");
    lcd.setCursor(0, 1);
    lcd.print("Dist:");
    lcd.print((int)distance);
    lcd.print(" cm");

    delay(500);
    return;
  }

  // Person detected
  Serial.println("Person Detected");
  beepDetected();
  lcdMessage("Person Found", "Enter PIN");
  delay(1000);

  //================ PIN =================
  int pinResult = verifyPIN();

  if (pinResult == -1) {
    // Timed out waiting for input; just go back to idle, don't count as a failed attempt
    lcdMessage("No Input", "Returning...");
    delay(1500);
    return;
  }

  if (pinResult == 0) {
    denyAccess("Wrong PIN");
    return;
  }

  lcdMessage("PIN Correct");
  delay(1000);

  //================ RFID ================
  int rfidResult = verifyRFID();

  if (rfidResult == -1) {
    lcdMessage("No Card", "Returning...");
    delay(1500);
    return;
  }

  if (rfidResult == 0) {
    denyAccess("Invalid RFID");
    return;
  }

  // Successful access
  failedAttempts = 0;
  unlockDoor();

  lcdMessage("Waiting Person", "System Ready");
  delay(1000);
}
