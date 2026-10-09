#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// ============================================================================
// Virtual PC-side serial peer -- the USB-serial port of the board's DAP-Link
// (what a real serial terminal on the PC would talk to).
//
// BYTE LEVEL ONLY. The peer queues bytes, spaces them by the line rate and
// hands them over one frame at a time; it knows NOTHING about USART registers,
// NVIC, interrupts or the GPIO pins. The BOARD (Ct117eM4) owns the wiring
// (which is exactly how the DAP-Link is soldered to PA9/PA10 on the real
// board) and the SoC applies the AF routing, so a wrong AF really loses the
// bytes in both directions.
//
// Timing: one frame = 10 bit times for 8N1 (start + 8 data + stop) at the baud
// rate the BOARD read from the firmware's BRR -- the peer never assumes 9600
// (or any other rate). While a frame is received the RX line is driven low
// with its exact virtual cycle and it idles high, so a pin observer sees a
// real byte-level waveform.
//
// All times are simulator VIRTUAL time (HCLK cycles); nothing here uses a wall
// clock, a timer or a thread.
// ============================================================================
class VirtualSerialPeer {
public:
    static constexpr uint32_t kDefaultBaud = 9600;

    // Board reset: drop both queues and restart the frame timeline at @now.
    void reset(uint64_t nowCycles) {
        toMcu_.clear();
        fromMcu_.clear();
        queueTail_ = nowCycles;
        now_ = nowCycles;
        rxLow_ = false;
    }

    // Line rate used to space PC -> MCU bytes. The board feeds it from the
    // decoded BRR; a 0 (USART not configured / no kernel clock) is ignored so
    // the last known rate stays.
    void setBaud(uint32_t baud) {
        if (baud >= 50u && baud <= 10'000'000u) baud_ = baud;
    }
    uint32_t baud() const { return baud_; }

    // ---- PC -> MCU: bytes typed in the terminal ----
    // The frame times are assigned when the engines picks the queue up (so the
    // spacing always follows the baud rate that is in effect then).
    void sendToMcu(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; i++) toMcu_.push_back(Frame{data[i], kUnset});
        fromPcTotal_ += n;
    }
    size_t pendingToMcu() const { return toMcu_.size(); }

    // ---- MCU -> PC: bytes delivered by the SoC transmitter ----
    void onByteFromMcu(uint8_t b) {
        fromMcu_.push_back(b);
        toPcTotal_++;
    }
    // Bytes received from the MCU since the last call (never the history).
    std::vector<uint8_t> takeFromMcu() {
        std::vector<uint8_t> out(fromMcu_.begin(), fromMcu_.end());
        fromMcu_.clear();
        return out;
    }
    size_t pendingFromMcu() const { return fromMcu_.size(); }
    uint64_t bytesToPcTotal() const { return toPcTotal_; }
    uint64_t bytesFromPcTotal() const { return fromPcTotal_; }

    // Engine path: run the receiver up to @now. @driveRx is called for every
    // RX pin level change with its exact virtual cycle; @deliver is called with
    // a byte whose frame completed (the board hands it to the SoC, which
    // applies the RX pin routing).
    void advance(uint64_t now, uint32_t hclk,
                 const std::function<void(bool level, uint64_t cpuCycle)>& driveRx,
                 const std::function<void(uint8_t)>& deliver) {
        now_ = now;
        hclk_ = hclk;
        const uint64_t frame = frameCycles();
        if (frame == 0) return;
        const uint64_t lowWindow = lowWindowCycles();
        // assign the frame times of the bytes queued since the last call
        for (Frame& f : toMcu_) {
            if (f.start != kUnset) continue;
            f.start = (queueTail_ > now) ? queueTail_ : now;
            queueTail_ = f.start + frame;
        }
        int guard = 0;
        while (!toMcu_.empty() && guard++ < 4096) {
            const Frame& f = toMcu_.front();
            if (f.start == kUnset) break;
            const uint64_t lowEnd = f.start + lowWindow;
            const uint64_t end = f.start + frame;
            if (now < f.start) break;
            if (now < lowEnd) {
                if (!rxLow_) {
                    rxLow_ = true;
                    driveRx(false, f.start);  // start bit: the line goes low
                }
                break;
            }
            if (now < end) {
                if (rxLow_) {
                    rxLow_ = false;
                    driveRx(true, lowEnd);  // stop bit: back to idle high
                }
                break;
            }
            if (rxLow_) {
                rxLow_ = false;
                driveRx(true, lowEnd);
            }
            deliver(f.byte);  // frame complete: the byte reached the RDR
            toMcu_.pop_front();
        }
        if (toMcu_.empty() && rxLow_) {
            rxLow_ = false;
            driveRx(true, now);
        }
    }

    // HCLK cycles until the next receiver transition (~0ull = none). A real
    // distance is returned whenever a transition is still ahead: returning 0
    // would clamp the emulation batch to its 64-instruction floor and turn
    // every received byte into a storm of tiny batches (the stop-bit window is
    // a whole bit time long), which throttles the whole simulation.
    uint64_t cyclesToNextEvent() const {
        if (toMcu_.empty()) return ~0ull;
        const uint64_t frame = frameCycles();
        if (frame == 0) return ~0ull;
        const Frame& f = toMcu_.front();
        if (f.start == kUnset) return 0;  // not scheduled yet: handle right away
        const uint64_t lowEnd = f.start + lowWindowCycles();
        const uint64_t end = f.start + frame;
        uint64_t next = lowEnd;
        if (!rxLow_) next = (now_ < lowEnd) ? f.start : end;
        else if (now_ >= lowEnd) next = end;
        return (next > now_) ? (next - now_) : 1;
    }

    // ---- inspection ----
    bool rxLineLow() const { return rxLow_; }

private:
    static constexpr uint64_t kUnset = ~0ull;

    struct Frame {
        uint8_t byte;
        uint64_t start;  // frame start (start bit) in virtual HCLK cycles
    };

    uint64_t bitCycles() const {
        if (baud_ == 0 || hclk_ == 0) return 0;
        const double c = double(hclk_) / double(baud_);
        return c < 1.0 ? 1u : uint64_t(c + 0.5);
    }
    uint64_t frameCycles() const { return bitCycles() * 10; }  // 8N1
    uint64_t lowWindowCycles() const { return bitCycles() * 9; }

    uint32_t baud_ = kDefaultBaud;
    std::deque<Frame> toMcu_;
    std::vector<uint8_t> fromMcu_;
    uint64_t queueTail_ = 0;  // when the next PC -> MCU frame starts
    uint64_t now_ = 0;
    uint32_t hclk_ = 0;
    bool rxLow_ = false;
    uint64_t toPcTotal_ = 0;    // bytes the MCU sent to the terminal
    uint64_t fromPcTotal_ = 0;  // bytes the terminal sent to the MCU
};