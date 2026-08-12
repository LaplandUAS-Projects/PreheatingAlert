
#include <Arduino.h>
#include <SPI.h>
#include "DFRobot_GDL.h"
#include "DFRobotDFPlayerMini.h"
#include <max6675.h>

// =============================================================
// FireBeetle 2 ESP32-E + DFR0665 (ILI9341+XPT2046, RAW-SPI + IRQ)
// + DFPlayer Mini (UART1, non-blocking init FSM)
// + 2x MAX6675 (bit-banged)
// + Big UI (two temps + setpoint) + Alarm (flash banner + audio)
// + ACK button on touch (silences alarm)
// NO blocking delays in loop() or hot paths.
// =============================================================

// ---------------- Pin map (GDI reserved pins are NOT reused) -----------------
// GDI / Display pins (fixed by FireBeetle GDI):
// VSPI: SCK=18, MISO=19, MOSI=23
// TFT:  DC=D2(25), CS=D6(14), RST=D3(26), BLK=D13(12)
// Touch: CS=D12(4), IRQ=D11(16)
// SD:    CS=D7(13)    (keep HIGH while reading touch)

// Display
#define TFT_DC    D2
#define TFT_CS    D6
#define TFT_RST   D3
#define TFT_BLK   D13
#define TOUCH_CS  D12
#define TOUCH_IRQ D11
#define SD_CS     D7

// DFPlayer (HardwareSerial1) — free pins, do not collide with GDI
static const int MP3_RX_PIN = 35;   // ESP32 RX <- DFPlayer TX
static const int MP3_TX_PIN = 15;   // ESP32 TX -> DFPlayer RX
HardwareSerial mp3Serial(1);
DFRobotDFPlayerMini myDFPlayer;
const int mp3Volume = 20;           // 0..30

// Optional audio routing (compile-time)
#ifndef AUDIO_OUT_BT
  #define AUDIO_OUT_BT 0
#endif
#ifndef AUDIO_OUT_DFPLAYER
  #define AUDIO_OUT_DFPLAYER 1
#endif

// --- Bluetooth alarm stubs (implement later) ---
#if AUDIO_OUT_BT
  void startBluetoothAlarm(){ /* TODO: user-provided BT play code */ }
  void stopBluetoothAlarm(){  /* TODO: user-provided BT stop code */ }
#else
  inline void startBluetoothAlarm(){}
  inline void stopBluetoothAlarm(){}
#endif

// MAX6675 (bit-banged) — free pins
static const int thermoSO  = 34;    // input-only OK
static const int thermoSCK = 22;    // 
static const int thermoCS  = 21;
static const int thermoCS2 = 17;    // second sensor CS 

// Potentiometer (alert setpoint)
static const int potPin = 36;       // ADC input-only

// Instantiate MAX6675
MAX6675 thermocouple(thermoSCK, thermoCS,  thermoSO);
MAX6675 thermocouple2(thermoSCK, thermoCS2, thermoSO);

// Display object
DFRobot_ILI9341_240x320_HW_SPI screen(/*dc=*/TFT_DC, /*cs=*/TFT_CS, /*rst=*/TFT_RST);

// ---------------- Touch calibration (adjust to your panel) -------------------
#define TOUCH_SWAP_XY   1
#define TOUCH_FLIP_X    1
#define TOUCH_FLIP_Y    1
#define X_RAW_MIN     300
#define X_RAW_MAX    3800
#define Y_RAW_MIN     300
#define Y_RAW_MAX    3800

// ---------------- App state ----------------
float currentTemperature  = 0.0f;
float currentTemperature2 = 0.0f;
float alertTemperature    = 30.0f;      // default °C; mapped from pot
float alertTemperatureRangeLow = 19.0f;      // alert temoperature range low threshold
float alertTemperatureRangeHigh = 200.0f;     // alert temoperature range high threshold
unsigned long lastTempUpdate = 0;
const unsigned long tempInterval = 1000;  // ms

bool   alarmActive   = false;   // both temps over threshold
bool   alarmSilenced = false;   // user acknowledged
bool   blinkOn       = false;   // UI flash toggle
unsigned long lastBlink = 0;
const unsigned long blinkPeriod = 300;    // ms

// ---------------- App banner messages ----------------
const char* currentBannerState = "";
static const char* TXT_HEATING = "Lammitetaan...";
static const char* TXT_READY   = "Valmis";
static const char* TXT_ACKED   = "Kuitattu";
static const char* TXT_ALERT   = "HALYTYS";

// ---------------- UI layout ----------------
struct Rect { int16_t x,y,w,h; };
static const Rect R_TEMP1   = {  10,  60, 140, 80 };  // left big value
static const Rect R_TEMP2   = { 170,  60, 140, 80 };  // right big value
static const Rect R_SETPT   = {  10, 135, 300, 40 };  // setpoint line
static const Rect R_BANNER  = {   0,   0, 320, 40 };  // alarm banner top

// Button
struct Button {
  int16_t x, y, w, h;
  const char* labelIdle;
  const char* labelOn;
  bool toggled;
  bool pressed;
  bool longReported;
  uint32_t tPressMs;
  Button(int16_t _x,int16_t _y,int16_t _w,int16_t _h,
         const char* li,const char* lo,
         bool tog=false,bool prs=false,bool lng=false,uint32_t tms=0)
    : x(_x),y(_y),w(_w),h(_h),labelIdle(li),labelOn(lo),
      toggled(tog),pressed(prs),longReported(lng),tPressMs(tms) {}
};
static Button btnAck(40, 185, 240, 60, "KUITTAA", "OK");

static inline void csIdle() { digitalWrite(TFT_CS, HIGH); digitalWrite(SD_CS, HIGH); }

static void drawBanner(bool on, const char* txt) {
  uint16_t fill = on ? COLOR_RGB565_RED : COLOR_RGB565_BLACK;
  screen.fillRect(R_BANNER.x, R_BANNER.y, R_BANNER.w, R_BANNER.h, fill);
  screen.drawRect(R_BANNER.x, R_BANNER.y, R_BANNER.w, R_BANNER.h, COLOR_RGB565_WHITE);
  screen.setTextSize(2);
  screen.setTextColor(COLOR_RGB565_WHITE);
  screen.setCursor(8, 12);
  screen.println(txt);
}

static void drawButton(const Button& b) {
  uint16_t fill = b.pressed ? COLOR_RGB565_DGRAY
                            : (b.toggled ? COLOR_RGB565_GREEN : COLOR_RGB565_BLUE);
  screen.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  screen.drawRoundRect(b.x, b.y, b.w, b.h, 8, COLOR_RGB565_WHITE);
  screen.setTextSize(2);
  screen.setTextColor(COLOR_RGB565_WHITE);
  const char* txt = b.toggled ? b.labelOn : b.labelIdle;
  int16_t tw = strlen(txt) * 12; // rough width for size=2
  int16_t tx = b.x + (b.w - tw)/2;
  int16_t ty = b.y + (b.h - 16)/2;
  screen.setCursor(tx, ty);
  screen.println(txt);
}

static inline bool ptInRect(const Button& b, int16_t px, int16_t py) {
  return (px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h);
}
static void setPressed(Button& b, bool p) { if (b.pressed!=p){ b.pressed=p; drawButton(b);} }

static void printBig(const Rect& r, const char* label, float value) {
  screen.fillRect(r.x, r.y, r.w, r.h, COLOR_RGB565_BLACK);
  screen.setTextSize(1);
  screen.setTextColor(COLOR_RGB565_CYAN);
  screen.setCursor(r.x, r.y - 12);
  screen.println(label);
  screen.setTextSize(4);
  screen.setTextColor(COLOR_RGB565_WHITE);
  screen.setCursor(r.x + 2, r.y + 12);
  char buf[16];
  dtostrf(value, 0, 1, buf);
  screen.print(buf);
  screen.setTextSize(2);
  screen.println(" C");
}

static void showSetpoint() {
  screen.fillRect(R_SETPT.x, R_SETPT.y, R_SETPT.w, R_SETPT.h, COLOR_RGB565_BLACK);
  screen.setTextSize(2);
  screen.setTextColor(COLOR_RGB565_YELLOW);
  screen.setCursor(R_SETPT.x, R_SETPT.y + 8);
  screen.print("Halytysraja: "); screen.print((int)alertTemperature); screen.println(" C");
}

// -------------- Touch (RAW-SPI, non-blocking FSM) --------------
static inline uint16_t xptRead(uint8_t cmd){
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  SPI.transfer(cmd);
  uint8_t hi = SPI.transfer(0x00);
  uint8_t lo = SPI.transfer(0x00);
  digitalWrite(TOUCH_CS, HIGH);
  SPI.endTransaction();
  return ((uint16_t)hi<<5) | (lo>>3);
}
static inline int xptReadPressure(){ uint16_t z1=xptRead(0xB0), z2=xptRead(0xC0); return (int)z1 + 4095 - (int)z2; }
static uint16_t median16(uint16_t *a, int n){ for(int i=1;i<n;i++){uint16_t k=a[i];int j=i-1;while(j>=0&&a[j]>k){a[j+1]=a[j];j--;}a[j+1]=k;} return a[n/2]; }
static void rawToScreen(uint16_t xr,uint16_t yr,int16_t &px,int16_t &py){ uint16_t x=xr,y=yr; 
  #if TOUCH_SWAP_XY
    {uint16_t t=x;x=y;y=t;}
  #endif
  #if TOUCH_FLIP_X
    x=X_RAW_MIN+X_RAW_MAX-x;
  #endif
  #if TOUCH_FLIP_Y
    y=Y_RAW_MIN+Y_RAW_MAX-y;
  #endif
  if(x<X_RAW_MIN)x=X_RAW_MIN; if(x>X_RAW_MAX)x=X_RAW_MAX; if(y<Y_RAW_MIN)y=Y_RAW_MIN; if(y>Y_RAW_MAX)y=Y_RAW_MAX;
  px=(int32_t)(x-X_RAW_MIN)*(screen.width()-1)/(int32_t)(X_RAW_MAX-X_RAW_MIN);
  py=(int32_t)(y-Y_RAW_MIN)*(screen.height()-1)/(int32_t)(Y_RAW_MAX-Y_RAW_MIN);
}

enum TouchState:uint8_t{T_IDLE,T_START,T_DUMMY,T_SAMPLE,T_HELD};
struct TouchCtx{TouchState st=T_IDLE; uint16_t xs[16],ys[16]; uint8_t k=0; uint32_t t0_ms=0,last_us=0; bool anyRead=false; int16_t px=0,py=0; bool inside=false;};
static TouchCtx tc;
static const uint8_t  N_SAMPLES=12; static const int Z_TH=200; static const uint32_t TS_US=300; static const uint32_t LONG_MS=300, CLICK_MS=120;

static void fsmStep(){
  switch(tc.st){
    case T_IDLE:
      if(digitalRead(TOUCH_IRQ)==LOW){ tc.st=T_START; tc.k=0; tc.anyRead=false; tc.t0_ms=millis(); tc.last_us=0; tc.inside=false; }
      break;

    case T_START:
      csIdle();
      if(xptReadPressure()<Z_TH){ tc.st=T_IDLE; break; }
      tc.st=T_DUMMY;
      break;

    case T_DUMMY:
      csIdle(); (void)xptRead(0xD0); (void)xptRead(0x90);
      tc.last_us=micros();
      tc.st=T_SAMPLE;
      break;

    // case T_SAMPLE:{
    //   if(digitalRead(TOUCH_IRQ)!=LOW){
    //     if(btnAck.pressed){ uint32_t dt=millis()-tc.t0_ms; setPressed(btnAck,false);
    //       if(tc.anyRead && dt<CLICK_MS && tc.inside){
    //         btnAck.toggled=!btnAck.toggled; drawButton(btnAck);
    //         if(alarmActive){ alarmSilenced=true; 
    //           #if AUDIO_OUT_DFPLAYER
    //             myDFPlayer.stop();
    //           #endif
    //           stopBluetoothAlarm();
    //         }
    //       }
    //     }
    //     tc.st=T_IDLE; break;
    //   }
    //   uint32_t now=micros();
    //   if(tc.last_us==0 || (now-tc.last_us)>=TS_US){
    //     tc.last_us=now; csIdle();
    //     if(xptReadPressure()<Z_TH){ tc.st=T_IDLE; break; }
    //     uint16_t xr=xptRead(0xD0), yr=xptRead(0x90);
    //     if(tc.k<N_SAMPLES){ tc.xs[tc.k]=xr; tc.ys[tc.k]=yr; tc.k++; }
    //     if(tc.k>=N_SAMPLES){ uint16_t xm[16],ym[16]; for(uint8_t i=0;i<N_SAMPLES;i++){ xm[i]=tc.xs[i]; ym[i]=tc.ys[i]; }
    //       uint16_t xrm=median16(xm,N_SAMPLES), yrm=median16(ym,N_SAMPLES);
    //       rawToScreen(xrm,yrm,tc.px,tc.py); tc.anyRead=true; tc.k=0;
    //       bool nowInside=ptInRect(btnAck, tc.px, tc.py);
    //       if(nowInside && !btnAck.pressed){ btnAck.tPressMs=millis(); btnAck.longReported=false; setPressed(btnAck,true);} 
    //       if(nowInside && btnAck.pressed && !btnAck.longReported){ uint32_t dt=millis()-btnAck.tPressMs; if(dt>=LONG_MS){ btnAck.longReported=true; } }
    //       tc.inside=nowInside; tc.st=T_HELD; }
    //   }
    // } break;

    case T_SAMPLE:{
      if(digitalRead(TOUCH_IRQ)!=LOW){
        if(btnAck.pressed){ uint32_t dt=millis()-tc.t0_ms; setPressed(btnAck,false);
          if(tc.anyRead && dt<CLICK_MS && tc.inside){
            Serial.println("Button pressed");
            btnAck.toggled=!btnAck.toggled; drawButton(btnAck);
            if(alarmActive){ alarmSilenced=true; 
              #if AUDIO_OUT_DFPLAYER
                myDFPlayer.stop();
              #endif
              stopBluetoothAlarm();
            }
          }
        }
        tc.st=T_IDLE; break;
      }
      uint32_t now=micros();
      if(tc.last_us==0 || (now-tc.last_us)>=TS_US){
        tc.last_us=now; csIdle();
        if(xptReadPressure()<Z_TH){ tc.st=T_IDLE; break; }
        uint16_t xr=xptRead(0xD0), yr=xptRead(0x90);
        if(tc.k<N_SAMPLES){ tc.xs[tc.k]=xr; tc.ys[tc.k]=yr; tc.k++; }
        if(tc.k>=N_SAMPLES){ uint16_t xm[16],ym[16]; for(uint8_t i=0;i<N_SAMPLES;i++){ xm[i]=tc.xs[i]; ym[i]=tc.ys[i]; }
          uint16_t xrm=median16(xm,N_SAMPLES), yrm=median16(ym,N_SAMPLES);
          rawToScreen(xrm,yrm,tc.px,tc.py); tc.anyRead=true; tc.k=0;

          // --- TESTI: Piirrä punainen pallo ja tulosta arvot ---
          screen.fillCircle(tc.px, tc.py, 3, COLOR_RGB565_RED);
          Serial.printf("Raw X:%u Y:%u -> Screen X:%d Y:%d\n", xrm, yrm, tc.px, tc.py);
          // ----------------------------------------------------

          bool nowInside=ptInRect(btnAck, tc.px, tc.py);
          if(nowInside && !btnAck.pressed){ btnAck.tPressMs=millis(); btnAck.longReported=false; setPressed(btnAck,true);} 
          if(nowInside && btnAck.pressed && !btnAck.longReported){ uint32_t dt=millis()-btnAck.tPressMs; if(dt>=LONG_MS){ btnAck.longReported=true; } }
          // ---- NOPEA KUITTAUS ----
            if (nowInside && alarmActive && !alarmSilenced) {

                alarmSilenced = true;

            #if AUDIO_OUT_DFPLAYER
                myDFPlayer.stop();
            #endif

                stopBluetoothAlarm();

                // Päivitä nappi "KUITATTU"-tilaan
                btnAck.labelIdle = "KUITATTU";
                drawButton(btnAck);

                // Päivitä banneri
                drawBanner(false, "KUITATTU");
                currentBannerState = TXT_ACKED;
            }
          tc.inside=nowInside; tc.st=T_HELD; }
      }
    } break;

    case T_HELD:{
      if(digitalRead(TOUCH_IRQ)!=LOW){
        if(btnAck.pressed){ uint32_t dt=millis()-tc.t0_ms; setPressed(btnAck,false);
          if(tc.anyRead && dt<CLICK_MS && tc.inside){
            btnAck.toggled=!btnAck.toggled; drawButton(btnAck);
            if(alarmActive){ alarmSilenced=true; 
              #if AUDIO_OUT_DFPLAYER
                myDFPlayer.stop();
              #endif
              stopBluetoothAlarm();
            }
          }
        }
        tc.st=T_IDLE; break;
      }
      uint32_t now=micros();
      if(tc.last_us==0 || (now-tc.last_us)>=TS_US){
        tc.last_us=now; csIdle();
        if(xptReadPressure()>=Z_TH){ uint16_t xr=xptRead(0xD0), yr=xptRead(0x90); int16_t px,py; rawToScreen(xr,yr,px,py);
          tc.px=px; tc.py=py; tc.anyRead=true; bool nowInside=ptInRect(btnAck,tc.px,tc.py);
          if(nowInside && !btnAck.pressed){ btnAck.tPressMs=millis(); btnAck.longReported=false; setPressed(btnAck,true);} 
          if(nowInside && btnAck.pressed && !btnAck.longReported){ uint32_t dt=millis()-btnAck.tPressMs; if(dt>=LONG_MS){ btnAck.longReported=true; } }
          tc.inside=nowInside; }
      }
    } break;
  }
}

// -------------- Sensors & setpoint --------------
//static void updatePot(){ int raw = analogRead(potPin); alertTemperature = map(raw, 0, 4095, alertTemperatureRangeLow, alertTemperatureRangeHigh); }
//static void updateTemperature(){ unsigned long now=millis(); Serial.println("Update temperature"); Serial.println(thermocouple.readCelsius()); if(now - lastTempUpdate >= tempInterval){ float t = thermocouple.readCelsius(); if(!isnan(t)) currentTemperature = t; float t2 = thermocouple2.readCelsius(); if(!isnan(t2)) currentTemperature2 = t2; updatePot(); lastTempUpdate = now; }}

// -------------- Sensors & setpoint --------------
static void updatePot() {
    int raw = analogRead(potPin);
    alertTemperature = map(raw, 0, 4095,
                           alertTemperatureRangeLow,
                           alertTemperatureRangeHigh);
}

static void updateTemperature() {
    unsigned long now = millis();

    //Serial.println("Update temperature");
    //Serial.println(thermocouple.readCelsius());

    if (now - lastTempUpdate >= tempInterval) {
        float t = thermocouple.readCelsius();
        if (!isnan(t))
            currentTemperature = t;

        float t2 = thermocouple2.readCelsius();
        if (!isnan(t2))
            currentTemperature2 = t2;

        updatePot();
        lastTempUpdate = now;
    }
}

// -------------- Alarm logic --------------
// static void updateAlarm(){ bool over = (currentTemperature >= alertTemperature) && (currentTemperature2 >= alertTemperature);
  
//   //Serial.println("updateAlarm function called");
//   if(over){ if(!alarmActive){ alarmActive=true; blinkOn=false; lastBlink=0; alarmSilenced=false; 
//       // start audio on entry (non-blocking)
//       #if AUDIO_OUT_DFPLAYER
//         Serial.println("DFPlayer startign to play alarm");
//         myDFPlayer.play(1);
//         Serial.println("DFPlayer play alarm");
//       #endif
//       startBluetoothAlarm();
//     }
//     if(!alarmSilenced){ unsigned long now=millis(); if(now - lastBlink >= blinkPeriod){ lastBlink = now; blinkOn = !blinkOn; drawBanner(blinkOn, "HALYTYS"); } }
//   } else {
//     if(alarmActive){ // clear alarm
//       alarmActive=false; alarmSilenced=false; blinkOn=false; drawBanner(false, "Valmis");
//       #if AUDIO_OUT_DFPLAYER
//         Serial.println("DFPlayer stopping alarm");
//         myDFPlayer.stop();
//         Serial.println("DFPlayer stop alarm");
//       #endif
//       stopBluetoothAlarm();
//     }
//   }
// }

static void updateAlarm() {
    // 1. Määritetään tilat
    bool heatingDone = (currentTemperature >= alertTemperature + 10) &&
                       (currentTemperature2 >= alertTemperature + 10);

    bool over = (currentTemperature >= alertTemperature) &&
                (currentTemperature2 >= alertTemperature);

    // --- CASE A: HÄLYTYSTILA ---
    if (over) {
        if (!alarmActive) {
            alarmActive = true;
            alarmSilenced = false;
            blinkOn = false;
            lastBlink = 0;
            #if AUDIO_OUT_DFPLAYER
                myDFPlayer.play(1);
            #endif
            startBluetoothAlarm();
        }

        // Vilkutuslogiikka (tämä saa "välkkyä" tarkoituksella punaisena)
        if (alarmActive){
            unsigned long now = millis();
            if (now - lastBlink >= blinkPeriod) {
                lastBlink = now;
                blinkOn = !blinkOn;
                drawBanner(blinkOn, TXT_ALERT);
                currentBannerState = TXT_ALERT; 
            }
        } 
        // Jos kuitattu, näytetään TXT_ACKED vain kerran
        else if (currentBannerState != TXT_ACKED) {
            drawBanner(false, TXT_ACKED);
            currentBannerState = TXT_ACKED;
        }
        return; 
    }

    // --- CASE B: NORMAALITILA (Ei hälytystä) ---
    if (alarmActive) {
        alarmActive = false;
        alarmSilenced = false;
        #if AUDIO_OUT_DFPLAYER
            myDFPlayer.stop();
        #endif
        stopBluetoothAlarm();
    }

    // Valitaan oikea teksti lämpötilan perusteella
    const char* targetTxt = heatingDone ? TXT_READY : TXT_HEATING;

    // PÄIVITYSLOGIIKKA: Piirretään vain jos teksti on eri kuin viimeksi
    if (currentBannerState != targetTxt) {
        drawBanner(false, targetTxt);
        currentBannerState = targetTxt;
    }
}

// -------------- DFPlayer non-blocking init FSM --------------
enum MP3InitState { MP3_INIT_START, MP3_INIT_WAIT, MP3_INIT_DONE, MP3_INIT_FAILED };
static MP3InitState mp3st = MP3_INIT_START;
static uint8_t  mp3Tries = 0;
static uint32_t mp3NextTryAt = 0;

static void tickDFPlayerInit() {
  if (alarmActive) return; 

  switch (mp3st) {
    case MP3_INIT_START: {   
      bool ok = myDFPlayer.begin(mp3Serial, false, false); 

      if (ok) {
        myDFPlayer.volume(mp3Volume);
        mp3st = MP3_INIT_DONE;
        Serial.println("DFPlayer init command sent");
      } else {
        mp3Tries++;
        mp3NextTryAt = millis() + 1000;
        mp3st = (mp3Tries >= 10) ? MP3_INIT_FAILED : MP3_INIT_WAIT;
        Serial.printf("DFPlayer init fail, retry %u/10\n", mp3Tries);
      }
    } break;
    case MP3_INIT_WAIT:
      if (millis() >= mp3NextTryAt) mp3st = MP3_INIT_START;
      break;
    case MP3_INIT_DONE:
    case MP3_INIT_FAILED:
    default: break;
  }
}

// -------------- Setup & Loop --------------
void setup(){
  Serial.begin(115200);
  Serial.println("\n\n--- BOOT (KAYNNISTYS) ---");

  // SPI for display/touch
  SPI.begin(18,19,23);
  pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);
  pinMode(SD_CS,  OUTPUT);  digitalWrite(SD_CS,  HIGH);
  pinMode(TOUCH_CS, OUTPUT);digitalWrite(TOUCH_CS, HIGH);
  pinMode(TFT_BLK, OUTPUT); digitalWrite(TFT_BLK, HIGH);

  // Touch IRQ
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TOUCH_IRQ), [](){ if(tc.st==T_IDLE) tc.st=T_START; }, FALLING);

  // Display
  screen.begin(); screen.setRotation(1);
  screen.fillScreen(COLOR_RGB565_BLACK);
  drawBanner(false, "Valmis");
  drawButton(btnAck);

  // // DFPlayer UART
  mp3Serial.begin(9600, SERIAL_8N1, MP3_RX_PIN, MP3_TX_PIN);
  
  // Initial UI
  Serial.println("Luetaan anturit ja päivitetään UI...");
  updateTemperature();
  printBig(R_TEMP1, "Anturi 1", currentTemperature);
  printBig(R_TEMP2, "Anturi 2", currentTemperature2);
  showSetpoint();
}

void loop(){
  // cooperative tasks (no blocking delays)
  fsmStep();
  updateTemperature();
  updateAlarm();
  tickDFPlayerInit();

  // redraw numbers only when significantly changed (reduce flicker)
  static float lastT1=-10000,lastT2=-10000,lastSet=-10000;
  if(fabs(currentTemperature - lastT1) > 0.2f){ printBig(R_TEMP1, "Anturi 1", currentTemperature); lastT1=currentTemperature; }
  if(fabs(currentTemperature2 - lastT2) > 0.2f){ printBig(R_TEMP2, "Anturi 2", currentTemperature2); lastT2=currentTemperature2; }
  if(fabs(alertTemperature - lastSet) > 0.5f){ showSetpoint(); lastSet=alertTemperature; }
}
