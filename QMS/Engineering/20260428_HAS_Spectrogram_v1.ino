#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
// TODO: check if this address is right, might be 0x3D depending on board
#define SCREEN_ADDRESS 0x3C

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

void setup() {
  Serial.begin(9600);
  while(!Serial); // remove this later, just for debugging
  
  // OLED setup
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    // TODO: maybe hang here or retry? for now just keep going
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // --- WELCOME PAGE ---
  display.setCursor(20, 20);
  display.println(F("Home Audio System"));
  display.setCursor(35, 35);
  display.println(F("by [Your Name]"));
  display.display();
  
  delay(3000); // spec says a few seconds delay before spectrogram
  
  // --- TIMER1 FAST PWM SETUP ---
  // Target: >80kHz carrier for class-D modulator
  // Using Timer1 (16-bit) on pin 9 (OC1A)
  // Mode 14: Fast PWM with ICR1 as TOP
  // TODO: verify frequency with scope, this math might be off
  
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
  
  // TODO: set up Timer1B for complementary output with deadtime?
  // ATmega328P doesn't do hardware deadtime so might need external gate driver
  // or second timer / logic gates. Ask TA about this.
  
  // TODO: analog reference? using default 5V for now but audio signal is small
}

void loop() {
  // --- SPECTROGRAM DATA ACQUISITION ---
  // FIXME: this is totally fake right now, just reading pots because
  // we don't have the peak detector circuit or BPF outputs wired to ADC yet
  
  int rawBass = analogRead(potBass);    // 0-1023
  int rawMid = analogRead(potMid);
  int rawTreble = analogRead(potTreble);
  
  // map to bar height (0 to 50 pixels)
  // TODO: replace with actual envelope detector / peak detector readings
  // from the BPF outputs. Need op-amp peak detector circuit first.
  int targetBass = map(rawBass, 0, 1023, 0, 50);
  int targetMid = map(rawMid, 0, 1023, 0, 50);
  int targetTreble = map(rawTreble, 0, 1023, 0, 50);
  
  // "smooth" transition - lerp by 1 pixel per loop
  // TODO: this is too slow / too fast depending on loop time, need millis() based
   unsigned long now = millis();
  if(now - lastBarUpdate >= BAR_UPDATE_INTERVAL) {
    lastBarUpdate = now;
    if(barBass < targetBass) barBass++;
    else if(barBass > targetBass) barBass--;
  
    if(barMid < targetMid) barMid++;
    else if(barMid > targetMid) barMid--;
  
    if(barTreble < targetTreble) barTreble++;
    else if(barTreble > targetTreble) barTreble--;
}
  
  // --- UPDATE OLED ---
  drawSpectrogram();
  
  // --- CLASS-D MODULATOR ---
  // TODO: this is where we should update OCR1A based on audio input
  // to do PWM. Right now it's just fixed 50% duty cycle.
  // Need to:
  // 1. Sample audio input (fast! need to check how fast analogRead is)
  // 2. Scale it to match ICR1 range
  // 3. Update OCR1A
  // 4. Add triangle wave comparison? Or is the timer generating the triangle?
  //    Actually wait - the project says use integrator circuit for triangle wave
  //    and comparator for PWM. So Arduino might just generate the carrier,
  //    not the modulated PWM... need to re-read spec.
  
  // int audioSample = analogRead(audioInput); // 0-1023, need to bias around 2.5V?
  // OCR1A = map(audioSample, 0, 1023, 0, ICR1); // this is probably wrong, needs offset
  
  // FIXME: analogRead is too slow (100us) to do proper audio sampling in loop
  // need to use timer interrupt or free-running ADC mode
  
}

void drawSpectrogram() {
  display.clearDisplay();
  
  // draw 3 bars
  int barWidth = 20;
  int spacing = 24;
  int baseLine = 60;
  
  // Bass
  display.fillRect(10, baseLine - barBass, barWidth, barBass, SSD1306_WHITE);
  display.setCursor(14, baseLine + 4);
  display.print(F("B"));
  
  // Mid
  display.fillRect(10 + spacing, baseLine - barMid, barWidth, barMid, SSD1306_WHITE);
  display.setCursor(14 + spacing, baseLine + 4);
  display.print(F("M"));
  
  // Treble
  display.fillRect(10 + 2*spacing, baseLine - barTreble, barWidth, barTreble, SSD1306_WHITE);
  display.setCursor(14 + 2*spacing, baseLine + 4);
  display.print(F("T"));
  
  display.display();
}

// TODO: add interrupt service routine for fast ADC sampling
// ISR(ADC_vect) { ... }

// TODO: add deadtime generation function
// maybe using delayMicroseconds? but that's bad practice
// probably need external RC delay + logic gates or dedicated gate driver IC

// TODO: implement actual peak detector in software?
// right now we're just reading DC pot values
// need to rectify and envelope detect the AC audio signal
// could do in hardware with diode+capacitor+op amp
// or in software with max value over a window

// NOTES FROM LAB:
// - OLED works but text is a little dim, maybe contrast setting?
// - Timer output on pin 9 is square wave at ~160kHz per scope, good
// - Haven't wired the comparator yet, so no real PWM modulation
// - Need 2nd breadboard tomorrow for class-D output stage
// - Forgot to add 0.1uF caps across op amp power pins, do that next time
// - Spectrogram bars move but they're just reading pot knobs, not audio
