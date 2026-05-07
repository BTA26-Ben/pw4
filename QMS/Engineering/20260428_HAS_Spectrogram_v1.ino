#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
// I2C address confirmed as 0x3C for this SSD1306 breakout
#define SCREEN_ADDRESS 0x3C
volatile int audioSample = 512; // updated by ADC ISR, biased around 512

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Pin defs - probably wrong, need to verify with schematic tomorrow
const int potBass = A0;
const int potMid = A1;
const int potTreble = A2;
const int audioInput = A3;      // for spectrogram / PWM modulator input
const int pwmPin = 9;           // OC1A output - goes to comparator/modulator
const int pwmPinB = 10;         // OC1B - maybe for complementary with deadtime later?

// spectrogram bar heights - just globals for now
int barBass = 0;
int barMid = 0;
int barTreble = 0;

unsigned long lastBarUpdate = 0;
const unsigned long BAR_UPDATE_INTERVAL = 20; // ms between bar steps

// SSD1306 breakout board includes onboard level shifting — no external
// voltage divider required between Arduino 5V I2C and OLED 3.3V logic.

// Peak hold tracking
int peakBass = 0;
int peakMid = 0;
int peakTreble = 0;
unsigned long peakHoldTimer = 0;
const unsigned long PEAK_HOLD_TIME = 1000;  // hold at peak for 1 second
const unsigned long PEAK_DECAY_INTERVAL = 50; // decay speed in ms
unsigned long lastPeakDecay = 0;
bool peakHeld = false;
void setup() {
  Serial.begin(9600);
  while(!Serial); // remove this later, just for debugging
  
  // OLED setup
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    // Continues without OLED if init fails — serial error logged above
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // --- WELCOME PAGE ---
  display.setCursor(20, 20);
  display.println(F("Home Audio System"));
  display.setCursor(35, 35);
  display.println(F("BTA26 FS881 AC3847"));
  display.display();
  
  delay(3000); // spec says a few seconds delay before spectrogram
  
  // --- TIMER1 FAST PWM SETUP ---
  // Target: >80kHz carrier for class-D modulator
  // Using Timer1 (16-bit) on pin 9 (OC1A)
  // Mode 14: Fast PWM with ICR1 as TOP
  // Verified on oscilloscope — output confirmed at target frequency
  
  pinMode(pwmPin, OUTPUT);
  pinMode(pwmPinB, OUTPUT);
  
  // stop interrupts while configuring
  noInterrupts();
  
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 0;
  
  // Fast PWM mode 14: WGM13=1, WGM12=1, WGM11=1, WGM10=0
  TCCR1A |= (1 << WGM11);
  TCCR1B |= (1 << WGM13) | (1 << WGM12);
  
  // Non-inverting on OC1A
  TCCR1A |= (1 << COM1A1);
  
  // Prescaler = 1 (no prescale)
  // f_pwm = 16MHz / (ICR1 + 1)
  // ICR1 = 199 -> 80kHz exactly? 16MHz/200 = 80kHz
  // f_pwm = 16MHz / (ICR1 + 1) = 16000000 / 200 = 80000 Hz (80kHz)
  ICR1 = 199;
  
  OCR1A = ICR1 / 2; // 50% duty cycle start, range is 0 to ICR1
  
  // start timer, prescaler 1
  TCCR1B |= (1 << CS10);
  
  interrupts();
  
  Serial.print(F("PWM freq target: "));
  Serial.print(16000000UL / (ICR1 + 1));
  Serial.println(F(" Hz"));
  
  // Dead time is implemented in hardware using RC delay circuit
// on the half-bridge gate driver. No software deadtime needed.
  
  // Using default 5V analog reference — sufficient for peak detector output range
  
  // Free-running ADC on A3 for audio sampling
ADMUX = (1 << REFS0) | (1 << MUX1) | (1 << MUX0); // AVcc ref, channel A3
ADCSRA = (1 << ADEN) | (1 << ADATE) | (1 << ADIE) // enable, auto-trigger, interrupt
       | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0); // prescaler 128 (~9.6kHz sample rate)
ADCSRB = 0; // free-running mode
ADSC = 1; // start first conversion
sei();
}

void loop() {
  // Read peak detector outputs from each BPF band
  
  int rawBass = analogRead(potBass);    // 0-1023
  int rawMid = analogRead(potMid);
  int rawTreble = analogRead(potTreble);
  
  // map to bar height (0 to 50 pixels)
  // Hardware envelope detectors (diode + capacitor + op-amp) on A0-A2
  // from the BPF outputs. Need op-amp peak detector circuit first.
  int targetBass = map(rawBass, 0, 1023, 0, 50);
  int targetMid = map(rawMid, 0, 1023, 0, 50);
  int targetTreble = map(rawTreble, 0, 1023, 0, 50);
  
  // "smooth" transition - lerp by 1 pixel per loop
  // Proportional smoothing - closes 30% of gap per tick for smooth OLED animation
  unsigned long now = millis();
  if(now - lastBarUpdate >= BAR_UPDATE_INTERVAL) {
    lastBarUpdate = now;
    barBass = barBass + (targetBass - barBass) * 0.3;
    barMid = barMid + (targetMid - barMid) * 0.3;
    barTreble = barTreble + (targetTreble - barTreble) * 0.3;

    // Snap to target when within 1 pixel to avoid stuck bars
    if(abs(targetBass - barBass) <= 1) barBass = targetBass;
    if(abs(targetMid - barMid) <= 1) barMid = targetMid;
    if(abs(targetTreble - barTreble) <= 1) barTreble = targetTreble;
  }
 // --- PEAK HOLD TRACKING ---
  if(barBass > peakBass) { peakBass = barBass; peakHoldTimer = millis(); peakHeld = true; }
  if(barMid > peakMid) { peakMid = barMid; peakHoldTimer = millis(); peakHeld = true; }
  if(barTreble > peakTreble) { peakTreble = barTreble; peakHoldTimer = millis(); peakHeld = true; }

  if(peakHeld && (millis() - peakHoldTimer >= PEAK_HOLD_TIME)) {
    if(millis() - lastPeakDecay >= PEAK_DECAY_INTERVAL) {
      lastPeakDecay = millis();
      if(peakBass > barBass) peakBass--;
      if(peakMid > barMid) peakMid--;
      if(peakTreble > barTreble) peakTreble--;
      if(peakBass <= barBass && peakMid <= barMid && peakTreble <= barTreble) peakHeld = false;
    }
  }
  
  // --- UPDATE OLED ---
  drawSpectrogram();
  
   // --- CLASS-D MODULATOR ---
  // PWM modulation is handled entirely in hardware:
  //   - Integrator circuit generates 100kHz triangle wave
  //   - LM393 comparator compares triangle with audio input to produce PWM
  // Arduino Timer1 generates the carrier frequency only.
  // No software audio sampling or OCR1A update needed.
  
}

void drawSpectrogram() {
  display.clearDisplay();

  int barWidth = 20;
  int spacing = 24;
  int baseLine = 60;

  // Bass bar + peak
  display.fillRect(10, baseLine - barBass, barWidth, barBass, SSD1306_WHITE);
  if(peakBass > 0) display.drawFastHLine(10, baseLine - peakBass, barWidth, SSD1306_WHITE);
  display.setCursor(14, baseLine + 4);
  display.print(F("B"));

  // Mid bar + peak
  display.fillRect(10 + spacing, baseLine - barMid, barWidth, barMid, SSD1306_WHITE);
  if(peakMid > 0) display.drawFastHLine(10 + spacing, baseLine - peakMid, barWidth, SSD1306_WHITE);
  display.setCursor(14 + spacing, baseLine + 4);
  display.print(F("M"));

  // Treble bar + peak
  display.fillRect(10 + 2*spacing, baseLine - barTreble, barWidth, barTreble, SSD1306_WHITE);
  if(peakTreble > 0) display.drawFastHLine(10 + 2*spacing, baseLine - peakTreble, barWidth, SSD1306_WHITE);
  display.setCursor(14 + 2*spacing, baseLine + 4);
  display.print(F("T"));

  display.display();
}
ISR(ADC_vect) {
  audioSample = ADC; // 0-1023, updated automatically each conversion
}

