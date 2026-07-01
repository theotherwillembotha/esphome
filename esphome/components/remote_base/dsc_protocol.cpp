#include "dsc_protocol.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::remote_base {

static const char *const TAG = "remote.dsc";

// Standard timing (µs): short_width=250, long_width/bit_period=500
static constexpr uint32_t DSC_HALF_BIT_US = 250;
static constexpr uint32_t DSC_BIT_PERIOD_US = 500;

// WS4945 variant (µs): used by EV-DW4927, WS4975, WS4945 — approximately double
static constexpr uint32_t DSC_WS4945_HALF_BIT_US = 536;
static constexpr uint32_t DSC_WS4945_BIT_PERIOD_US = 1072;

// Minimum preamble duration to distinguish DSC from noise (2.5 ms standard, 5.6 ms smoke)
static constexpr uint32_t DSC_PREAMBLE_MIN_US = 2000;

// First space ≥ this threshold → WS4945 variant
static constexpr uint32_t DSC_VARIANT_THRESHOLD_US = 400;

void DscProtocol::encode(RemoteTransmitData *dst, const DscData &data) {
  // Encoding is not supported for DSC security contacts (receive-only protocol).
}

optional<DscData> DscProtocol::decode(RemoteReceiveData src) {
  // Scan for the preamble (≥ 2000 µs mark). OOK RF receivers often capture
  // spurious noise before the actual packet starts, so the preamble may not
  // be the very first element in the buffer.
  while (src.is_valid() && !src.peek_mark_at_least(DSC_PREAMBLE_MIN_US)) {
    src.advance();
  }
  if (!src.is_valid()) {
    return {};
  }
  src.advance();  // skip the preamble mark itself

  // Auto-detect timing variant from the first space following the preamble.
  // Standard first space ≈ 250 µs (half a bit period); WS4945 ≈ 536 µs.
  if (!src.is_valid()) {
    return {};
  }
  int32_t first_space = src.peek();
  if (first_space >= 0) {
    return {};  // Expected a space after the preamble, got a mark
  }
  uint32_t half_bit, bit_period;
  if (static_cast<uint32_t>(-first_space) >= DSC_VARIANT_THRESHOLD_US) {
    half_bit = DSC_WS4945_HALF_BIT_US;
    bit_period = DSC_WS4945_BIT_PERIOD_US;
  } else {
    half_bit = DSC_HALF_BIT_US;
    bit_period = DSC_BIT_PERIOD_US;
  }

  // Decode 48 bits from OOK_PULSE_RZ encoding.
  //
  // In OOK_PULSE_RZ each bit occupies one bit_period:
  //   Logic '1': half_bit of silence, then half_bit of mark
  //   Logic '0': bit_period of silence (no mark pulse)
  //
  // Consecutive '0' bits merge into a single long space, so the space before
  // each mark encodes the count of preceding zero bits:
  //   space = half_bit + n_zeros × bit_period
  //
  // Trailing '0' bits at the end generate no mark — raw[] is pre-zeroed so
  // they are handled implicitly.
  uint8_t raw[6] = {0};
  int bit_pos = 0;

  while (src.is_valid() && bit_pos < 48) {
    int32_t space_val = src.peek();
    if (space_val >= 0) {
      break;  // Got a mark when expecting a space — stop decoding
    }
    uint32_t space_us = static_cast<uint32_t>(-space_val);
    src.advance();

    // Derive the number of '0' bits from the space duration.
    // space = half_bit + n_zeros * bit_period  =>  n_zeros = (space - half_bit) / bit_period
    // Use signed arithmetic and clamp to guard against timing jitter below half_bit.
    int n_zeros = (static_cast<int32_t>(space_us) - static_cast<int32_t>(half_bit) +
                   static_cast<int32_t>(bit_period / 2)) /
                  static_cast<int32_t>(bit_period);
    if (n_zeros < 0) {
      n_zeros = 0;
    }
    bit_pos += n_zeros;

    // The mark that follows the space is the '1' bit pulse
    if (!src.is_valid()) {
      break;
    }
    if (src.peek() <= 0) {
      break;  // Malformed — expected a mark after the space
    }
    src.advance();

    // Record the '1' bit; bits are transmitted MSB first within each byte
    if (bit_pos < 48) {
      raw[bit_pos / 8] |= static_cast<uint8_t>(1 << (7 - (bit_pos % 8)));
      bit_pos++;
    }
  }

  // At minimum the sync bit at stream position 39 must have been received
  if (bit_pos < 40) {
    return {};
  }

  // Packet structure (48 bits, positions 0-47):
  //   Bits 0-3   : 4 leading sync '1' bits (raw[0] bits 7-4)
  //   Bits 4-11  : status byte data
  //   Bit 12     : sync '1'                (raw[1] bit 3)
  //   Bits 13-20 : ESN[23:16] + type data
  //   Bit 21     : sync '1'                (raw[2] bit 2)
  //   Bits 22-29 : ESN[15:8] data
  //   Bit 30     : sync '1'                (raw[3] bit 1)
  //   Bits 31-38 : ESN[7:0] data
  //   Bit 39     : sync '1'                (raw[4] bit 0)
  //   Bits 40-47 : CRC byte
  if (!(raw[0] & 0xF0) || !(raw[1] & 0x08) || !(raw[2] & 0x04) || !(raw[3] & 0x02) || !(raw[4] & 0x01)) {
    ESP_LOGV(TAG, "DSC sync check failed: %02X %02X %02X %02X %02X", raw[0], raw[1], raw[2], raw[3], raw[4]);
    return {};
  }

  // Strip the interleaved sync bits to recover the five data bytes
  uint8_t bytes[5];
  bytes[0] = static_cast<uint8_t>(((raw[0] & 0x0F) << 4) | ((raw[1] & 0xF0) >> 4));
  bytes[1] = static_cast<uint8_t>(((raw[1] & 0x07) << 5) | ((raw[2] & 0xF8) >> 3));
  bytes[2] = static_cast<uint8_t>(((raw[2] & 0x03) << 6) | ((raw[3] & 0xFC) >> 2));
  bytes[3] = static_cast<uint8_t>(((raw[3] & 0x01) << 7) | ((raw[4] & 0xFE) >> 1));
  bytes[4] = raw[5];

  // CRC-8: polynomial 0xF5, initial value 0x3D, LSB-first.
  // Including the CRC byte itself, a valid packet returns 0.
  if (esphome::crc8(bytes, 5, 0x3D, 0xF5) != 0) {
    ESP_LOGV(TAG, "DSC CRC failed: ESN=%06" PRIX32 " status=%02X CRC=%02X",
             static_cast<uint32_t>((bytes[1] << 16) | (bytes[2] << 8) | bytes[3]), bytes[0], bytes[4]);
    return {};
  }

  DscData out{};
  out.address = (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
  out.status = bytes[0];
  // 0x02 = contact closed/restored
  out.closed = (out.status & 0x02) == 0x02;
  // 0x40 = heartbeat (not an open/close event)
  out.event = (out.status & 0x40) != 0x40;
  // 0x08 = battery low
  out.battery_low = (out.status & 0x08) == 0x08;
  // 0x01 cleared or 0x10 set indicate tamper
  out.tamper = ((out.status & 0x01) != 0x01) || ((out.status & 0x10) == 0x10);

  return out;
}

void DscProtocol::dump(const DscData &data) {
  ESP_LOGD(TAG,
           "Received DSC: address=0x%06" PRIX32 " closed=%s event=%s battery_low=%s tamper=%s status=0x%02X",
           data.address, YESNO(data.closed), YESNO(data.event), YESNO(data.battery_low), YESNO(data.tamper),
           data.status);
}

}  // namespace esphome::remote_base
