/*  Parking Gate – Final+Buzz (I2C + HC-SR04 + Servo D6 + LEDs + Buzzer D10)
    Ahmed edition – state machine + debouncing + hysteresis
    LCD I2C addr = 0x27, UNO: SDA=A4, SCL=A5
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// ---------- LCD ----------
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ---------- Pins ----------
const int PIN_TRIG  = 9;
const int PIN_ECHO  = 8;
const int PIN_SERVO = 6;
const int PIN_BUZZ  = 10;    // <— البَزّر اللطيف

const int LED_GRN1  = 2;
const int LED_GRN2  = 3;
const int LED_RED1  = 4;
const int LED_RED2  = 5;

bool LED_ACTIVE_HIGH = true;

// ---------- Thresholds & Timings ----------
const int  OPEN_TH_CM          = 20;
const int  CLOSE_TH_CM         = 28;
const int  PRESENCE_CONFIRM_MS = 400;
const int  ABSENCE_CONFIRM_MS  = 900;
const int  MIN_OPEN_MS         = 3000;
const int  GREEN_PREOPEN_MS    = 300;
const int  RED_PRECLOSE_MS     = 300;

// ---------- Servo Angles (كما ضبطتها) ----------
const int  SERVO_CLOSED_ANG    = 100;   // مغلق
const int  SERVO_OPENED_ANG    = 20;    // مفتوح
const int  SERVO_STEP_DELAY    = 6;

Servo gate;
bool  isOpen = false;

enum State { S_CLOSED, S_PREOPEN_GREEN, S_OPEN, S_PRERED, S_CLOSING };
State state = S_CLOSED;
unsigned long stateT0 = 0;
bool presenceLatched = false;
bool absenceLatched  = true;

// ---------- Helpers: LEDs ----------
void ledWrite(int pin, bool on){
  digitalWrite(pin, LED_ACTIVE_HIGH ? (on?HIGH:LOW) : (on?LOW:HIGH));
}
void setLEDs(bool g1,bool g2,bool r1,bool r2){
  ledWrite(LED_GRN1,g1); ledWrite(LED_GRN2,g2);
  ledWrite(LED_RED1,r1);  ledWrite(LED_RED2,r2);
}

// ---------- Helpers: Buzzer (لطيف غير مستمر) ----------
void chirp(unsigned int freq, unsigned int ms, unsigned int gap=40) {
  tone(PIN_BUZZ, freq, ms);
  delay(ms + gap);
  noTone(PIN_BUZZ);
}
void beepOpenPattern() {         // عند بداية الفتح
  chirp(1500, 80);
  chirp(1800, 90);
}
void beepClosePattern() {        // قبل الإغلاق
  chirp(1000, 160);
}
void beepDone() {                // عند اكتمال الحركة
  chirp(1800, 60);
}

// ---------- Distance (median of 5) ----------
long readDistanceCM(){
  long v[5];
  for(int i=0;i<5;i++){
    digitalWrite(PIN_TRIG,LOW);  delayMicroseconds(3);
    digitalWrite(PIN_TRIG,HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG,LOW);
    unsigned long dur=pulseIn(PIN_ECHO,HIGH,30000UL);
    v[i]= dur ? (dur/58) : 400;
    delay(5);
  }
  for(int i=0;i<4;i++) for(int j=i+1;j<5;j++)
    if(v[j]<v[i]){ long t=v[i]; v[i]=v[j]; v[j]=t; }
  return v[2];
}

// ---------- LCD nice messages (بدون مسافة) ----------
void lcdLine1(const char* s){
  lcd.setCursor(0,0); lcd.print(s);
  for (int i=strlen(s); i<16; i++) lcd.print(' ');
}
void lcdLine2(const char* s){
  lcd.setCursor(0,1); lcd.print(s);
  for (int i=strlen(s); i<16; i++) lcd.print(' ');
}

// presence/absence with time confirmation + hysteresis
void updatePresence(long cm){
  static unsigned long tPresenceEdge=0, tAbsenceEdge=0;
  unsigned long now=millis();

  bool rawPresence=(cm<=OPEN_TH_CM);
  if(rawPresence){
    if(!presenceLatched){
      if(tPresenceEdge==0) tPresenceEdge=now;
      if(now-tPresenceEdge>=PRESENCE_CONFIRM_MS){
        presenceLatched=true; absenceLatched=false; tAbsenceEdge=0;
      }
    }
  } else tPresenceEdge=0;

  bool rawAbsence=(cm>=CLOSE_TH_CM);
  if(rawAbsence){
    if(!absenceLatched){
      if(tAbsenceEdge==0) tAbsenceEdge=now;
      if(now-tAbsenceEdge>=ABSENCE_CONFIRM_MS){
        absenceLatched=true; presenceLatched=false; tPresenceEdge=0;
      }
    }
  } else tAbsenceEdge=0;
}

void moveServoSmooth(int fromA,int toA){
  if(fromA==toA) return;
  int step=(toA>fromA)?1:-1;
  for(int a=fromA; a!=toA; a+=step){
    gate.write(a);
    delay(SERVO_STEP_DELAY);
  }
  gate.write(toA);
}

void setup(){
  pinMode(PIN_TRIG,OUTPUT); pinMode(PIN_ECHO,INPUT);
  pinMode(LED_GRN1,OUTPUT); pinMode(LED_GRN2,OUTPUT);
  pinMode(LED_RED1,OUTPUT); pinMode(LED_RED2,OUTPUT);
  pinMode(PIN_BUZZ,OUTPUT);

  gate.attach(PIN_SERVO,500,2500);
  gate.write(SERVO_CLOSED_ANG);
  isOpen=false;

  Wire.begin(); lcd.init(); lcd.backlight(); lcd.clear();
  lcdLine1("Parking System");
  lcdLine2("Ready...");
  delay(800); lcd.clear();

  setLEDs(false,false,true,true);     // أحمرين
  lcdLine1("Gate CLOSED");
  lcdLine2("Waiting vehicle");
  state=S_CLOSED; stateT0=millis();
}

void loop(){
  long cm=readDistanceCM();
  updatePresence(cm);

  switch(state){
    case S_CLOSED:
      setLEDs(false,false,true,true);              // أحمر
      if(presenceLatched){
        setLEDs(true,true,false,false);            // أخضر قبل الفتح
        lcdLine1("Car detected!");
        lcdLine2("Opening...");
        beepOpenPattern();                         // بيبين لطيفين
        state=S_PREOPEN_GREEN; stateT0=millis();
      }
      break;

    case S_PREOPEN_GREEN:
      if(millis()-stateT0>=GREEN_PREOPEN_MS){
        moveServoSmooth(SERVO_CLOSED_ANG,SERVO_OPENED_ANG);
        beepDone();                                // تشيرب خفيفة
        isOpen=true;
        lcdLine1("Gate OPEN");
        lcdLine2("Drive safely");
        state=S_OPEN; stateT0=millis();
      }
      break;

    case S_OPEN:
      setLEDs(true,true,false,false);              // أخضرين
      // بعد مدة فتح دنيا وبمجرد تأكيد الغياب، اذهب للإغلاق
      if((millis()-stateT0>=MIN_OPEN_MS) && absenceLatched){
        setLEDs(false,false,true,true);            // أحمر مؤقتًا
        lcdLine1("Clearing zone");
        lcdLine2("Closing...");
        beepClosePattern();                        // بيب واحد أطول
        state=S_PRERED; stateT0=millis();
      }
      break;

    case S_PRERED:
      if(millis()-stateT0>=RED_PRECLOSE_MS){
        state=S_CLOSING;
      }
      break;

    case S_CLOSING:
      moveServoSmooth(SERVO_OPENED_ANG,SERVO_CLOSED_ANG);
      beepDone();                                  // تشيرب ختام
      isOpen=false;
      setLEDs(false,false,true,true);              // أحمرين
      lcdLine1("Gate CLOSED");
      lcdLine2("Waiting vehicle");
      state=S_CLOSED; stateT0=millis();
      break;
  }

  delay(12);
}
