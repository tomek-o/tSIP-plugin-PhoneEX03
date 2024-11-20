#include "CommThread.h"
#include "..\tSIP\tSIP\phone\Phone.h"
#include "HidDevice.h"
#include "Log.h"
#include <windows.h>
#include <assert.h>
#include <time.h>


// buf[17] LSB = ikona telefonu, bit 3?: antena
// buf[18]: dziesi¹tki miesiêcy + TUE
// buf[19]: LSB = WED, dalej miesi¹ce
// [20] myœlnik i THU
// 22: dni i SAT
// 23: sekundy i MON (ale MON nie jest LSB)
// 24: dziesi¹tki sekund i SUN
// 25: minuty i dwukropek
// 26: dziesi¹tki minut i drugi dwukropek
// 27: godziny i najwysza kreska zasiêgu
// 32: pierwsza cyfra od lewej i najni¿sza kreska zasiêgu
// 33: nic?

/*
{
    // ring? LED?
    unsigned char buf[33] = {
0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memcpy(sendbuf, buf, sizeof(sendbuf));
    status = hidDevice.WriteReport(HidDevice::E_REPORT_OUT, 0, sendbuf, sizeof(sendbuf));
}

{
    // ring OFF
    unsigned char buf[33] = {
0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memcpy(sendbuf, buf, sizeof(sendbuf));
    status = hidDevice.WriteReport(HidDevice::E_REPORT_OUT, 0, sendbuf, sizeof(sendbuf));
}
*/

void Key(int keyCode, int state);

using namespace nsHidDevice;

namespace {
volatile bool connected = false;
volatile bool exited = false;

int VendorID = 0x04B4;
int ProductID = 0x0302;

int regState = 0;
int callState = 0;
int ringState = 0;
std::string callDisplay;
bool displayUpdateFlag = false;
bool ringUpdateFlag = false;

void UpdateButtons(unsigned char oldData, unsigned char data) {
    LOG("Update buttons %02X -> %02X", oldData, data);
    enum E_KEY key;
    unsigned char rawKey = data;
    if (rawKey == 0) {
        rawKey = oldData;
    }
    switch (rawKey) {
    case 0x15:
        key = KEY_0;
        break;
    case 0x04:
        key = KEY_1;
        break;
    case 0x03:
        key = KEY_2;
        break;
    case 0x02:
        key = KEY_3;
        break;
    case 0x0A:
        key = KEY_4;
        break;
    case 0x09:
        key = KEY_5;
        break;
    case 0x08:
        key = KEY_6;
        break;
    case 0x10:
        key = KEY_7;
        break;
    case 0x0F:
        key = KEY_8;
        break;
    case 0x0E:
        key = KEY_9;
        break;
    case 0x16:
        key = KEY_STAR;
        break;
    case 0x14:
        key = KEY_HASH;
        break;
    case 0x07:
        key = KEY_OK;
        break;
    case 0x01:
        key = KEY_C;
        break;
    case 0x0D:
        key = KEY_UP;
        break;
    case 0x13:
        key = KEY_DOWN;
        break;
    default:
        LOG("Unhandled raw key code 0x%02X", rawKey);
        return;
    }
    Key(key, data?1:0);
}

void UpdateHook(unsigned char data) {
    LOG("Update hook %02X", data);
    if (data) {
        Key(KEY_HOOK, 0);
    } else {
        Key(KEY_HOOK, 1);
    }
}

// row 1 uses different scheme then row2
void FillDigit(int row, const char src, char& dst) {
    if (row == 0) {
        dst &= 0xFE;    // digit mask
    } else {
        dst &= 0xEF;
    }

    /*
     A
    F B
     G
    E C
     D  DP
    */
    enum {
        SEG_A = 0x08,
        SEG_B = 0x04,
        SEG_C = 0x02,
        SEG_D = 0x10,
        SEG_E = 0x20,
        SEG_F = 0x80,
        SEG_G = 0x40,
        SEG_DP = 0x01
    };

    unsigned char val = 0;
    switch (src) {
    case '0':
        val = SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        break;
    case '1':
        val = SEG_B | SEG_C;
        break;
    case '2':
        val = SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
        break;
    case '3':
        val = SEG_A | SEG_B | SEG_C | SEG_D | SEG_G;
        break;
    case '4':
        val = SEG_F | SEG_G | SEG_B | SEG_C;
        break;
    case '5':
        val = SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
        break;
    case '6':
        val = SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        break;
    case '7':
        val = SEG_A | SEG_B | SEG_C;
        break;
    case '8':
        val = SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        break;
    case '9':
        val = SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        break;
    case '-':
        val = SEG_G;
        break;
    case '_':
        val = SEG_D;
        break;
    default:
        break;
    }

    if (row == 1) {
        // swap nibbles
        val = ((val & 0x0F) << 4) | ((val & 0xF0) >> 4);
    }
    dst |= val;
}

struct DisplayExtras {
    bool colon1;
    bool colon2;
    bool mon;
    bool tue;
    bool wed;
    bool thu;
    bool fri;
    bool sat;
    bool sun;
    bool antenna;
    bool phone;
    DisplayExtras(void) {
        memset(this, 0, sizeof(*this));
    }
};

void FillDisplayBuf(char* buf, const char* line1, const char* line2, DisplayExtras &extras) {
    // buf[17] LSB = ikona telefonu, bit 3?: antena
    // buf[18]: dziesiątki miesięcy + TUE
    // buf[19]: LSB = WED, dalej miesiące
    // [20] myślnik i THU
    // 22: dni i SAT
    // 23: sekundy i MON (ale MON nie jest LSB)
    // 24: dziesiątki sekund i SUN
    // 25: minuty i dwukropek
    // 26: dziesiątki minut i drugi dwukropek
    // 27: godziny i najwysza kreska zasięgu
    // 32: pierwsza cyfra od lewej i najniższa kreska zasięgu
    // 33: nic?

    FillDigit(0, line1[0], buf[18]);
    FillDigit(0, line1[1], buf[19]);
    FillDigit(0, line1[2], buf[20]);
    FillDigit(0, line1[3], buf[21]);
    FillDigit(0, line1[4], buf[22]);

    FillDigit(1, line2[0], buf[32]);
    FillDigit(1, line2[1], buf[31]);
    FillDigit(1, line2[2], buf[30]);
    FillDigit(1, line2[3], buf[29]);
    FillDigit(1, line2[4], buf[28]);
    FillDigit(1, line2[5], buf[27]);
    FillDigit(1, line2[6], buf[26]);
    FillDigit(1, line2[7], buf[25]);
    FillDigit(1, line2[8], buf[24]);
    FillDigit(1, line2[9], buf[23]);

    if (extras.colon1) {
        buf[25] |= 0x10;
    }
    if (extras.colon2) {
        buf[26] |= 0x10;
    }
    if (extras.mon) {
        buf[23] |= 0x10;
    }
    if (extras.tue) {
        buf[18] |= 0x01;
    }
    if (extras.wed) {
        buf[19] |= 0x01;
    }
    if (extras.thu) {
        buf[20] |= 0x01;
    }
    if (extras.fri) {
        buf[21] |= 0x01;
    }
    if (extras.sat) {
        buf[22] |= 0x01;
    }
    if (extras.sun) {
        buf[24] |= 0x10;
    }
    if (extras.antenna) {
        //LOG("antenna");
        buf[17] |= 0x10;
    }
    if (extras.phone) {
        buf[17] |= 0x01;
    }
}

int UpdateDisplay(HidDevice &dev) {
    int status;

    displayUpdateFlag = false;

    char sendbuf[33] =  {
        0x03, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00
    };

    char line1[6];
    memset(line1, 0, sizeof(line1));
    char line2[11];
    memset(line2, 0, sizeof(line2));

    DisplayExtras extras;

    if (callState == 0 && callDisplay.empty()) {
        time_t rawtime;
        struct tm * timeinfo;
        time (&rawtime);
        timeinfo = localtime (&rawtime);
        strftime (line1, sizeof(line1), "%m-%d", timeinfo);

        strftime (line2, sizeof(line2), "    %H%M%S", timeinfo);

        extras.colon1 = true;
        extras.colon2 = true;
        switch (timeinfo->tm_wday) {
        case 0:
            extras.sun = true;
            break;
        case 1:
            extras.mon = true;
            break;
        case 2:
            extras.tue = true;
            break;
        case 3:
            extras.wed = true;
            break;
        case 4:
            extras.thu = true;
            break;
        case 5:
            extras.fri = true;
            break;
        case 6:
            extras.sat = true;
            break;
        }
    } else {
        strncpy(line2, callDisplay.c_str(), sizeof(line2)-1);
        extras.phone = true;
    }

    if (regState) {
        extras.antenna = true;
    }

    FillDisplayBuf(sendbuf, line1, line2, extras);

    status = dev.WriteReport(HidDevice::E_REPORT_OUT, 0, sendbuf, sizeof(sendbuf));
    return status;
}

int UpdateRing(HidDevice &dev) {
    int status;

    ringUpdateFlag = false;

    char sendbuf[33] = { 0x01, 0xFF, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    LOG("UpdateRing, ringState = %d", ringState);

    // ring ON
    if (ringState) {

    } else {
        sendbuf[1] = 0x00;
    }

    // something fishy - device often not responding correctly without pause
    Sleep(30);
    status = dev.WriteReport(HidDevice::E_REPORT_OUT, 0, sendbuf, sizeof(sendbuf));
    Sleep(30);
    return status;
}

};

DWORD WINAPI CommThreadProc(LPVOID data) {
    //ThreadComm *thrRead = (ThreadComm*)data;
    HidDevice hidDevice;
    bool devConnected = false;
    unsigned int loopCnt = 0;

    unsigned char rcvbuf[17];
    LOG("Running EX-03 comm thread");

    bool lastRcvFilled = false;
    unsigned char lastRcv[2];

    while (connected) {
        if (devConnected == false) {
            if (loopCnt % 100 == 0) {
                int status = hidDevice.Open(VendorID, ProductID, NULL, NULL);
                if (status == 0) {
                    devConnected = true;
                    LOG("EX-03 connected");
                    //LOG("  devConnected: %d", (int)devConnected);
                } else {
                    LOG("Error opening EX-03: %s", HidDevice::GetErrorDesc(status).c_str());
                }
            }
        } else {
            int status = 0;
            if (callState == 0) {
                if ((loopCnt & 0x03) == 0) {
                    displayUpdateFlag = true;
                }
            }
            if (displayUpdateFlag) {
                status = UpdateDisplay(hidDevice);
            }
            if (status == 0 && ringUpdateFlag) {
                status = UpdateRing(hidDevice);
            }
            if (status) {
                LOG("EX-03: error updating, %s", HidDevice::GetErrorDesc(status).c_str());
                hidDevice.Close();
                devConnected = false;
                lastRcvFilled = false;
            } else {
                int size = sizeof(rcvbuf);
                //LOG("%03d  devConnected: %d, size = %d", __LINE__, (int)devConnected, size);
                int status = hidDevice.ReadReport(HidDevice::E_REPORT_IN, 0, (char*)rcvbuf, &size, 100);
                //LOG("%03d  devConnected: %d, size = %d", __LINE__, (int)devConnected, size);
                if (status == 0) {
                    //LOG("REPORT_IN received: %02X %02X %02X", rcvbuf[0], rcvbuf[1], rcvbuf[2]);
                    if (!lastRcvFilled) {
                        lastRcv[0] = rcvbuf[1]; // pushbuttons
                        lastRcv[1] = rcvbuf[2]; // hook switch
                        lastRcvFilled = true;
                    } else {
                        if (lastRcv[0] != rcvbuf[1]) {
                            UpdateButtons(lastRcv[0], rcvbuf[1]);
                            lastRcv[0] = rcvbuf[1];
                        }
                        if (lastRcv[1] != rcvbuf[2]) {
                            lastRcv[1] = rcvbuf[2];
                            UpdateHook(lastRcv[1]);
                        }
                    }
                } else if (status != HidDevice::E_ERR_TIMEOUT) {
                    LOG("EX-03: error reading report");
                    hidDevice.Close();
                    devConnected = false;
                    lastRcvFilled = false;
                }
            }
        }
        loopCnt++;
        Sleep(50);
    }

    // clear display
    if (devConnected) {
        char sendbuf[33] =  {
            0x03, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00
        };
        hidDevice.WriteReport(HidDevice::E_REPORT_OUT, 0, sendbuf, sizeof(sendbuf));
    }

    hidDevice.Close();
    exited = true;
    return 0;
}


int CommThreadStart(void) {
    DWORD dwtid;
    exited = false;
    connected = true;
    HANDLE CommThread = CreateThread(NULL, 0, CommThreadProc, /*this*/NULL, 0, &dwtid);
    if (CommThread == NULL) {
        connected = false;
        exited = true;
    }

    return 0;
}

int CommThreadStop(void) {
    connected = false;
    while (!exited) {
        Sleep(50);
    }
    return 0;
}

void UpdateRegistrationState(int state) {
    regState = state;
    //LOG("regState = %d", regState);
    displayUpdateFlag = true;
}

void UpdateCallState(int state, const char* display) {
    callState = state;
    callDisplay = display;
    displayUpdateFlag = true;
}

void UpdateRing(int state) {
    if (ringState != state) {
        ringState = state;
        ringUpdateFlag = true;
    }
    //LOG("ringState = %d", ringState);
}
