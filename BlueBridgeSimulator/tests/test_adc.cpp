// ADC model unit test (stage 4): register file, HAL-compatible enable /
// calibration / start sequence, resolution, alignment, EOC/OVR semantics and
// the board-supplied channel voltage.
//
// The ADC is exercised through its MMIO interface only (busRead/busWrite), the
// same way firmware would drive it.
#include <cmath>
#include <cstdio>

#include "stm32/adc/Adc.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void checkNear(const char* what, double value, double expected,
                      double tol) {
    const bool ok = std::fabs(value - expected) <= tol;
    std::printf("  %-46s = %10.3f (expected %8.3f +-%5.3f) %s\n", what, value,
                expected, tol, ok ? "ok" : "FAIL");
    if (!ok) g_failures++;
}

// read/write helpers through the bus interface (the same path the CPU uses)
static uint32_t rdReg(Adc& a, uint32_t off) {
    return a.busRead(a.base() + off, 4);
}
static void wrReg(Adc& a, uint32_t off, uint32_t v) {
    a.busWrite(a.base() + off, v, 4);
}

// register offsets (CMSIS ADC_TypeDef)
static constexpr uint32_t ISR = 0x00, IER = 0x04, CR = 0x08, CFGR = 0x0C,
                          SMPR1 = 0x14, SQR1 = 0x30, DR = 0x40;
static constexpr uint32_t ISR_ADRDY = 1u << 0, ISR_EOC = 1u << 2,
                          ISR_EOS = 1u << 3, ISR_OVR = 1u << 4;
static constexpr uint32_t CR_ADEN = 1u << 0, CR_ADSTART = 1u << 2,
                          CR_ADSTP = 1u << 4, CR_ADCAL = 1u << 31;
static constexpr uint32_t CFGR_RES_10B = 1u << 3, CFGR_ALIGN = 1u << 15,
                          CFGR_CONT = 1u << 13;

// board-like channel voltages for this test
static double g_chVoltage[19] = {0};
static double chanVoltage(int ch) {
    return (ch >= 0 && ch < 19) ? g_chVoltage[ch] : 0.0;
}

// emulate what HAL_ADC_Init + HAL_ADCEx_Calibration_Start + HAL_ADC_Start do
static void halEnable(Adc& a) {
    wrReg(a, CR, CR_ADEN);
    CHECK(rdReg(a, ISR) & ISR_ADRDY);
}
static void halCalibrate(Adc& a) {
    wrReg(a, CR, CR_ADCAL | (1u << 28));  // ADCAL + ADVREGEN
    CHECK((rdReg(a, CR) & CR_ADCAL) == 0);  // self-clears, like real silicon
    CHECK((rdReg(a, CR) & CR_ADEN) == 0);   // ADEN went low during calibration
    halEnable(a);                            // HAL re-enables afterwards
}
static void halStart(Adc& a) { wrReg(a, CR, CR_ADEN | CR_ADSTART); }

int main() {
    Adc adc1(0x50000000u, {"ADC1", 1, 18});
    Adc adc2(0x50000100u, {"ADC2", 2, 18});
    adc1.setChannelVoltageProvider(chanVoltage);
    adc2.setChannelVoltageProvider(chanVoltage);

    int irqCount1 = 0;
    adc1.setIrqCallback([&irqCount1](int irq) {
        CHECK(irq == 18);
        irqCount1++;
    });

    // ---- reset values ----
    adc1.reset();
    CHECK(rdReg(adc1, ISR) == 0);
    CHECK(rdReg(adc1, CR) == 0);
    CHECK(rdReg(adc1, DR) == 0);
    CHECK(!adc1.enabled());
    CHECK(adc1.resolutionBits() == 12);
    CHECK(adc1.maxCode() == 4095);

    // ---- HAL sequence: enable + calibrate ----
    halEnable(adc1);
    CHECK(adc1.enabled());
    halCalibrate(adc1);
    CHECK(adc1.enabled());

    // ---- channel selection + 12-bit conversion (R38 style: ADC1_IN11) ----
    wrReg(adc1, SQR1, (11u << 6));  // rank 1 = channel 11
    CHECK(adc1.selectedChannel() == 11);
    g_chVoltage[11] = 1.65;
    halStart(adc1);
    CHECK(rdReg(adc1, ISR) & ISR_EOC);
    CHECK(rdReg(adc1, ISR) & ISR_EOS);
    checkNear("ADC1 IN11 1.65 V @12 bit", double(adc1.lastCode()), 2048.0, 2.0);
    // reading DR clears EOC/EOS
    const uint32_t dr = rdReg(adc1, DR);
    CHECK((dr & 0x0FFFu) == adc1.lastCode());
    CHECK((rdReg(adc1, ISR) & (ISR_EOC | ISR_EOS)) == 0);
    CHECK(!adc1.converting());  // single conversion: ADSTART self-cleared

    // ---- range ends ----
    g_chVoltage[11] = 0.0;
    halStart(adc1);
    CHECK(adc1.lastCode() == 0);
    g_chVoltage[11] = 3.3;
    halStart(adc1);
    CHECK(adc1.lastCode() == 4095);
    g_chVoltage[11] = 5.0;  // above Vref: clamped (no wrap-around)
    halStart(adc1);
    CHECK(adc1.lastCode() == 4095);

    // ---- another channel on the same ADC ----
    g_chVoltage[11] = 0.0;
    g_chVoltage[15] = 3.3 * 0.5;  // 1.65 V on channel 15
    wrReg(adc1, SQR1, (15u << 6));
    halStart(adc1);
    checkNear("ADC1 IN15 1.65 V @12 bit", double(adc1.lastCode()), 2048.0, 2.0);
    // an unrouted channel reads 0 V
    wrReg(adc1, SQR1, (4u << 6));
    halStart(adc1);
    CHECK(adc1.lastCode() == 0);

    // ---- resolution: 10 bit ----
    wrReg(adc1, SQR1, (15u << 6));
    wrReg(adc1, CFGR, CFGR_RES_10B);
    CHECK(adc1.resolutionBits() == 10);
    CHECK(adc1.maxCode() == 1023);
    halStart(adc1);
    checkNear("ADC1 IN15 1.65 V @10 bit", double(adc1.lastCode()), 512.0, 1.0);
    // 6 bit
    wrReg(adc1, CFGR, 3u << 3);
    CHECK(adc1.resolutionBits() == 6);
    halStart(adc1);
    checkNear("ADC1 IN15 1.65 V @6 bit", double(adc1.lastCode()), 32.0, 1.0);
    // back to 12 bit, left aligned
    wrReg(adc1, CFGR, CFGR_ALIGN);
    halStart(adc1);
    CHECK(rdReg(adc1, DR) == (2048u << 4));  // code << (16-12)
    wrReg(adc1, CFGR, 0);

    // ---- ADC2 is independent (R37 style: ADC2_IN15) ----
    g_chVoltage[15] = 2.475;  // 75 % of 3.3 V
    halEnable(adc2);
    halCalibrate(adc2);
    wrReg(adc2, SQR1, (15u << 6));
    halStart(adc2);
    checkNear("ADC2 IN15 2.475 V @12 bit", double(adc2.lastCode()), 3071.0, 2.0);
    CHECK(rdReg(adc1, DR) != 0);  // ADC1 result untouched by ADC2

    // ---- continuous mode: EOC comes back after reading DR ----
    wrReg(adc1, CFGR, CFGR_CONT);
    g_chVoltage[15] = 0.825;  // 25 %
    halStart(adc1);
    CHECK(rdReg(adc1, ISR) & ISR_EOC);
    rdReg(adc1, DR);
    CHECK(rdReg(adc1, ISR) & ISR_EOC);  // next conversion already done
    checkNear("continuous mode value", double(adc1.lastCode()), 1024.0, 2.0);
    wrReg(adc1, CR, CR_ADEN | CR_ADSTP);  // HAL_ADC_Stop
    CHECK(!adc1.converting());
    wrReg(adc1, CFGR, 0);

    // ---- overrun: a new conversion while EOC is still set ----
    g_chVoltage[15] = 3.3;
    halStart(adc1);
    CHECK((rdReg(adc1, ISR) & ISR_EOC) != 0);
    halStart(adc1);  // result not read yet -> overrun
    CHECK(rdReg(adc1, ISR) & ISR_OVR);
    wrReg(adc1, ISR, ISR_OVR);  // write 1 to clear
    CHECK((rdReg(adc1, ISR) & ISR_OVR) == 0);
    rdReg(adc1, DR);

    // ---- EOC interrupt ----
    irqCount1 = 0;
    wrReg(adc1, IER, 1u << 2);  // EOCIE
    halStart(adc1);
    CHECK(irqCount1 == 1);
    rdReg(adc1, DR);
    halStart(adc1);
    CHECK(irqCount1 == 2);
    wrReg(adc1, IER, 0);

    // ---- disable: ADDIS clears ADEN/ADRDY ----
    wrReg(adc1, CR, 1u << 1);  // ADDIS
    CHECK(!adc1.enabled());
    CHECK((rdReg(adc1, ISR) & ISR_ADRDY) == 0);
    // conversions cannot start while disabled
    wrReg(adc1, CR, CR_ADSTART);
    CHECK(!adc1.converting());

    // ---- stored registers do not crash / read back ----
    wrReg(adc1, SMPR1, 0x7u);
    CHECK(rdReg(adc1, SMPR1) == 0x7u);
    wrReg(adc1, CFGR, 1u << 0);   // DMAEN stored
    CHECK((rdReg(adc1, CFGR) & 1u) != 0);

    if (g_failures == 0) {
        std::printf("test_adc: all tests passed\n");
        return 0;
    }
    std::printf("test_adc: %d failure(s)\n", g_failures);
    return 1;
}