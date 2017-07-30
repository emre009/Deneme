#include "rtc.h"
#include "i2c.h"

#define BCDVALID(b) (((b)<=0x99)&&(((b)&0xF)<=0x9)&&((((b)>>4)&0xF)<=0x9))
#define BCD2NUM(b)  (BCDVALID(b) ? (((b)&0xF)+((((b)>>4)&0xF)*10)) : 0)
#define DSTIMEGET(bcd,n) (BCD2NUM((bcd)->n))

// see: http://3dbrew.org/wiki/I2C_Registers#Device_3 (register 30)
typedef struct {
    u8 bcd_s;
    u8 bcd_m;
    u8 bcd_h;
    u8 unknown;
    u8 bcd_D;
    u8 bcd_M;
    u8 bcd_Y;
} __attribute__((packed)) DsTime;

DWORD get_fattime( void ) {
    DsTime dstime;
    I2C_readRegBuf(I2C_DEV_MCU, 0x30, (void*) &dstime, 7);
    DWORD fattime = (((DSTIMEGET(&dstime, bcd_s)>>1)<<0)&0x1F) |
        ((DSTIMEGET(&dstime, bcd_m)<< 5)&0x3F) |
        ((DSTIMEGET(&dstime, bcd_h)<<11)&0x3F) |
        ((DSTIMEGET(&dstime, bcd_D)<<16)&0x1F) |
        ((DSTIMEGET(&dstime, bcd_M)<<21)&0x0F) |
        (((DSTIMEGET(&dstime, bcd_Y)+(2000-1980))<<25)&0x7F);
    
    return fattime;
}

#include "ui.h"
void disp_time( void ) {
    DsTime dstime;
    I2C_readRegBuf(I2C_DEV_MCU, 0x30, (void*) &dstime, 7);
    ShowPrompt(false, "Current time/date is:\n%02lu:%02lu:%02lu %02lu/%02lu/%02lu",
        DSTIMEGET(&dstime, bcd_h), DSTIMEGET(&dstime, bcd_m), DSTIMEGET(&dstime, bcd_s),
        DSTIMEGET(&dstime, bcd_D), DSTIMEGET(&dstime, bcd_M), DSTIMEGET(&dstime, bcd_Y));
}
