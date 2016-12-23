/**
 * DIY RF Laptimer by Andrey Voroshkov (bshep)
 * SPI driver based on fs_skyrf_58g-main.c by Simon Chambers
 * fast ADC reading code is by "jmknapp" from Arduino forum
 * fast port I/O code from http://masteringarduino.blogspot.com.by/2013/10/fastest-and-smallest-digitalread-and.html

The MIT License (MIT)

Copyright (c) 2016 by Andrey Voroshkov (bshep)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// #define DEBUG

#ifdef DEBUG
    #define DEBUG_CODE(x) do { x } while(0)
#else
    #define DEBUG_CODE(x) do { } while(0)
#endif

#include <avr/pgmspace.h>
#include "fastReadWrite.h"
#include "fastADC.h"
#include "pinAssignments.h"
#include "channels.h"
#include "sendSerialHex.h"
#include "rx5808spi.h"
#include "sounds.h"

#define BAUDRATE 115200

const uint16_t musicNotes[] PROGMEM = { 523, 587, 659, 698, 784, 880, 988, 1046 };

// rx5808 module needs >20ms to tune.
#define MIN_TUNE_TIME 25

// number of analog rssi reads to average for the current check.
// single analog read with FASTADC defined (see below) takes ~20us on 16MHz arduino
// so e.g. 10 reads will take 200 ms, which gives resolution of 5 RSSI reads per ms,
// this means that we can theoretically have 1ms timing accuracy :)
#define RSSI_READS 5 // 5 should give about 10 000 readings per second

// input control byte constants
#define CONTROL_START_RACE 1
#define CONTROL_END_RACE 2
#define CONTROL_DEC_MIN_LAP 3
#define CONTROL_INC_MIN_LAP 4
#define CONTROL_DEC_CHANNEL 5
#define CONTROL_INC_CHANNEL 6
#define CONTROL_DEC_THRESHOLD 7
#define CONTROL_INC_THRESHOLD 8
#define CONTROL_SET_THRESHOLD 9
#define CONTROL_DATA_REQUEST 255

//----- RSSI --------------------------------------
#define FILTER_ITERATIONS 5
uint16_t rssiArr[FILTER_ITERATIONS + 1];
uint16_t rssiThreshold = 0;
uint16_t rssi;

#define RSSI_MAX 1024
#define RSSI_MIN 0
#define MAGIC_THRESHOLD_REDUCE_CONSTANT 2
#define THRESHOLD_ARRAY_SIZE  100
uint16_t rssiThresholdArray[THRESHOLD_ARRAY_SIZE];

//----- Lap timings--------------------------------
uint32_t lastMilliseconds = 0;
#define MIN_MIN_LAP_TIME 1 //seconds
#define MAX_MIN_LAP_TIME 60 //seconds
uint8_t minLapTime = 5; //seconds
#define MAX_LAPS 100
uint32_t lapTimes[MAX_LAPS];

//----- other globals------------------------------
uint8_t allowEdgeGeneration = 1;
uint8_t channelIndex = 0;
uint8_t isRaceStarted = 0;
uint8_t newLapIndex = 0;
uint8_t sendData = 0;
uint8_t sendStage = 0;
uint8_t sendLapTimesIndex = 0;

// ----------------------------------------------------------------------------
void setup() {
    // initialize digital pin 13 LED as an output.
    pinMode(led, OUTPUT);
    digitalHigh(led);

    // init buzzer pin
    pinMode(buzzerPin, OUTPUT);

    //init raspberrypi interrupt generator pin
    pinMode(pinRaspiInt, OUTPUT);
    digitalLow(pinRaspiInt);

    // SPI pins for RX control
    setupSPIpins();

    // set the channel as soon as we can
    // faster boot up times :)
    setChannelModule(channelIndex);
    wait_rssi_ready();
    Serial.begin(BAUDRATE);

    allowEdgeGeneration = 1;

    initFastADC();

    // Setup Done - Turn Status LED off.
    digitalLow(led);

    DEBUG_CODE(
        pinMode(serialTimerPin, OUTPUT);
        pinMode(loopTimerPin, OUTPUT);
    );
}
// ----------------------------------------------------------------------------
void loop() {
    DEBUG_CODE(
        digitalToggle(loopTimerPin);
    );
    rssi = getFilteredRSSI();

    // check rssi threshold to identify when drone finishes the lap
    if (rssiThreshold > 0) { // threshold = 0 means that we don't check rssi values
        if(rssi > rssiThreshold) { // rssi above the threshold - drone is near
            if (allowEdgeGeneration) {  // we haven't fired event for this drone proximity case yet
                allowEdgeGeneration = 0;
                gen_rising_edge(pinRaspiInt);  //generate interrupt for EasyRaceLapTimer software

                uint32_t now = millis();
                uint32_t diff = now - lastMilliseconds;
                if (isRaceStarted) { // if we're within the race, then log lap time
                    if (diff > minLapTime*1000) { // if minLapTime haven't passed since last lap, then it's probably false alarm
                        if (newLapIndex < MAX_LAPS-1) { // log time only if there are slots available
                            lapTimes[newLapIndex] = diff;
                            newLapIndex++;
                        }
                        lastMilliseconds = now;
                        playLapTones(); // during the race play tone sequence even if no more laps can be logged
                        digitalLow(led);
                    }
                }
                else {
                    playLapTones(); // if not within the race, then play once per case
                    digitalLow(led);
                }
            }
        }
        else  {
            allowEdgeGeneration = 1; // we're below the threshold, be ready to catch another case
            digitalHigh(led);
        }
    }

    // send data chunk through Serial
    if (sendData) {
        DEBUG_CODE(
            digitalHigh(serialTimerPin);
        );
        switch (sendStage) {
            case 0:
                if (send4BitsToSerial('C', channelIndex)) {
                    sendStage++;
                }
                break;
            case 1:
                if (send4BitsToSerial('R', isRaceStarted)) {
                    sendStage++;
                }
                break;
            case 2:
                if (sendByteToSerial('M', minLapTime)) {
                    sendStage++;
                }
                break;
            case 3:
                if (sendIntToSerial('T', rssiThreshold)) {
                    sendStage++;
                }
                break;
            case 4:
                if (sendIntToSerial('S', rssi)) {
                    sendStage++;
                }
                break;
            case 5:
                if (sendLapTimesIndex < newLapIndex) {
                    if (sendLongToSerial('L', sendLapTimesIndex, lapTimes[sendLapTimesIndex])) {
                        sendLapTimesIndex++;
                    }
                }
                else {
                    sendStage++;
                }
                break;
            default:
                sendData = 0;
                DEBUG_CODE(
                    digitalLow(serialTimerPin);
                );
        }
    }

    // input control byte from serial (use only the last one received to keep it simple)
    uint8_t controlData = 0;
    while (Serial.available()) {
        controlData = Serial.read();
    };
    if (controlData) {
        switch (controlData) {
            case CONTROL_START_RACE: // start race
                lastMilliseconds = millis();
                newLapIndex = 0;
                isRaceStarted = 1;
                playClickTone();
                break;
            case CONTROL_END_RACE: // end race
                isRaceStarted = 0;
                newLapIndex = 0;
                playClickTone();
                break;
            case CONTROL_DEC_MIN_LAP: // decrease minLapTime
                decMinLap();
                playClickTone();
                break;
            case CONTROL_INC_MIN_LAP: // increase minLapTime
                incMinLap();
                playClickTone();
                break;
            case CONTROL_DEC_CHANNEL: // decrease channel
                decChannel();
                playClickTone();
                break;
            case CONTROL_INC_CHANNEL: // increase channel
                incChannel();
                playClickTone();
                break;
            case CONTROL_DEC_THRESHOLD: // decrease threshold
                decThreshold();
                playClickTone();
                break;
            case CONTROL_INC_THRESHOLD: // increase threshold
                incThreshold();
                playClickTone();
                break;
            case CONTROL_SET_THRESHOLD: // set threshold
                setThreshold();
                playClickTone();
                break;
            case CONTROL_DATA_REQUEST: // request data
                sendData = 1;
                sendLapTimesIndex = 0;
                sendStage = 0;
                break;
        }
    }

    if (playSound) {
        if (playStartTime == 0) {
            tone(buzzerPin,curToneSeq[curToneIndex]);
            playStartTime = millis();
        }
        uint32_t dur = millis() - playStartTime;
        if (dur >= curToneSeq[curDurIndex]) {
            if (curDurIndex == lastToneSeqIndex) {
                noTone(buzzerPin);
                playSound = 0;
            } else {
                curToneIndex += 2;
                curDurIndex += 2;
                tone(buzzerPin, curToneSeq[curToneIndex]);
                playStartTime = millis();
            }
        }
    }
}
// ----------------------------------------------------------------------------
void decMinLap() {
    if (minLapTime > MIN_MIN_LAP_TIME) {
        minLapTime--;
    }
}
// ----------------------------------------------------------------------------
void incMinLap() {
    if (minLapTime < MAX_MIN_LAP_TIME) {
        minLapTime++;
    }
}
// ----------------------------------------------------------------------------
void incChannel() {
    if (channelIndex < 7) {
        channelIndex++;
    }
    setChannelModule(channelIndex);
    wait_rssi_ready();
}
// ----------------------------------------------------------------------------
void decChannel() {
    if (channelIndex > 0) {
        channelIndex--;
    }
    setChannelModule(channelIndex);
    wait_rssi_ready();
}
// ----------------------------------------------------------------------------
void incThreshold() {
    if (rssiThreshold < RSSI_MAX) {
        rssiThreshold++;
    }
}
// ----------------------------------------------------------------------------
void decThreshold() {
    if (rssiThreshold > RSSI_MIN) {
        rssiThreshold--;
    }
}
// ----------------------------------------------------------------------------
void setThreshold() {
    if (rssiThreshold == 0) {
        uint16_t median;
        for(uint8_t i=0; i < THRESHOLD_ARRAY_SIZE; i++) {
            rssiThresholdArray[i] = getFilteredRSSI();
        }
        sortArray(rssiThresholdArray, THRESHOLD_ARRAY_SIZE);
        median = getMedian(rssiThresholdArray, THRESHOLD_ARRAY_SIZE);
        if (median > MAGIC_THRESHOLD_REDUCE_CONSTANT) {
            rssiThreshold = median - MAGIC_THRESHOLD_REDUCE_CONSTANT;
            playSetThresholdTones();
        }
    }
    else {
        rssiThreshold = 0;
        playClearThresholdTones();
    }
}
// ----------------------------------------------------------------------------
uint16_t getFilteredRSSI() {
    rssiArr[0] = readRSSI();

    // several-pass filter (need several passes because of integer artithmetics)
    // it reduces possible max value by 1 with each iteration.
    // e.g. if max rssi is 300, then after 5 filter stages it won't be greater than 295
    for(uint8_t i=1; i<=FILTER_ITERATIONS; i++) {
        rssiArr[i] = (rssiArr[i-1] + rssiArr[i]) >> 1;
    }

    return rssiArr[FILTER_ITERATIONS];
}
// ----------------------------------------------------------------------------
void sortArray(uint16_t a[], uint16_t size) {
    for(uint16_t i=0; i<(size-1); i++) {
        for(uint16_t j=0; j<(size-(i+1)); j++) {
                if(a[j] > a[j+1]) {
                    uint16_t t = a[j];
                    a[j] = a[j+1];
                    a[j+1] = t;
                }
        }
    }
}
// ----------------------------------------------------------------------------
uint16_t getMedian(uint16_t a[], uint16_t size) {
    return a[size/2];
}
// ----------------------------------------------------------------------------
void gen_rising_edge(int pin) {
    digitalHigh(pin); //this will open mosfet and pull the RasPi pin to GND
    delayMicroseconds(10);
    digitalLow(pin); // this will close mosfet and pull the RasPi pin to 3v3 -> Rising Edge
}
// ----------------------------------------------------------------------------
void wait_rssi_ready() {
    delay(MIN_TUNE_TIME);
}
// ----------------------------------------------------------------------------
uint16_t readRSSI() {
    int rssi = 0;
    int rssiA = 0;

    for (uint8_t i = 0; i < RSSI_READS; i++) {
        rssiA += analogRead(rssiPinA);
    }

    rssiA = rssiA/RSSI_READS; // average of RSSI_READS readings
    return rssiA;
}
