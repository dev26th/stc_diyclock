//
// STC15F204EA DIY LED Clock
// Copyright 2016, Jens Jensen
//

#include <stc12.h>
#include <stdint.h>

#include "config.h"
#include "adc.h"
#include "ds1302.h"
#include "led.h"
    
#define FOSC    11059200

// clear wdt
#define WDT_CLEAR()    (WDT_CONTR |= 1 << 4)

// alias for relay and buzzer outputs, using relay to drive led for indication of main loop status
#define RELAY   P1_4
#define BUZZER  P1_5
    
// adc channels for sensors
#define ADC_LIGHT 6
#define ADC_TEMP  7

// three steps of dimming. Photoresistor adc value is 0-255. Lower values = brighter.
#define DIM_HI  100
#define DIM_LO  190

// button switch aliases
#define SW2     P3_0
#define S2      1
#define SW1     P3_1
#define S1      0

// button press states
#define PRESS_NONE   0
#define PRESS_SHORT  1
#define PRESS_LONG   2

// should the DP be shown, negative logic
#define DP_OFF 0x0
#define DP_ON 0x80

// display mode states
enum display_mode {
    M_NORMAL,

    #if CFG_SET_DATE_TIME == 1
        M_SET_HOUR,
        M_SET_MINUTE,
        M_SET_MONTH,
        M_SET_DAY,
    #endif

    M_TEMP_DISP,
    M_DATE_DISP,
    M_WEEKDAY_DISP
};

/* ------------------------------------------------------------------------- */

volatile uint8_t timerTicksNow;
// delay may be only tens of ms
void _delay_ms(uint8_t ms)
{	
    uint8_t stop = timerTicksNow + ms / 10;
    while(timerTicksNow != stop);
}

// GLOBALS
uint8_t i;
uint16_t count;
uint16_t temp;    // temperature sensor value
uint8_t lightval;   // light sensor value
__bit  beep = 1;

struct ds1302_rtc rtc;
struct ram_config config;

volatile uint8_t displaycounter;
uint8_t dbuf[4];     // led display buffer, next state
uint8_t dbufCur[4];  // led display buffer, current state
uint8_t dmode = M_NORMAL;   // display mode state
uint8_t display_colon;         // flash colon

#if CFG_SET_DATE_TIME == 1
__bit  flash_hours;
__bit  flash_minutes;
__bit  flash_month;
__bit  flash_day;
#endif

volatile uint8_t debounce[2];      // switch debounce buffer
volatile uint8_t switchcount[2];

void timer0_isr() __interrupt 1 __using 1
{
    // display refresh ISR
    // cycle thru digits one at a time
    uint8_t digit = displaycounter % 4; 

    // turn off all digits, set high    
    P3 |= 0x3C;

    // auto dimming, skip lighting for some cycles
    if (displaycounter % lightval < 4 ) {
        // fill digits
        P2 = dbufCur[digit];
        // turn on selected digit, set low
        P3 &= ~((0x1 << digit) << 2);  
    }
    displaycounter++;
    // done    
}

void timer1_isr() __interrupt 3 __using 1 {
    // debounce ISR
    
    uint8_t s0 = switchcount[0];
    uint8_t s1 = switchcount[1];
    uint8_t d0 = debounce[0];
    uint8_t d1 = debounce[1];

    // debouncing stuff
    // keep resetting halfway if held long
    if (s0 > 250)
        s0 = 100;
    if (s1 > 250)
        s1 = 100;

    // increment count if settled closed
    if ((d0 & 0x0F) == 0x00)    
        s0++;
    else
        s0 = 0;
    
    if ((d1 & 0x0F) == 0x00)
        s1++;
    else
        s1 = 0;

    switchcount[0] = s0;
    switchcount[1] = s1;

    // read switch positions into sliding 8-bit window
    debounce[0] = (d0 << 1) | SW1;
    debounce[1] = (d1 << 1) | SW2;  

    ++timerTicksNow;
}

void Timer0Init(void)		//100us @ 11.0592MHz
{
    TL0 = 0xA3;		//Initial timer value
    TH0 = 0xFF;		//Initial timer value
    TF0 = 0;		//Clear TF0 flag
    TR0 = 1;		//Timer0 start run
    ET0 = 1;        // enable timer0 interrupt
    EA = 1;         // global interrupt enable
}

void Timer1Init(void)		//10ms @ 11.0592MHz
{
	TL1 = 0xD5;		//Initial timer value
	TH1 = 0xDB;		//Initial timer value
	TF1 = 0;		//Clear TF1 flag
	TR1 = 1;		//Timer1 start run
    ET1 = 1;    // enable Timer1 interrupt
    EA = 1;     // global interrupt enable
}

uint8_t getkeypress(uint8_t keynum)
{
    if (switchcount[keynum] > 150) {
        _delay_ms(30);
        return PRESS_LONG;  // ~1.5 sec
    }
    if (switchcount[keynum]) {
        _delay_ms(60);
        return PRESS_SHORT; // ~100 msec
    }
    return PRESS_NONE;
}

int8_t gettemp(uint16_t raw) {
#if CFG_TEMP_UNIT == 'F'
       // formula for ntc adc value to approx F
       // note: 354 ~= 637*5/9; 169 ~= 9*76/5+32
       return 169 - raw * 64 / 354;
#else
       // formula for ntc adc value to approx C
       return 76 - raw * 64 / 637;
#endif
}

// store display bytes
// logic is inverted due to bjt pnp drive, i.e. low = on, high = off
#define filldisplay(pos, val, dp)  dbuf[pos] = (~dp & ledtable[val])

// rotate third digit, by swapping bits fed with cba
#define rotateThirdPos() dbuf[2] = dbuf[2] & 0b11000000 | (dbuf[2] & 0b00111000) >> 3 | (dbuf[2] & 0b00000111) << 3;

/*********************************************/
int main()
{
#if CFG_HOUR_MODE == 12
    uint8_t showDp;
#endif

    // SETUP
    // set ds1302, photoresistor, & ntc pins to open-drain output, already have strong pullups
    P1M1 |= (1 << 0) | (1 << 1) | (1 << 2) | (1<<6) | (1<<7);
    P1M0 |= (1 << 0) | (1 << 1) | (1 << 2) | (1<<6) | (1<<7);
            
    // init rtc
    ds_init();
    // init/read ram config
    ds_ram_config_init((uint8_t *) &config);    
    
    // uncomment in order to reset minutes and hours to zero.. Should not need this.
    //ds_reset_clock();    
    
    Timer0Init(); // display refresh
    Timer1Init(); // switch debounce
    
    // LOOP
    while(1)
    {   
            
      RELAY = 0;
      _delay_ms(60);

      RELAY = 1;

      // run every ~1 secs
      if (count % 4 == 0) {
          lightval = getADCResult(ADC_LIGHT) >> 5;
          temp = gettemp(getADCResult(ADC_TEMP)) + config.temp_offset;

          // constrain dimming range
          if (lightval < 4) 
              lightval = 4;

      }       

      ds_readburst((uint8_t *) &rtc); // read rtc

      // display decision tree
      switch (dmode) {
          #if CFG_SET_DATE_TIME == 1 
          case M_SET_HOUR:
              display_colon = DP_ON;
              flash_hours = !flash_hours;
              if (! flash_hours) {
                  if (getkeypress(S2)) {
                      ds_hours_incr(&rtc);
                  }
                  if (getkeypress(S1))
                      dmode = M_SET_MINUTE;
              }
              break;
              
          case M_SET_MINUTE:
              flash_hours = 0;
              flash_minutes = !flash_minutes;
              if (! flash_minutes) {
                  if (getkeypress(S2)) {
                      ds_minutes_incr(&rtc);
                  }
                  if (getkeypress(S1))
                      dmode = M_NORMAL;
              }
              break;

          case M_SET_MONTH:
              flash_month = !flash_month;
              if (! flash_month) {
                  if (getkeypress(S2)) {
                      ds_month_incr(&rtc);
                  }
                  if (getkeypress(S1)) {
                      flash_month = 0;
                      dmode = M_SET_DAY;
                  }
              }
              break;
              
          case M_SET_DAY:
              flash_day = !flash_day;
              if (! flash_day) {
                  if (getkeypress(S2)) {
                      ds_day_incr(&rtc);
                  }
                  if (getkeypress(S1)) {
                      flash_day = 0;
                      dmode = M_DATE_DISP;
                  }
              }
              break;

          #endif // CFG_SET_DATE_TIME == 1 

          case M_TEMP_DISP:
              if (getkeypress(S1))
                  config.temp_offset++;
              if (getkeypress(S2))
                  dmode = M_DATE_DISP;
              break;
                        
          case M_DATE_DISP:
              #if CFG_SET_DATE_TIME == 1
              if (getkeypress(S1))
                  dmode = M_SET_MONTH;
              #endif // CFG_SET_DATE_TIME == 1 
              if (getkeypress(S2))
                  dmode = M_WEEKDAY_DISP;                        
              break;
              
          case M_WEEKDAY_DISP:
              #if CFG_SET_DATE_TIME == 1
              if (getkeypress(S1))
                  ds_weekday_incr(&rtc);
              #endif // CFG_SET_DATE_TIME == 1 
              if (getkeypress(S2))
                  dmode = M_NORMAL;
              break;
              
          case M_NORMAL:          
          default:
              if (count % 10 < 4)
                  display_colon = DP_ON; // flashing colon
              else
                  display_colon = DP_OFF;

              #if CFG_SET_DATE_TIME == 1
              flash_hours = 0;
              flash_minutes = 0;

              if (getkeypress(S1) == PRESS_LONG && getkeypress(S2) == PRESS_LONG)
                  ds_reset_clock();   
              
              if (getkeypress(S1 == PRESS_SHORT)) {
                  dmode = M_SET_HOUR;
              }
              #endif // CFG_SET_DATE_TIME == 1 

              if (getkeypress(S2 == PRESS_SHORT)) {
                  dmode = M_TEMP_DISP;
              }
      
      };

      // display execution tree
      
      switch (dmode) {
          case M_NORMAL:

          #if CFG_SET_DATE_TIME == 1
          case M_SET_HOUR:
          case M_SET_MINUTE:
              if (flash_hours) {
                  filldisplay(0, LED_BLANK, DP_OFF);
                  filldisplay(1, LED_BLANK, display_colon);
              } else 
          #endif // CFG_SET_DATE_TIME == 1
              {
                  #if CFG_HOUR_MODE == 12
                      filldisplay(0, rtc.h12.tenhour ? rtc.h12.tenhour : LED_BLANK, DP_OFF);
                  #else // CFG_HOUR_MODE == 12
                      filldisplay(0, rtc.h24.tenhour, DP_OFF);
                  #endif // CFG_HOUR_MODE == 12
                  filldisplay(1, rtc.h12.hour, display_colon);      
              }
  
              #if CFG_HOUR_MODE == 12
                  #if CFG_SET_DATE_TIME == 1
                  showDp = rtc.h12.pm ? DP_ON : DP_OFF;
                  if (flash_minutes) {
                      filldisplay(2, LED_BLANK, display_colon);
                      filldisplay(3, LED_BLANK, showDp);
                  } else
                  #endif // CFG_SET_DATE_TIME == 1
                  {
                      filldisplay(2, rtc.tenminutes, display_colon);
                      filldisplay(3, rtc.minutes, showDp);
                  }
              #else // CFG_HOUR_MODE == 12
                  #if CFG_SET_DATE_TIME == 1
                  if (flash_minutes) {
                      filldisplay(2, LED_BLANK, display_colon);
                      filldisplay(3, LED_BLANK, DP_OFF);
                  } else
                  #endif // CFG_SET_DATE_TIME == 1
                  {
                      filldisplay(2, rtc.tenminutes, display_colon);
                      filldisplay(3, rtc.minutes, DP_OFF);
                  }
              #endif // CFG_HOUR_MODE == 12
              break;

          case M_DATE_DISP:

          #if CFG_SET_DATE_TIME == 1
          case M_SET_MONTH:
          case M_SET_DAY:
              if (flash_month) {
                  filldisplay(0, LED_BLANK, DP_OFF);
                  filldisplay(1, LED_BLANK, DP_ON);
              } else
          #endif // CFG_SET_DATE_TIME == 1
              {
                  filldisplay(0, rtc.tenmonth, DP_OFF);
                  filldisplay(1, rtc.month, DP_ON);          
              }
          #if CFG_SET_DATE_TIME == 1
              if (flash_day) {
                  filldisplay(2, LED_BLANK, DP_OFF);
                  filldisplay(3, LED_BLANK, DP_OFF);              
              }
              else
          #endif // CFG_SET_DATE_TIME == 1
              {
                  filldisplay(2, rtc.tenday, DP_OFF);
                  filldisplay(3, rtc.day, DP_OFF);              
              }     
              break;
                   
          case M_WEEKDAY_DISP:
              filldisplay(0, LED_BLANK, DP_OFF);
              filldisplay(1, LED_DASH, DP_OFF);
              filldisplay(2, rtc.weekday, DP_OFF);
              filldisplay(3, LED_DASH, DP_OFF);
              break;
              
          case M_TEMP_DISP:
              filldisplay(0, ds_int2bcd_tens(temp), DP_OFF);
              filldisplay(1, ds_int2bcd_ones(temp), DP_OFF);
              #if CFG_TEMP_UNIT == 'F'
                  filldisplay(2, LED_f, DP_ON);
              #else
                  filldisplay(2, LED_c, DP_ON);
              #endif
              filldisplay(3, (temp > 0) ? LED_BLANK : LED_DASH, DP_OFF);  
              break;                  
      }
                  
      rotateThirdPos();
      dbufCur[0] = dbuf[0];
      dbufCur[1] = dbuf[1];
      dbufCur[2] = dbuf[2];
      dbufCur[3] = dbuf[3];

      // save ram config
      ds_ram_config_write((uint8_t *) &config); 
      _delay_ms(40);
      count++;
      WDT_CLEAR();
    }
}
/* ------------------------------------------------------------------------- */
