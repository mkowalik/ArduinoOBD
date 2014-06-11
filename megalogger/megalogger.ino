/*************************************************************************
* Arduino GPS/OBD-II/G-Force Data Logger
* Distributed under GPL v2.0
* Copyright (c) 2013 Stanley Huang <stanleyhuangyc@gmail.com>
* All rights reserved.
*************************************************************************/

#include <Arduino.h>
#if OBD_MODEL == OBD_MODEL_I2C
#include <Wire.h>
#endif
#include <OBD.h>
#include <SD.h>
#include <MultiLCD.h>
#include <TinyGPS.h>
#include <MPU6050.h>
#include <SPI.h>
#include "config.h"
#include "images.h"
#if ENABLE_DATA_OUT
#include <SoftwareSerial.h>
#endif
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_CONNECTED 0x4
#define STATE_GPS_READY 0x8
#define STATE_ACC_READY 0x10
#define STATE_DATE_SAVED 0x20

#if USE_GPS
// GPS logging can only be enabled when there is additional hardware serial UART
#define GPSUART Serial2

#define PMTK_SET_NMEA_UPDATE_1HZ "$PMTK220,1000*1F"
#define PMTK_SET_NMEA_UPDATE_5HZ "$PMTK220,200*2C"
#define PMTK_SET_NMEA_UPDATE_10HZ "$PMTK220,100*2F"
#define PMTK_SET_NMEA_OUTPUT_ALLDATA "$PMTK314,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"
#define PMTK_SET_BAUDRATE "$PMTK251,115200*1F"

TinyGPS gps;

#endif

static uint8_t lastFileSize = 0;
static uint32_t lastGPSDataTime = 0;
static uint32_t lastACCDataTime = 0;
static uint8_t lastRefreshTime = 0;
static uint16_t lastSpeed = -1;
static uint16_t startDistance = 0;
static uint16_t fileIndex = 0;
static uint32_t startTime = 0;

static byte pidTier1[]= {PID_RPM, PID_SPEED, PID_ENGINE_LOAD, PID_THROTTLE};
static byte pidTier2[] = {PID_INTAKE_MAP, PID_MAF_FLOW, PID_TIMING_ADVANCE};
static byte pidTier3[] = {PID_COOLANT_TEMP, PID_INTAKE_TEMP, PID_AMBIENT_TEMP, PID_FUEL_LEVEL};

#define TIER_NUM1 sizeof(pidTier1)
#define TIER_NUM2 sizeof(pidTier2)
#define TIER_NUM3 sizeof(pidTier3)

#if DISPLAY_DASHBOARD
#include "plots.h"
CirclePlot plotSpeed;
CirclePlot plotRPM;
BarPlot plotTempCoolant;
BarPlot plotThrottle;
GPlot plotG;

void initDashboard(){
    plotSpeed.init(&lcd, 172, 0, 180, "SPEED", "km/h");
    plotRPM.init(&lcd, 20, 0, 650, "RPM", "0 rpm");
    plotTempCoolant.init(&lcd, 0, 120, 150, 100, "COOLANT", "'C");
    plotThrottle.init(&lcd, 96, 120, 100, 70, "THROTTLE", "%");
    plotG.init(&lcd, 192, 120, 70, 50, false, true, "G");
}
#endif

#if OBD_MODEL == OBD_MODEL_I2C
class COBDLogger : public COBDI2C, public CDataLogger
#else
class COBDLogger : public COBD, public CDataLogger
#endif
{
public:
    COBDLogger():state(0) {}
    void setup()
    {
        lastGPSDataTime = 0;

        showStates();

#if USE_MPU6050
        if (MPU6050_init() == 0) state |= STATE_ACC_READY;
        showStates();
#endif

#if USE_GPS
        unsigned long t = millis();
        do {
            if (GPSUART.available() && GPSUART.read() == '\r') {
                state |= STATE_GPS_CONNECTED;
                break;
            }
        } while (millis() - t <= 2000);
#endif

        do {
            showStates();
        } while (!init(OBD_PROTOCOL));

        state |= STATE_OBD_READY;

        showStates();

        //lcd.setFont(FONT_SIZE_MEDIUM);
        //lcd.setCursor(0, 14);
        //lcd.print("VIN: XXXXXXXX");

        // open file for logging
        if (!(state & STATE_SD_READY)) {
            if (checkSD()) {
                state |= STATE_SD_READY;
                showStates();
            }
        }

#if ENABLE_DATA_LOG
        uint16_t index = openFile();
        lcd.setCursor(0, 6);
        lcd.print("File ID:");
        lcd.printInt(index);
#endif

#if SHOWECUCAP
        showECUCap();
        delay(2000);
#endif

        initScreen();

        read(PID_DISTANCE, (int&)startDistance);

        lastRefreshTime = millis() >> 10;
    }
    void loop()
    {
        static byte index = 0;
        static byte index2 = 0;
        static byte index3 = 0;
        uint32_t t = millis();

        logOBDData(pidTier1[index++]);
        t = millis() - t;

        if (index == TIER_NUM1) {
            index = 0;
            if (index2 == TIER_NUM2) {
                index2 = 0;
                logOBDData(pidTier3[index3]);
                index3 = (index3 + 1) % TIER_NUM3;
            } else {
                logOBDData(pidTier2[index2++]);
            }
        }
        if (dataTime >> 10 != lastRefreshTime) {
#if !DISPLAY_DASHBOARD
            char buf[10];
            unsigned int sec = (dataTime - startTime) / 1000;
            sprintf(buf, "%02u:%02u", sec / 60, sec % 60);
            lcd.setFontSize(FONT_SIZE_MEDIUM);
            lcd.setCursor(250, 2);
            lcd.print(buf);
            lcd.setCursor(250, 11);
            lcd.printInt((uint16_t)t);
            lcd.print("ms ");
#endif
            lastRefreshTime = dataTime >> 10;
        }

        if (errors >= 10) {
            reconnect();
        }

#if USE_GPS
        if (millis() - lastGPSDataTime > GPS_DATA_TIMEOUT || gps.satellites() < 3) {
            // GPS not ready
            state &= ~STATE_GPS_READY;
        } else {
            // GPS ready
            state |= STATE_GPS_READY;
        }
#endif
    }
    bool checkSD()
    {
#if ENABLE_DATA_LOG
        Sd2Card card;
        SdVolume volume;
        state &= ~STATE_SD_READY;
        pinMode(SS, OUTPUT);
        lcd.setCursor(0, 4);

        lcd.setFontSize(FONT_SIZE_MEDIUM);
        if (card.init(SPI_FULL_SPEED, SD_CS_PIN)) {
            const char* type;
            char buf[20];

            switch(card.type()) {
            case SD_CARD_TYPE_SD1:
                type = "SD1";
                break;
            case SD_CARD_TYPE_SD2:
                type = "SD2";
                break;
            case SD_CARD_TYPE_SDHC:
                type = "SDHC";
                break;
            default:
                type = "SDx";
            }

            lcd.print(type);
            lcd.write(' ');
            if (!volume.init(card)) {
                lcd.print("No FAT!");
                return false;
            }

            uint32_t volumesize = volume.blocksPerCluster();
            volumesize >>= 1; // 512 bytes per block
            volumesize *= volume.clusterCount();
            volumesize >>= 10;

            sprintf(buf, "%dMB", (int)volumesize);
            lcd.print(buf);
        } else {
            lcd.print("No SD Card      ");
            return false;
        }

        lcd.setCursor(0, 6);
        if (!SD.begin(SD_CS_PIN)) {
            lcd.print("Bad SD");
            return false;
        }

        state |= STATE_SD_READY;
        return true;
#else
        return false;
#endif
    }
private:
    void dataIdleLoop()
    {
        // callback while waiting OBD data
        if (getState() == OBD_CONNECTED) {
            if (state & STATE_ACC_READY) {
                processAccelerometer();
            }
#if USE_GPS
            uint32_t t = millis();
            while (GPSUART.available() && millis() - t < MAX_GPS_PROCESS_TIME) {
                processGPS();
            }
#endif
            return;
        }

        // display while initializing
        char buf[10];
        unsigned int t = (millis() - startTime) / 1000;
#if !DISPLAY_DASHBOARD
        sprintf(buf, "%02u:%02u", t / 60, t % 60);
        lcd.setFontSize(FONT_SIZE_SMALL);
        lcd.setCursor(0, 28);
        lcd.print(buf);
#endif
#if USE_GPS
        // detect GPS signal
        if (GPSUART.available()) {
            char c = GPSUART.read();
            if (gps.encode(c)) {
                state |= STATE_GPS_READY;
                lastGPSDataTime = millis();

                unsigned long date, time;
                gps.get_datetime(&date, &time, 0);
                long lat, lon;
                gps.get_position(&lat, &lon, 0);
#if !DISPLAY_DASHBOARD                
                lcd.setCursor(0, 14);
                lcd.print("Time:");
                lcd.print(time);
                lcd.setCursor(0, 16);
                lcd.print("LAT: ");
                lcd.print(lat);
                lcd.setCursor(0, 18);
                lcd.print("LON: ");
                lcd.println(lon);
#endif                
            }
        }
#endif
    }
#if USE_GPS
    void processGPS()
    {
        // process GPS data
        char c = GPSUART.read();
        if (!gps.encode(c))
            return;

        // parsed GPS data is ready
        uint32_t time;
        uint32_t date;

        dataTime = millis();

        gps.get_datetime(&date, &time, 0);
        logData(PID_GPS_TIME, (int32_t)time);

        int kph = gps.speed() * 1852 / 100000;
        logData(PID_GPS_SPEED, kph);

        // no need to log GPS data when vehicle has not been moving
        // that's when previous speed is zero and current speed is also zero
        byte sat = gps.satellites();
        if (sat >= 3 && sat < 100) {
            // lastSpeed will be updated
            //ShowSensorData(PID_SPEED, speed);

            int32_t lat, lon;
            gps.get_position(&lat, &lon, 0);
            logData(PID_GPS_LATITUDE, lat);
            logData(PID_GPS_LONGITUDE, lon);
            logData(PID_GPS_ALTITUDE, (int)(gps.altitude() / 100));
#if !DISPLAY_DASHBOARD
            lcd.setFontSize(FONT_SIZE_MEDIUM);
            char buf[16];
            sprintf(buf, "%d.%ld", (int)(lat / 100000), abs(lat) % 100000);
            lcd.setCursor(50, 18);
            lcd.print(buf);
            sprintf(buf, "%d.%ld", (int)(lon / 100000), abs(lon) % 100000);
            lcd.setCursor(50, 21);
            lcd.print(buf);
            int32_t alt = gps.altitude();
            if (alt < 1000000) {
                sprintf(buf, "%dm ", (int)(alt / 100));
                lcd.setCursor(50, 24);
                lcd.print(buf);
            }
            lcd.setCursor(50, 27);
            lcd.printInt(gps.satellites());
#endif
        }
        lastGPSDataTime = dataTime;
    }
#endif
    void processAccelerometer()
    {
#if USE_MPU6050
        dataTime = millis();

        if (dataTime - lastACCDataTime < ACC_DATA_INTERVAL) {
            return;
        }

        char buf[20];
        accel_t_gyro_union data;
        MPU6050_readout(&data);
#if DISPLAY_DASHBOARD
        plotG.displayValue((int16_t)data.value.x_accel/160, (int16_t)data.value.y_accel/160);
#else        
        lcd.setFontSize(FONT_SIZE_SMALL);

        sprintf(buf, "%3d", data.value.x_accel / 160);
        lcd.setCursor(197, 21);
        lcd.print(buf);
        sprintf(buf, "%3d", data.value.y_accel / 160);
        lcd.setCursor(239, 21);
        lcd.print(buf);
        sprintf(buf, "%3d", data.value.z_accel / 160);
        lcd.setCursor(281, 21);
        lcd.print(buf);

        sprintf(buf, "%3d", data.value.x_gyro / 256);
        lcd.setCursor(197, 26);
        lcd.print(buf);
        sprintf(buf, "%3d", data.value.y_gyro / 256);
        lcd.setCursor(239, 26);
        lcd.print(buf);
        sprintf(buf, "%3d", data.value.z_gyro / 256);
        lcd.setCursor(281, 26);
        lcd.print(buf);
#endif
        // log x/y/z of accelerometer
        logData(PID_ACC, data.value.x_accel, data.value.y_accel, data.value.z_accel);
        // log x/y/z of gyro meter
        //logData(PID_GYRO, data.value.x_gyro, data.value.y_gyro, data.value.z_gyro);

        lastACCDataTime = dataTime;
#endif
    }
    void logOBDData(byte pid)
    {
        char buffer[OBD_RECV_BUF_SIZE];
        uint32_t start = millis();

        // read OBD-II data
        int value;
        sendQuery(pid);
        pid = 0;
        if (!getResult(pid, value)) {
            return;
        }

        dataTime = millis();
        // display data
        showSensorData(pid, value);

        // log data to SD card
        logData(0x100 | pid, value);

#if ENABLE_DATA_LOG
        // flush SD data every 1KB
        if ((dataSize >> 10) != lastFileSize) {
            flushFile();
            // display logged data size
#if !DISPLAY_DASHBOARD
            lcd.setFontSize(FONT_SIZE_MEDIUM);
            lcd.setCursor(250, 8);
            lcd.printInt((unsigned int)(dataSize >> 10));
            lcd.setFontSize(FONT_SIZE_SMALL);
            lcd.print(" KB");
#endif
            lastFileSize = dataSize >> 10;
        }
#endif

        // if OBD response is very fast, go on processing other data for a while
#ifdef OBD_MIN_INTERVAL
        while (millis() - start < OBD_MIN_INTERVAL) {
            dataIdleLoop();
        }
#endif
    }
    void showECUCap()
    {
        byte pidlist[] = {PID_RPM, PID_SPEED, PID_THROTTLE, PID_ENGINE_LOAD, PID_MAF_FLOW, PID_INTAKE_MAP, PID_FUEL_LEVEL, PID_FUEL_PRESSURE, PID_COOLANT_TEMP, PID_INTAKE_TEMP, PID_AMBIENT_TEMP, PID_TIMING_ADVANCE, PID_BAROMETRIC};
        const char* namelist[] = {"ENGINE RPM", "SPEED", "THROTTLE", "ENGINE LOAD", "MAF", "MAP", "FUEL LEVEL", "FUEL PRESSURE", "COOLANT TEMP", "INTAKE TEMP","AMBIENT TEMP", "IGNITION TIMING", "BAROMETER"};

        lcd.setFontSize(FONT_SIZE_MEDIUM);
        for (byte i = 0; i < sizeof(pidlist) / sizeof(pidlist[0]); i++) {
            lcd.setCursor(160, i * 2 + 4);
            lcd.print(namelist[i]);
            lcd.write(' ');
            bool valid = isValidPID(pidlist[i]);
            lcd.setColor(valid ? RGB16_GREEN : RGB16_RED);
            lcd.draw(valid ? tick : cross, 16, 16);
            lcd.setColor(RGB16_WHITE);
        }
    }
    void reconnect()
    {
#if ENABLE_DATA_LOG
        closeFile();
#endif
        lcd.clear();
        lcd.setFontSize(FONT_SIZE_MEDIUM);
        lcd.print("Reconnecting...");
        state &= ~(STATE_OBD_READY | STATE_ACC_READY | STATE_DATE_SAVED);
        //digitalWrite(SD_CS_PIN, LOW);
        for (uint16_t i = 0; ; i++) {
            if (i == 5) {
                lcd.backlight(false);
                lcd.clear();
            }
            if (init()) {
                int value;
                if (read(PID_RPM, value) && value > 0)
                    break;
            }
        }
        fileIndex++;
        setup();
    }
    byte state;

    // screen layout related stuff
    void showStates()
    {
        lcd.setFontSize(FONT_SIZE_MEDIUM);

        lcd.setCursor(0, 8);
        lcd.print("OBD ");
        lcd.setColor((state & STATE_OBD_READY) ? RGB16_GREEN : RGB16_RED);
        lcd.draw((state & STATE_OBD_READY) ? tick : cross, 16, 16);
        lcd.setColor(RGB16_WHITE);

        lcd.setCursor(0, 10);
        lcd.print("ACC ");
        lcd.setColor((state & STATE_ACC_READY) ? RGB16_GREEN : RGB16_RED);
        lcd.draw((state & STATE_ACC_READY) ? tick : cross, 16, 16);
        lcd.setColor(RGB16_WHITE);

        lcd.setCursor(0, 12);
        lcd.print("GPS ");
        if (state & STATE_GPS_READY) {
            lcd.setColor(RGB16_GREEN);
            lcd.draw(tick, 16, 16);
        } else if (state & STATE_GPS_CONNECTED)
            lcd.print("--");
        else {
            lcd.setColor(RGB16_RED);
            lcd.draw(cross, 16, 16);
        }
        lcd.setColor(RGB16_WHITE);
    }
    void showSensorData(byte pid, int value)
    {
#if DISPLAY_DASHBOARD
		switch (pid) {
        case PID_RPM:
            plotRPM.displayValue((int16_t)value / 10);
            break;
        case PID_SPEED:
            plotSpeed.displayValue((int16_t)value % 1000);
            break;
        case PID_COOLANT_TEMP:
            plotTempCoolant.displayValue((int16_t)value % 1000);
            break;
        case PID_THROTTLE:
            plotThrottle.displayValue((int16_t)value % 100);
            break;
        }
#else
        char buf[8];
        switch (pid) {
        case PID_RPM:
            lcd.setFontSize(FONT_SIZE_XLARGE);
            lcd.setCursor(34, 7);
            lcd.printInt((unsigned int)value % 10000, 4);
            break;
        case PID_SPEED:
            if (lastSpeed != value) {
                lcd.setFontSize(FONT_SIZE_XLARGE);
                lcd.setCursor(50, 2);
                lcd.printInt((unsigned int)value % 1000, 3);
                lastSpeed = value;
            }
            break;
        case PID_THROTTLE:
            lcd.setFontSize(FONT_SIZE_SMALL);
            lcd.setCursor(39, 12);
            lcd.printInt(value % 100, 3);
            break;
        case PID_INTAKE_TEMP:
            lcd.setFontSize(FONT_SIZE_SMALL);
            lcd.setCursor(102, 12);
            lcd.printInt(value % 1000, 3);
            break;
        case PID_DISTANCE:
            if ((unsigned int)value >= startDistance) {
                lcd.setFontSize(FONT_SIZE_MEDIUM);
                lcd.setCursor(250, 5);
                lcd.printInt(((unsigned int)value - startDistance) % 1000);
                lcd.print("km");
            }
            break;
        }
#endif
    }
    void initScreen()
    {
        lcd.clear();

#if DISPLAY_DASHBOARD
        initDashboard();

#else
        lcd.draw4bpp(frame0[0], 78, 58);
        lcd.setXY(164, 0);
        lcd.draw4bpp(frame0[0], 78, 58);
        lcd.setXY(0, 124);
        lcd.draw4bpp(frame0[0], 78, 58);
        lcd.setXY(164, 124);
        lcd.draw4bpp(frame0[0], 78, 58);

        //lcd.setColor(RGB16(0x7f, 0x7f, 0x7f));
        lcd.setFontSize(FONT_SIZE_SMALL);
        lcd.setCursor(110, 4);
        lcd.print("km/h");
        lcd.setCursor(110, 8);
        lcd.print("rpm");
        lcd.setCursor(15, 12);
        lcd.print("THR:    %");
        lcd.setCursor(78, 12);
        lcd.print("AIR:    C");

        //lcd.setFont(FONT_SIZE_MEDIUM);
        lcd.setCursor(185, 2);
        lcd.print("ELAPSED:");
        lcd.setCursor(185, 5);
        lcd.print("DISTANCE:");
        lcd.setCursor(185, 8);
        lcd.print("LOG SIZE:");
        lcd.setCursor(185, 11);
        lcd.print("OBD TIME:");

        lcd.setCursor(20, 18);
        lcd.print("LAT:");
        lcd.setCursor(20, 21);
        lcd.print("LON:");
        lcd.setCursor(20, 24);
        lcd.print("ALT:");
        lcd.setCursor(20, 27);
        lcd.print("SAT:");

        lcd.setFontSize(FONT_SIZE_MEDIUM);
        lcd.setCursor(185, 18);
        lcd.print("Accelerometer");
        lcd.setCursor(200, 23);
        lcd.print("Gyroscope");
        lcd.setFontSize(FONT_SIZE_SMALL);
        lcd.setCursor(185, 21);
        lcd.print("X:     Y:     Z:");
        lcd.setCursor(185, 26);
        lcd.print("X:     Y:     Z:");

        //lcd.setColor(0xFFFF);
        /*
        lcd.setCursor(32, 4);
        lcd.print("%");
        lcd.setCursor(68, 5);
        lcd.print("Intake Air");
        lcd.setCursor(112, 4);
        lcd.print("C");
        */
#endif
    }
};

static COBDLogger logger;

#if ENABLE_DATA_OUT && USE_OBD_BT
void btInit(int baudrate)
{
    logger.btInit(baudrate);
}

void btSend(byte* data, byte length)
{
    logger.btSend(data, length);
}
#endif

void setup()
{
    lcd.begin();
    lcd.setFontSize(FONT_SIZE_MEDIUM);
    lcd.backlight(true);
    lcd.setColor(0xFFE0);
    lcd.print("MEGA LOGGER - OBD-II/GPS/G-FORCE");
    lcd.setColor(RGB16_WHITE);

#if USE_GPS
#ifdef GPS_OPEN_BAUDRATE
    GPSUART.begin(GPS_OPEN_BAUDRATE);
    delay(10);
    GPSUART.println(PMTK_SET_BAUDRATE);
    GPSUART.end();
#endif
    GPSUART.begin(GPS_BAUDRATE);
    // switching to 10Hz mode, effective only for MTK3329
    //GPSUART.println(PMTK_SET_NMEA_OUTPUT_ALLDATA);
    GPSUART.println(PMTK_SET_NMEA_UPDATE_10HZ);
#endif

    logger.begin();
    logger.initSender();
    delay(500);

    logger.checkSD();
    logger.setup();
}

void loop()
{
    logger.loop();
}
