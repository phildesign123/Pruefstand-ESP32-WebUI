#pragma once
#include <stdint.h>

// =============================================================
// TMC2208 Register-Adressen und Bitmasken
// =============================================================

// Register-Adressen (Read)
#define TMC2208_REG_GCONF       0x00
#define TMC2208_REG_GSTAT       0x01
#define TMC2208_REG_IFCNT       0x02
#define TMC2208_REG_OTP_PROG    0x04
#define TMC2208_REG_OTP_READ    0x05
#define TMC2208_REG_IOIN        0x06
#define TMC2208_REG_FACTORY_CONF 0x07
#define TMC2208_REG_IHOLD_IRUN  0x10
#define TMC2208_REG_TPOWERDOWN  0x11
#define TMC2208_REG_TSTEP       0x12
#define TMC2208_REG_TPWMTHRS    0x13
#define TMC2208_REG_VACTUAL     0x22
#define TMC2208_REG_MSCNT       0x6A
#define TMC2208_REG_MSCURACT    0x6B
#define TMC2208_REG_CHOPCONF    0x6C
#define TMC2208_REG_DRV_STATUS  0x6F
#define TMC2208_REG_PWMCONF     0x70
#define TMC2208_REG_PWM_SCALE   0x71
#define TMC2208_REG_PWM_AUTO    0x72

// GCONF Bits
#define GCONF_I_SCALE_ANALOG    (1u << 0)
#define GCONF_INTERNAL_RSENSE   (1u << 1)
#define GCONF_EN_SPREADCYCLE    (1u << 2)   // 0=StealthChop, 1=SpreadCycle
#define GCONF_SHAFT             (1u << 3)
#define GCONF_INDEX_OTPW        (1u << 4)
#define GCONF_INDEX_STEP        (1u << 5)
#define GCONF_PDN_DISABLE       (1u << 6)   // 1 = UART aktiviert
#define GCONF_MSTEP_REG_SELECT  (1u << 7)   // 1 = Mikroschritt aus Register
#define GCONF_MULTISTEP_FILT    (1u << 8)

// CHOPCONF Bits
#define CHOPCONF_TOFF_MASK      0x0000000F
#define CHOPCONF_HSTRT_MASK     0x00000070
#define CHOPCONF_HEND_MASK      0x00000780
#define CHOPCONF_TBL_MASK       0x00018000
#define CHOPCONF_VSENSE         (1u << 17)
#define CHOPCONF_MRES_MASK      0x0F000000   // Mikroschritt-Bits [27:24]
#define CHOPCONF_MRES_SHIFT     24
#define CHOPCONF_INTPOL         (1u << 28)   // Interpolation auf 256 µStep
#define CHOPCONF_DEDGE          (1u << 29)
#define CHOPCONF_DISS2G         (1u << 30)
#define CHOPCONF_DISS2VS        (1u << 31)

// IHOLD_IRUN Bits
#define IHOLD_MASK              0x0000001F
#define IRUN_SHIFT              8
#define IRUN_MASK               0x00001F00
#define IHOLDDELAY_SHIFT        16
#define IHOLDDELAY_MASK         0x000F0000

// DRV_STATUS Bits
#define DRV_STATUS_OTPW         (1u << 0)
#define DRV_STATUS_OT           (1u << 1)
#define DRV_STATUS_S2GA         (1u << 2)
#define DRV_STATUS_S2GB         (1u << 3)
#define DRV_STATUS_S2VSA        (1u << 4)
#define DRV_STATUS_S2VSB        (1u << 5)
#define DRV_STATUS_OLA          (1u << 6)
#define DRV_STATUS_OLB          (1u << 7)
#define DRV_STATUS_T120         (1u << 8)
#define DRV_STATUS_T143         (1u << 9)
#define DRV_STATUS_T150         (1u << 10)
#define DRV_STATUS_T157         (1u << 11)
#define DRV_STATUS_CS_ACTUAL_MASK 0x001F0000
#define DRV_STATUS_STEALTH      (1u << 30)
#define DRV_STATUS_STST         (1u << 31)

// Mikroschritt-Code zu MRES-Bits (CHOPCONF)
static inline uint8_t microstep_to_mres(uint16_t ms) {
    switch (ms) {
        case 256: return 0;
        case 128: return 1;
        case  64: return 2;
        case  32: return 3;
        case  16: return 4;
        case   8: return 5;
        case   4: return 6;
        case   2: return 7;
        default:  return 8;  // Vollschritt
    }
}
