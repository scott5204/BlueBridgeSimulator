#include "stm32/Stm32G431.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
// ---------------------------------------------------------------------------
// TIMx_CHy <-> GPIO pin alternate-function table for STM32G431RBTx.
// Derived (never guessed) from two authoritative sources which are
// intersected: the CubeMX MCU database for the exact part
// (STM32G431R(6-8-B)Tx.xml -> which TIM signals exist on which pin) and the
// ST pin map (NuttX arch/arm/src/stm32g4/hardware/stm32g4xxr_pinmap.h,
// R package = LQFP64 -> the AF number). Regenerate with tim_af_build.py.
// ---------------------------------------------------------------------------
struct AfRow {
    int port;   // 0 = A
    int pin;
    int af;
    int timer;  // index into Stm32G431::timers_
    int channel;
};
constexpr int kTim1 = 0, kTim2 = 1, kTim3 = 2, kTim4 = 3, kTim6 = 4, kTim7 = 5,
              kTim8 = 6, kTim15 = 7, kTim16 = 8, kTim17 = 9;
const AfRow kTimAfTable[] = {
    // --- TIM1 (advanced, APB2) ---
    {0,  8,  6, kTim1, 1},  // PA8  TIM1_CH1 AF6
    {2,  0,  2, kTim1, 1},  // PC0  TIM1_CH1 AF2
    {0,  9,  6, kTim1, 2},  // PA9  TIM1_CH2 AF6
    {2,  1,  2, kTim1, 2},  // PC1  TIM1_CH2 AF2
    {0, 10,  6, kTim1, 3},  // PA10 TIM1_CH3 AF6
    {2,  2,  2, kTim1, 3},  // PC2  TIM1_CH3 AF2
    {0, 11, 11, kTim1, 4},  // PA11 TIM1_CH4 AF11
    {2,  3,  2, kTim1, 4},  // PC3  TIM1_CH4 AF2
    // --- TIM2 (32-bit, APB1) ---
    {0,  0,  1, kTim2, 1},  // PA0  TIM2_CH1 AF1
    {0, 15,  1, kTim2, 1},  // PA15 TIM2_CH1 AF1   (<- input capture pin)
    {0,  5,  1, kTim2, 1},  // PA5  TIM2_CH1 AF1
    {0,  1,  1, kTim2, 2},  // PA1  TIM2_CH2 AF1
    {1,  3,  1, kTim2, 2},  // PB3  TIM2_CH2 AF1
    {0,  2,  1, kTim2, 3},  // PA2  TIM2_CH3 AF1
    {0,  9, 10, kTim2, 3},  // PA9  TIM2_CH3 AF10
    {1, 10,  1, kTim2, 3},  // PB10 TIM2_CH3 AF1
    {0,  3,  1, kTim2, 4},  // PA3  TIM2_CH4 AF1
    {0, 10, 10, kTim2, 4},  // PA10 TIM2_CH4 AF10
    {1, 11,  1, kTim2, 4},  // PB11 TIM2_CH4 AF1
    // --- TIM3 (APB1) ---
    {0,  6,  2, kTim3, 1},  // PA6  TIM3_CH1 AF2
    {1,  4,  2, kTim3, 1},  // PB4  TIM3_CH1 AF2
    {2,  6,  2, kTim3, 1},  // PC6  TIM3_CH1 AF2
    {0,  4,  2, kTim3, 2},  // PA4  TIM3_CH2 AF2
    {0,  7,  2, kTim3, 2},  // PA7  TIM3_CH2 AF2   (<- PWM output pin)
    {1,  5,  2, kTim3, 2},  // PB5  TIM3_CH2 AF2
    {2,  7,  2, kTim3, 2},  // PC7  TIM3_CH2 AF2
    {1,  0,  2, kTim3, 3},  // PB0  TIM3_CH3 AF2
    {2,  8,  2, kTim3, 3},  // PC8  TIM3_CH3 AF2
    {1,  1,  2, kTim3, 4},  // PB1  TIM3_CH4 AF2
    {1,  7, 10, kTim3, 4},  // PB7  TIM3_CH4 AF10
    {2,  9,  2, kTim3, 4},  // PC9  TIM3_CH4 AF2
    // --- TIM4 (APB1) ---
    {0, 11, 10, kTim4, 1},  // PA11 TIM4_CH1 AF10
    {1,  6,  2, kTim4, 1},  // PB6  TIM4_CH1 AF2
    {0, 12, 10, kTim4, 2},  // PA12 TIM4_CH2 AF10
    {1,  7,  2, kTim4, 2},  // PB7  TIM4_CH2 AF2
    {0, 13, 10, kTim4, 3},  // PA13 TIM4_CH3 AF10
    {1,  9,  2, kTim4, 4},  // PB9  TIM4_CH4 AF2
    // --- TIM8 (advanced, APB2) ---
    {0, 15,  2, kTim8, 1},  // PA15 TIM8_CH1 AF2
    {1,  6,  5, kTim8, 1},  // PB6  TIM8_CH1 AF5
    {2,  6,  4, kTim8, 1},  // PC6  TIM8_CH1 AF4
    {0, 14,  5, kTim8, 2},  // PA14 TIM8_CH2 AF5
    {2,  7,  4, kTim8, 2},  // PC7  TIM8_CH2 AF4
    {1,  9, 10, kTim8, 3},  // PB9  TIM8_CH3 AF10
    {2,  8,  4, kTim8, 3},  // PC8  TIM8_CH3 AF4
    {2,  9,  4, kTim8, 4},  // PC9  TIM8_CH4 AF4
    // --- TIM15 / TIM16 / TIM17 (APB2, outputs gated by MOE) ---
    {0,  2,  9, kTim15, 1}, // PA2  TIM15_CH1 AF9
    {1, 14,  1, kTim15, 1}, // PB14 TIM15_CH1 AF1
    {0,  3,  9, kTim15, 2}, // PA3  TIM15_CH2 AF9
    {1, 15,  1, kTim15, 2}, // PB15 TIM15_CH2 AF1
    {0, 12,  1, kTim16, 1}, // PA12 TIM16_CH1 AF1
    {0,  6,  1, kTim16, 1}, // PA6  TIM16_CH1 AF1
    {1,  4,  1, kTim16, 1}, // PB4  TIM16_CH1 AF1
    {0,  7,  1, kTim17, 1}, // PA7  TIM17_CH1 AF1
    {1,  5, 10, kTim17, 1}, // PB5  TIM17_CH1 AF10
    {1,  9,  1, kTim17, 1}, // PB9  TIM17_CH1 AF1
};

// one input route candidate for a pin (several TIM channels can share a pin,
// selected by the AF number the firmware programs in AFR)
struct PinInputRoute {
    int af;
    int timer;
    int channel;
};

// ---------------------------------------------------------------------------
// USART1 <-> GPIO pin alternate-function table for STM32G431RBTx.
// Derived from the ST pin map (NuttX stm32g4xxr_pinmap.h, R package = LQFP64):
//   USART1_TX: PA9 AF7, PB6 AF7, PC4 AF7
//   USART1_RX: PA10 AF7, PB7 AF7, PC5 AF7
// The CT117E-M4 wires PA9/PA10 to the DAP-Link USB-serial port (schematic
// SCH_CT117E_M4 Rev V1.2, page "MCU" + the DAP-Link sheet).
// ---------------------------------------------------------------------------
struct UsartAfRow {
    int port;  // 0 = A
    int pin;
    int af;
    bool tx;
};
constexpr UsartAfRow kUsartAfTable[] = {
    {0,  9, 7, true},   // PA9  USART1_TX AF7
    {1,  6, 7, true},   // PB6  USART1_TX AF7
    {2,  4, 7, true},   // PC4  USART1_TX AF7
    {0, 10, 7, false},  // PA10 USART1_RX AF7
    {1,  7, 7, false},  // PB7  USART1_RX AF7
    {2,  5, 7, false},  // PC5  USART1_RX AF7
};
}  // namespace

Stm32G431::Stm32G431() {
    std::memset(flash_, 0xFF, sizeof(flash_));
    std::memset(sram_, 0, sizeof(sram_));
    std::memset(ccm_, 0, sizeof(ccm_));

    timers_[0] = &tim1;
    timers_[1] = &tim2;
    timers_[2] = &tim3;
    timers_[3] = &tim4;
    timers_[4] = &tim6;
    timers_[5] = &tim7;
    timers_[6] = &tim8;
    timers_[7] = &tim15;
    timers_[8] = &tim16;
    timers_[9] = &tim17;

    buildBus();
    wireTimers();
    wireAnalog();
    wireUsart();

    // DWT reads the virtual cycle counter
    dwt.cycleProvider = [this] { return cycles_; };
    // AIRCR SYSRESETREQ / ICSR NMI plumbing
    ppb.sysResetRequested = [this] { resetRequested_ = true; };
    // RCC clock gates -> GPIO ports and the APB2 peripherals
    rcc.onAhb2enrChanged = [this] {
        gpioA.setClockEnabled(rcc.gpioClockEnabled(0));
        gpioB.setClockEnabled(rcc.gpioClockEnabled(1));
        gpioC.setClockEnabled(rcc.gpioClockEnabled(2));
        gpioD.setClockEnabled(rcc.gpioClockEnabled(3));
        gpioE.setClockEnabled(rcc.gpioClockEnabled(4));
    };
    rcc.onApb2enrChanged = [this] {
        usart1.setClockEnabled(rcc.usart1ClockEnabled());
    };
}

// Timer <-> pin wiring. The timers only know channels, the GPIO only knows
// pins: this function is the single place where the AF table is applied
// (PROJECT_HANDOFF "AF router").
//
// A TIMx_CHy signal can reach SEVERAL pins (PA4/PA7/PB5/PC7 all carry
// TIM3_CH2) and a pin can host several timer channels (PA15 = TIM2_CH1 AF1 or
// TIM8_CH1 AF2). The routing is therefore resolved dynamically, per pin, from
// what the FIRMWARE programmed: MODER = alternate function and the AF number
// of the table row (RM0440 AFRL/AFRH). A pin whose MODER is not AF, or whose
// AF number selects a different peripheral, is simply not driven -- which is
// what makes "forgot the AF configuration" visible on the pin.
void Stm32G431::wireTimers() {
    for (int i = 0; i < kTimerCount; i++) {
        Timer* t = timers_[i];
        t->setIrqCallback([this](int irq) { ppb.nvic.pend(irq + 16); });
        t->setChannelOutputCallback(
            [this, i](int ch, bool driven, bool level, uint64_t cpuCycle) {
                if (ch < 0 || ch >= 4) return;
                for (const AfRow& r : kTimAfTable) {
                    if (r.timer != i || r.channel - 1 != ch) continue;
                    GpioPort* p = portPtr(r.port);
                    const bool routed =
                        (p->pinMode(r.pin) == 2) && (p->pinAf(r.pin) == r.af);
                    p->setAfPin(r.pin, routed && driven, routed && level,
                                cpuCycle);
                }
            });
    }

    // GPIO routing register changes (MODER/AFR) must re-apply the peripheral
    // drive immediately, otherwise a pin switched to AF would keep showing the
    // pull/external level until the next compare event.
    for (int port = 0; port < 5; port++) {
        portPtr(port)->setAfRemapCallback([this] {
            for (int i = 0; i < kTimerCount; i++)
                timers_[i]->refreshChannelOutputs();
            usart1.refreshPins();  // AF7: show the transmitter idle level
        });
    }

    // input path (capture): one observer per pin, dispatching on the AF
    // number the firmware programmed -> several channels may share a pin
    for (int port = 0; port < 5; port++) {
        for (int pin = 0; pin < 16; pin++) {
            auto routes = std::make_shared<std::vector<PinInputRoute>>();
            for (const AfRow& r : kTimAfTable) {
                if (r.port == port && r.pin == pin)
                    routes->push_back(PinInputRoute{r.af, r.timer, r.channel});
            }
            if (routes->empty()) continue;
            GpioPort* p = portPtr(port);
            p->setLevelObserver(
                pin, [this, port, pin, routes](bool level, uint64_t cpuCycle) {
                    GpioPort* gp = portPtr(port);
                    if (gp->pinMode(pin) != 2) return;  // not in AF mode
                    const int af = gp->pinAf(pin);
                    for (const PinInputRoute& r : *routes) {
                        if (r.af != af) continue;
                        // table channels are 1-based, the timer API is 0-based
                        timers_[r.timer]->onChannelPinLevel(r.channel - 1, level,
                                                            cpuCycle);
                    }
                });
        }
    }
}

// ADC interrupt plumbing: a conversion-complete event pends ADC1_2_IRQn in the
// NVIC exactly like a timer update event does (single place, SoC layer).
void Stm32G431::wireAnalog() {
    auto pend = [this](int irq) { ppb.nvic.pend(irq + 16); };
    adc1.setIrqCallback(pend);
    adc2.setIrqCallback(pend);
}

// USART1 wiring (stage 5). Two things happen here, both SoC-level:
//   * the interrupt line: a USART event (RXNE / TC / TXE / ORE) pends
//     USART1_IRQn = 37 in the NVIC -- the same real-exception path as the
//     timers and the ADC;
//   * the byte-level serial line: the transmitter reports finished bytes and
//     the board's virtual PC peer reports received ones, and BOTH directions
//     are gated by the AF table above. A firmware that forgot PA9/PA10 = AF7
//     really loses the bytes (TX: nothing reaches the terminal; RX: RXNE stays
//     0), which is what the stage-5 self test checks.
void Stm32G431::wireUsart() {
    usart1.setIrqCallback([this](int irq) { ppb.nvic.pend(irq + 16); });
    usart1.setClockProviders([this] { return rcc.ahbHz(); },
                             [this] { return rcc.apb2Hz(); });
    usart1.setTxPinCallback(
        [this](bool driven, bool level, uint64_t cpuCycle) {
            for (const UsartAfRow& r : kUsartAfTable) {
                if (!r.tx) continue;
                GpioPort* p = portPtr(r.port);
                const bool routed = (p->pinMode(r.pin) == 2) &&
                                    (p->pinAf(r.pin) == r.af);
                p->setAfPin(r.pin, routed && driven, routed && level,
                            cpuCycle);
            }
        });
    usart1.setTxSink([this](uint8_t b) {
        if (!usartPinsRouted(/*tx=*/true)) return;  // TX pin not on AF7
        if (usartLineRx_) usartLineRx_(b);
    });
}

bool Stm32G431::usartPinsRouted(bool tx) const {
    for (const UsartAfRow& r : kUsartAfTable) {
        if (r.tx != tx) continue;
        const GpioPort* p = portPtr(r.port);
        if (p->pinMode(r.pin) == 2 && p->pinAf(r.pin) == r.af) return true;
    }
    return false;
}

bool Stm32G431::usart1RxFromLine(uint8_t b) {
    if (!usartPinsRouted(/*tx=*/false)) return false;
    usart1.receive(b);
    return true;
}

GpioPort* Stm32G431::portPtr(int port) {
    switch (port) {
    case 0: return &gpioA;
    case 1: return &gpioB;
    case 2: return &gpioC;
    case 3: return &gpioD;
    default: return &gpioE;
    }
}

const GpioPort* Stm32G431::portPtr(int port) const {
    return const_cast<Stm32G431*>(this)->portPtr(port);
}

Timer* Stm32G431::timerByName(const char* name) {
    for (int i = 0; i < kTimerCount; i++)
        if (std::strcmp(timers_[i]->traits().name, name) == 0) return timers_[i];
    return nullptr;
}

void Stm32G431::setTimerLogger(std::function<void(const std::string&)> logger) {
    for (int i = 0; i < kTimerCount; i++) timers_[i]->setLogger(logger);
}

void Stm32G431::setTimerTrace(bool events, bool regs) {
    for (int i = 0; i < kTimerCount; i++) {
        timers_[i]->setTraceEvents(events);
        timers_[i]->setTraceRegs(regs);
    }
}

void Stm32G431::buildBus() {
    bus_.addRegion(Rcc::kBase, Rcc::kSize, &rcc);
    bus_.addRegion(0x48000000u, 0x400u, &gpioA);
    bus_.addRegion(0x48000400u, 0x400u, &gpioB);
    bus_.addRegion(0x48000800u, 0x400u, &gpioC);
    bus_.addRegion(0x48000C00u, 0x400u, &gpioD);
    bus_.addRegion(0x48001000u, 0x400u, &gpioE);
    bus_.addRegion(Ppb::kBase, Ppb::kSize, &ppb);
    // ADCs (RM0440 / CMSIS bases). The board wires the channel -> voltage
    // providers; the ADC itself knows nothing about the potentiometers.
    bus_.addRegion(0x50000000u, 0x100u, &adc1);
    bus_.addRegion(0x50000100u, 0x100u, &adc2);
    bus_.addRegion(AdcCommonRegs::kBase, AdcCommonRegs::kSize, &adcCommon);
    // USART1 (APB2). The board's virtual serial peer talks to it over the
    // byte-level line wired in wireUsart().
    bus_.addRegion(0x40013800u, 0x400u, &usart1);
    bus_.addRegion(DwtDevice::kBase, DwtDevice::kSize, &dwt);
    bus_.addRegion(ItmDevice::kBase, ItmDevice::kSize, &itm);
    // Timers (RM0440 bases). Registered dynamically so the list stays in sync
    // with timers_[] / the AF table.
    for (int i = 0; i < kTimerCount; i++) {
        Timer* t = timers_[i];
        if (!t) continue;
        bus_.addRegion(t->base(), 0x400u, t);
    }
}

void Stm32G431::reset() {
    rcc.reset();
    gpioA.reset();
    gpioB.reset();
    gpioC.reset();
    gpioD.reset();
    gpioE.reset();
    ppb.reset();
    dwt.reset();
    itm.reset();
    for (int i = 0; i < kTimerCount; i++) timers_[i]->reset();
    adc1.reset();
    adc2.reset();
    adcCommon.reset();
    usart1.reset();
    std::memset(sram_, 0, sizeof(sram_));
    std::memset(ccm_, 0, sizeof(ccm_));
    cycles_ = 0;
    resetRequested_ = false;

    // wire the RCC clock gates into the GPIO ports
    gpioA.setClockEnabled(rcc.gpioClockEnabled(0));
    gpioB.setClockEnabled(rcc.gpioClockEnabled(1));
    gpioC.setClockEnabled(rcc.gpioClockEnabled(2));
    gpioD.setClockEnabled(rcc.gpioClockEnabled(3));
    gpioE.setClockEnabled(rcc.gpioClockEnabled(4));
    usart1.setClockEnabled(rcc.usart1ClockEnabled());
}

bool Stm32G431::buildFlashImage(const FirmwareImage& img, uint8_t* out,
                                size_t size, std::string& error) {
    if (img.empty()) {
        error = "firmware image is empty";
        return false;
    }
    if (!out || size != kFlashSize) {
        error = "flash image buffer must be exactly 128 KiB";
        return false;
    }
    uint32_t lo = img.lowestAddress();
    uint32_t hi = img.highestEndAddress();
    if (lo < kFlashBase || hi > kFlashBase + kFlashSize) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "image outside flash: 0x%08X..0x%08X (flash "
                      "0x08000000..0x0801FFFF)",
                      lo, hi);
        error = buf;
        return false;
    }
    std::memset(out, 0xFF, size);
    for (const auto& seg : img.segments) {
        if (seg.data.empty()) continue;
        std::memcpy(out + (seg.address - kFlashBase), seg.data.data(),
                    seg.data.size());
    }
    return true;
}

bool Stm32G431::replaceFlash(const uint8_t* data, size_t size) {
    if (!data || size != kFlashSize) return false;
    std::memcpy(flash_, data, size);  // the one and only flash backing store
    return true;
}

bool Stm32G431::loadFirmware(const FirmwareImage& img, std::string& error) {
    // Shared image builder: the firmware loader and the debugger programming
    // commit (Simulator::replaceFlashImage) must agree byte for byte.
    return buildFlashImage(img, flash_, kFlashSize, error);
}

bool Stm32G431::readWord(uint32_t addr, uint32_t& out) const {
    // boot alias: 0x00000000..0x0001FFFF mirrors flash
    if (addr < kFlashSize) addr += kFlashBase;
    if (addr >= kFlashBase && addr + 4 <= kFlashBase + kFlashSize) {
        uint32_t off = addr - kFlashBase;
        out = uint32_t(flash_[off]) | (uint32_t(flash_[off + 1]) << 8) |
              (uint32_t(flash_[off + 2]) << 16) |
              (uint32_t(flash_[off + 3]) << 24);
        return true;
    }
    if (addr >= kSramBase && addr + 4 <= kSramBase + kSramSize) {
        uint32_t off = addr - kSramBase;
        std::memcpy(&out, sram_ + off, 4);
        return true;
    }
    if (addr >= kCcmBase && addr + 4 <= kCcmBase + kCcmSize) {
        uint32_t off = addr - kCcmBase;
        std::memcpy(&out, ccm_ + off, 4);
        return true;
    }
    return false;
}

bool Stm32G431::writeWord(uint32_t addr, uint32_t value) {
    if (addr >= kSramBase && addr + 4 <= kSramBase + kSramSize) {
        std::memcpy(sram_ + (addr - kSramBase), &value, 4);
        return true;
    }
    if (addr >= kCcmBase && addr + 4 <= kCcmBase + kCcmSize) {
        std::memcpy(ccm_ + (addr - kCcmBase), &value, 4);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// host (debugger) guest memory access -- see Stm32G431.h
// ---------------------------------------------------------------------------
namespace {
// is [addr, addr+size) fully inside [lo, hi)?  (64-bit to catch wraparound)
bool within(uint32_t addr, size_t size, uint32_t lo, uint32_t hi) {
    const uint64_t end = uint64_t(addr) + uint64_t(size);
    return addr >= lo && end <= uint64_t(hi);
}
}  // namespace

bool Stm32G431::isPeripheralAddress(uint32_t addr, size_t size) {
    // The windows the bus serves (see buildBus()) plus their unimplemented
    // scratch surroundings, which the bus treats as read/write-back storage --
    // exactly what the CPU sees there too.
    return within(addr, size, 0x40000000u, 0x40030000u) ||  // APB1/APB2/AHB1
           within(addr, size, 0x48000000u, 0x48002000u) ||  // AHB2 (GPIO)
           within(addr, size, 0x50000000u, 0x50001000u) ||  // ADC1/2 + common
           within(addr, size, 0xE0000000u, 0xE0041000u);    // ITM/DWT/PPB/TPIU
}

const uint8_t* Stm32G431::realMemoryAt(uint32_t addr, size_t size) const {
    if (size == 0) return nullptr;
    if (within(addr, size, 0, kFlashSize)) return flash_ + addr;  // boot alias
    if (within(addr, size, kFlashBase, kFlashBase + kFlashSize))
        return flash_ + (addr - kFlashBase);
    if (within(addr, size, kSramBase, kSramBase + kSramSize))
        return sram_ + (addr - kSramBase);
    if (within(addr, size, kCcmBase, kCcmBase + kCcmSize))
        return ccm_ + (addr - kCcmBase);
    return nullptr;
}

uint8_t* Stm32G431::realMemoryAt(uint32_t addr, size_t size) {
    return const_cast<uint8_t*>(
        static_cast<const Stm32G431*>(this)->realMemoryAt(addr, size));
}

DebugStatus Stm32G431::guestRead(uint32_t addr, void* data, size_t size) {
    if (!data || size == 0) return DebugStatus::InvalidAddress;
    if (const uint8_t* p = realMemoryAt(addr, size)) {
        std::memcpy(data, p, size);
        return DebugStatus::Ok;
    }
    if (!isPeripheralAddress(addr, size))
        return DebugStatus::InvalidAddress;
    // peripheral space: go through the real bus model, one naturally sized
    // access at a time (word / halfword / byte, honouring alignment)
    uint8_t* out = static_cast<uint8_t*>(data);
    for (size_t off = 0; off < size;) {
        const uint32_t a = addr + uint32_t(off);
        const size_t left = size - off;
        const size_t n = ((a & 3u) == 0 && left >= 4) ? 4
                       : ((a & 1u) == 0 && left >= 2) ? 2
                                                      : 1;
        const uint32_t v = bus_.read(a, uint32_t(n));
        std::memcpy(out + off, &v, n);  // x86 host is little-endian like ARM
        off += n;
    }
    return DebugStatus::Ok;
}

DebugStatus Stm32G431::guestWrite(uint32_t addr, const void* data,
                                  size_t size) {
    if (!data || size == 0) return DebugStatus::InvalidAddress;
    // flash programming is a separate (future) stage: never silently corrupt
    // the image the firmware is executing / reading back
    if (within(addr, size, 0, kFlashSize) ||
        within(addr, size, kFlashBase, kFlashBase + kFlashSize))
        return DebugStatus::Unsupported;
    if (uint8_t* p = realMemoryAt(addr, size)) {  // SRAM / CCM
        std::memcpy(p, data, size);
        return DebugStatus::Ok;
    }
    if (!isPeripheralAddress(addr, size))
        return DebugStatus::InvalidAddress;
    const uint8_t* in = static_cast<const uint8_t*>(data);
    for (size_t off = 0; off < size;) {
        const uint32_t a = addr + uint32_t(off);
        const size_t left = size - off;
        const size_t n = ((a & 3u) == 0 && left >= 4) ? 4
                       : ((a & 1u) == 0 && left >= 2) ? 2
                                                      : 1;
        uint32_t v = 0;
        std::memcpy(&v, in + off, n);
        bus_.write(a, v, uint32_t(n));
        off += n;
    }
    return DebugStatus::Ok;
}

void Stm32G431::advanceTime(uint64_t cycles) {
    // ---------------------------------------------------------------------
    // Hard guard on the peripheral time base. The scheduler only ever advances
    // ONE emulation batch (<= kMaxBatchInsns = 40000 instructions), so a delta
    // far above that is a corrupt value upstream. On 2026-09-26 such a delta
    // (0xffe0defdeb6f217f = 1.8e19 cycles) reached the SysTick loop below and
    // made it spin ~1e14 times: the GUI froze solid and gdb showed the PC
    // looping inside advanceTime. Clamp + report instead of hanging, and keep
    // the log line so the value is on disk if it ever happens again.
    // ---------------------------------------------------------------------
    if (cycles > kMaxAdvanceDelta) {
        if (!hugeDeltaReported_) {
            hugeDeltaReported_ = true;
            char buf[192];
            std::snprintf(buf, sizeof buf,
                          "[sim] BAD time delta 0x%llX (%llu cycles) clamped to "
                          "%llu -- corrupt value upstream (freeze guard)",
                          (unsigned long long)cycles,
                          (unsigned long long)cycles,
                          (unsigned long long)kMaxAdvanceDelta);
            if (busLogger_) busLogger_(buf);
        }
        cycles = kMaxAdvanceDelta;
    }

    cycles_ += cycles;

    // SysTick runs on the processor clock (or HCLK/8 with CLKSOURCE=0)
    const uint32_t hclk = ahbHz();
    if (hclk == 0) return;

    // GPIO ports: virtual-time reference for register-driven level changes
    // (peripheral-driven changes pass their exact cycle themselves)
    gpioA.setNowCycles(cycles_);
    gpioB.setNowCycles(cycles_);
    gpioC.setNowCycles(cycles_);
    gpioD.setNowCycles(cycles_);
    gpioE.setNowCycles(cycles_);

    // Timers: clocked from the RCC tree (APB timer clock, x2 rule applied)
    const uint32_t apb1TimHz = rcc.apb1TimerHz();
    const uint32_t apb2TimHz = rcc.apb2TimerHz();
    for (int i = 0; i < kTimerCount; i++) {
        Timer* t = timers_[i];
        const uint32_t timHz = (t->traits().apb == 2) ? apb2TimHz : apb1TimHz;
        t->advance(cycles_, cycles, hclk, timHz);
    }

    // USART1: byte-level transmitter events (the baud rate is decoded from the
    // kernel clock at every use, never hard-coded)
    usart1.setNowCycles(cycles_);
    usart1.advance(cycles_);

    uint64_t ticks;
    if (ppb.systick.csr() & SysTickTimer::CLKSOURCE) {
        ticks = cycles;
    } else {
        ticks = cycles / 8;
    }
    if (ticks == 0) return;
    ppb.systick.advance(ticks, [this] { ppb.nvic.pend(15); });
}

Stm32G431::EventSource Stm32G431::nextEventSource() const {
    EventSource ev{~0ull, "none"};
    if (ppb.systick.csr() & SysTickTimer::CLKSOURCE) {
        ev = {ppb.systick.cyclesToEvent(), "SysTick"};
    } else {
        const uint64_t t = ppb.systick.cyclesToEvent();
        ev = {(t == ~0ull) ? t : t * 8, "SysTick/8"};
    }
    // timer IRQ events: only interrupts need a timely batch boundary (PWM
    // edges are generated exactly inside Timer::advance regardless)
    const uint32_t hclk = ahbHz();
    if (hclk) {
        for (int i = 0; i < kTimerCount; i++) {
            const Timer* t = timers_[i];
            const uint32_t timHz = timerClockHz(t->traits().apb);
            const uint64_t tev = t->cpuCyclesToNextIrq(hclk, timHz);
            if (tev < ev.cycles) ev = {tev, t->traits().name};
        }
    }
    // USART1 byte completion (the transmitter schedules exactly like a timer
    // compare event: the batch never crosses it)
    const uint64_t uev = usart1.cyclesToNextEvent();
    if (uev < ev.cycles) ev = {uev, "USART1"};
    return ev;
}
