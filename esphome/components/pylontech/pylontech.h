#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/uart/uart.h"

namespace esphome::pylontech {

static const uint8_t TEXT_SENSOR_MAX_LEN = 14;

class PylontechListener {
 public:
  struct LineContents {
    int bat_num = 0, volt, curr, tempr, tlow, thigh, vlow, vhigh, coulomb, mostempr;
    bool has_mostempr = false;
    char base_st[TEXT_SENSOR_MAX_LEN] = {0}, volt_st[TEXT_SENSOR_MAX_LEN] = {0}, curr_st[TEXT_SENSOR_MAX_LEN] = {0},
         temp_st[TEXT_SENSOR_MAX_LEN] = {0};
  };

  virtual void on_line_read(LineContents *line);
  virtual void dump_config();
};

class PylontechComponent final : public PollingComponent, public uart::UARTDevice {
 public:
  PylontechComponent();

  /// Schedule data readings.
  void update() override;
  /// Read data once available
  void loop() override;
  /// Setup the sensor and test for a connection.
  void setup() override;
  void dump_config() override;

  void register_listener(PylontechListener *listener) { this->listeners_.push_back(listener); }

 protected:
  void parse_response_();
  void process_line_(std::string &buffer);

  std::string rx_buffer_;
  bool response_complete_ = false;
  bool has_tlow_id_ = false;

  std::vector<PylontechListener *> listeners_{};
};

}  // namespace esphome::pylontech
