#pragma once

#include "esphome/core/component.h"
#include "remote_base.h"

namespace esphome::remote_base {

struct DscData {
  uint32_t address;  // 24-bit Electronic Serial Number (ESN)
  uint8_t status;    // Raw status byte
  bool closed;       // Contact state: true = closed/restored, false = open
  bool event;        // true = open/close event, false = periodic heartbeat
  bool battery_low;
  bool tamper;

  bool operator==(const DscData &rhs) const { return this->address == rhs.address; }
};

class DscProtocol : public RemoteProtocol<DscData> {
 public:
  void encode(RemoteTransmitData *dst, const DscData &data) override;
  optional<DscData> decode(RemoteReceiveData src) override;
  void dump(const DscData &data) override;
};

DECLARE_REMOTE_PROTOCOL(Dsc)

}  // namespace esphome::remote_base
