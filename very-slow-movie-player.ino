#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM !!!"
#endif

#include <Arduino.h>
#include <SPI.h>
#include <SD_MMC.h>
#include "pins.h"
#include "epd_driver.h"

#include "firasans.h"
#include "Filename.h"
#include "MyUtils.h"
#include "JpegUtils.h"
#include "DrawFuncs.h"

// update more frequently in debug mode
// #define DEBUG

// threshold of frame values average to consider a frame as dark
#define DARK_PX_RATIO_TH 0.80

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */

// Track number of wakeups
RTC_DATA_ATTR int bootCount = 0;

//--------------------
// Config parameters
//--------------------
/*
    Time ESP32 will go to sleep in seconds
    (larger values - longer battery life)
*/
#ifdef DEBUG
  #define TIME_TO_SLEEP_S 5
#else
  #define TIME_TO_SLEEP_S 120
#endif

/*
    How many frames to advance each update
    Value is a matter of taste.
    Lower means more uniform pictures,
    higher gives more variance but some scenes may be missed
*/
#define FRAMES_DELTA 1

/*
   Whether to use high quality screen clearing.
   False: some ghosting may show.
   True: no ghosting but uses more power
*/
#define HQ_CLEAR false //deprecated

/*
   Whether to use different draw logic for dark frames
   False: dark frames look closer to the original but
     details may not show as they are too dark
   True: dark frames look better.
*/
#define DARK_FRAME_ENH true

/*
   How many frames to wait between saving current file number.
   Lower values means less frames will repeat in case of reset
   but more writes to the SD card.
*/
#define SAVE_FILE_NUM_EVERY 10   /* How many frames to wait between saving file number */

//#define SHOW_LOG
//#define SHOW_BARS

// frame buffers
uint8_t *framebuffer;
uint8_t *nframebuffer;

void updateDisplay() {
  // Measure wake time
  volatile uint32_t t1 = millis();

  char logBuf[128];

  //--------------------------
  // Read voltage
  //--------------------------
  volatile float battery_voltage = MyUtils::readVoltage();
  String voltage = "➸ Voltage : " + String(battery_voltage) + "V";
  Serial.println(voltage);

  //--------------------------
  // Read folder name from SD
  //--------------------------
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.printf("Wakeup reason %d\n", wakeup_reason);
  if (wakeup_reason == 0) {
    Serial.printf("Wakeup after reset\n");
    // read folder from file
    Filename::readFromSd();
  }

  //--------------------------
  // Init frame buffers
  //--------------------------
  framebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
  if (!framebuffer) {
    Serial.println("alloc memory failed !!!");
    ESP.restart();
  }
  memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

  if (HQ_CLEAR) {
    nframebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
    memset(nframebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  }

  //--------------------------
  // Draw to frame buffers
  //--------------------------
  // First, draw last frame for erasing if needed
  if (HQ_CLEAR) {
    JpegUtils::drawFileToBuffer(Filename::getPath(), nframebuffer);
  }

  // Now draw new frame
  Serial.println("Advancing frame");
  Filename::advance(FRAMES_DELTA);
  if (Filename::fileNumber > 0) {
    Serial.printf("Draw new %s\n", Filename::getPath());
    JpegUtils::drawFileToBuffer(Filename::getPath(), framebuffer);
  } else {
    int cursor_x = 420;
    int cursor_y = 290;
    writeln((GFXfont *)&FiraSans, "The End", &cursor_x, &cursor_y, framebuffer);
  }

  // We persist file details to SD card every SAVE_FILE_NUM_EVERY frames
  // To continue where we stopped in case of power loss
  if (Filename::fileNumber % SAVE_FILE_NUM_EVERY == 0) {
    Filename::writeToSd();
  }

  sprintf(
    logBuf,
    "%s, b# %d, v: %.2f, Err: %d",
    Filename::getPath(),
    bootCount,
    battery_voltage,
    JpegUtils::lastError
  );
  Serial.println(logBuf);

#ifdef SHOW_LOG
  epd_fill_rect(50, 460, 860, 50, 0xff, framebuffer);
  epd_fill_rect(40, 475, 20, 20, 0x00, framebuffer);
  epd_fill_rect(900, 475, 20, 20, 0x00, framebuffer);

  int cursor_x = 60;
  int cursor_y = 500;

  writeln((GFXfont *)&FiraSans, logBuf, &cursor_x, &cursor_y, framebuffer);
#endif

#ifdef SHOW_BARS
  uint8_t c1 = bootCount % 2 ? 0xff : 0x00;
  uint8_t c2 = bootCount % 2 ? 0x00 : 0xff;

  epd_fill_rect(0, 0, 480, 10, c1, framebuffer);
  epd_fill_rect(480, 0, 480, 10, c2, framebuffer);
  epd_fill_rect(0, 530, 480, 10, c2, framebuffer);
  epd_fill_rect(480, 530, 480, 10, c1, framebuffer);
#endif

  // Display low battery indicator if needed
  MyUtils::renderBattery(framebuffer, battery_voltage);

  //--------------------------
  // Draw from frame buffers to display
  //--------------------------
  if (DARK_FRAME_ENH) {
    float dRatio = MyUtils::calcPartialDarkRatio(framebuffer, 20);
    
    if (dRatio > DARK_PX_RATIO_TH) {
      DrawFuncs::drawDark(framebuffer);
    } else {
      DrawFuncs::drawLight(framebuffer);
    }
  } else {
    DrawFuncs::drawLight(framebuffer);
  }

  // Report awake time
  volatile uint32_t t2 = millis();
  Serial.printf("Update took %dms.\n", t2 - t1);
}

void rebootWithReason(String reason) {
    Serial.println(reason);
    // sleep for 1s so that we ease the boot loop
    Serial.println("Waiting 1s before restart");
    delay(1000L);
    ESP.restart();
}

void setup()
{
  Serial.begin(115200);

  // Track reboots
  ++bootCount;
  Serial.printf("****** Boot number %d ******\n", bootCount);

  //--------------------------
  // Init SD card
  //--------------------------
#if defined(T5_47_PLUS)
  if(!SD_MMC.setPins(SD_SCLK, SD_MOSI, SD_MISO)){
    rebootWithReason("SD card pin configuration failed!");
  }
#endif

  if(!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)){
    rebootWithReason("SD init failed");
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("SD card could not be read, insert a valid SD then reboot");
    return;
  }

  Serial.printf("Detected SdCard (type %d) insert:%.2f GB\n", SD_MMC.cardType(), (SD_MMC.cardSize() / (1024.0 * 1024.0 * 1024.0)));

  //--------------------
  // Draw new frame
  //--------------------
  epd_init();
  delay(10L);
  epd_poweron();
  delay(10L);
  updateDisplay();

  // Turn off the power of the entire
  // POWER_EN control and also turn off the blue LED light
  epd_poweroff_all();

  //------------------------------
  // Go to sleep
  //------------------------------
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_S * uS_TO_S_FACTOR);
  Serial.println("Going to sleep now for " + String(TIME_TO_SLEEP_S) + " seconds");
  esp_deep_sleep_start();
}

void loop()
{
  return;
}
