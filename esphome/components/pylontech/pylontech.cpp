#include "pylontech.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::pylontech {

static const char *const TAG = "pylontech";

PylontechComponent::PylontechComponent() {}

void PylontechComponent::dump_config() {
  this->check_uart_settings(115200, 1, esphome::uart::UART_CONFIG_PARITY_NONE, 8);
  ESP_LOGCONFIG(TAG, "pylontech:");
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Connection with pylontech failed!");
  }

  for (PylontechListener *listener : this->listeners_) {
    listener->dump_config();
  }

  LOG_UPDATE_INTERVAL(this);
}

void PylontechComponent::setup() {
  while (this->available() != 0) {
    this->read();
  }
  this->rx_buffer_.reserve(6144);
}

void PylontechComponent::update() {
  this->rx_buffer_.clear();
  this->response_complete_ = false;
  this->write_str("pwr\n");
}

void PylontechComponent::loop() {
  // Drain the UART hardware buffer into rx_buffer_ as fast as possible.
  // Read at most 128 bytes per loop() call to stay within ESPHome's time budget.
  int bytes_read = 0;
  while (this->available() && bytes_read < 128) {
    int c = this->read();
    if (c >= 0) {
      this->rx_buffer_ += (char) c;
      bytes_read++;
    }
  }

  // Once the end-of-response marker arrives, parse the full response.
  if (!this->response_complete_ && this->rx_buffer_.find("$$") != std::string::npos) {
    this->response_complete_ = true;
    this->parse_response_();
  }
}

void PylontechComponent::parse_response_() {
  ESP_LOGD(TAG, "Parsing complete response (%d bytes)", (int) this->rx_buffer_.size());

  size_t pos = 0;
  while (pos < this->rx_buffer_.size()) {
    size_t end = this->rx_buffer_.find('\n', pos);
    if (end == std::string::npos) {
      end = this->rx_buffer_.size();
    }
    std::string line = this->rx_buffer_.substr(pos, end - pos);
    pos = end + 1;

    // Strip carriage returns
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    if (!line.empty()) {
      this->process_line_(line);
    }
  }
}

void PylontechComponent::process_line_(std::string &buffer) {
  ESP_LOGV(TAG, "Read from serial: %s", buffer.c_str());
  // clang-format off
  // example lines to parse:
  // Power Volt   Curr   Tempr  Tlow   Thigh  Vlow   Vhigh  Base.St  Volt.St  Curr.St  Temp.St  Coulomb  Time                 B.V.St   B.T.St   MosTempr M.T.St
  // 1     50548  8910   25000  24200  25000  3368   3371   Charge   Normal   Normal   Normal   97%      2021-06-30 20:49:45  Normal  Normal  22700    Normal
  // 1     46012  1255   9100   5300   5500   3047   3091   SysError Low      Normal   Normal   4%       2025-11-28 17:56:33  Low      Normal  7800     Normal
  // older hardware (no MosTempr):
  // 1     49871  6950   32600  29900  30300  3324   3325   Charge   Normal   Normal   Normal   54%      2026-04-18 07:20:49  Normal   Normal  -        -
  // newer firmware (with Tlow.Id columns):
  // Power Volt Curr Tempr Tlow Tlow.Id Thigh Thigh.Id Vlow Vlow.Id Vhigh Vhigh.Id Base.St Volt.St Curr.St Temp.St Coulomb Time                B.V.St B.T.St MosTempr M.T.St SysAlarm.St
  // 1     49405 0   17600 13700 8      14500 0        3293 2       3294   0       Idle    Normal  Normal  Normal  60%     2025-12-05 00:53:41 Normal Normal 16600    Normal Normal
  // clang-format on

  PylontechListener::LineContents l{};

  const char *cursor = buffer.c_str();
  char token_buf[TEXT_SENSOR_MAX_LEN] = {0};

  auto get_token = [&](char *out) -> void {
    while (*cursor == ' ' || *cursor == '\t') {
      cursor++;
    }
    if (*cursor == '\0') {
      out[0] = 0;
      return;
    }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r') {
      cursor++;
    }
    size_t len = std::min(static_cast<size_t>(cursor - start), static_cast<size_t>(TEXT_SENSOR_MAX_LEN - 1));
    memcpy(out, start, len);
    out[len] = 0;
  };

  get_token(token_buf);
  auto bat_val = parse_number<int>(token_buf);
  if (bat_val.has_value() && bat_val.value() > 0) {
    l.bat_num = bat_val.value();
  } else if (strcmp(token_buf, "Power") == 0) {
    // Header line -- detect whether this firmware includes Tlow.Id columns
    this->has_tlow_id_ = buffer.find("Tlow.Id") != std::string::npos;
    ESP_LOGD(TAG, "header line %s Tlow.Id: %s", this->has_tlow_id_ ? "with" : "without", buffer.c_str());
    return;
  } else {
    ESP_LOGV(TAG, "skip: %s", buffer.c_str());
    return;
  }

  auto parse_int_field = [&](int &field, const char *name) -> bool {
    get_token(token_buf);
    auto val = parse_number<int>(token_buf);
    if (val.has_value()) {
      field = val.value();
      return true;
    }
    // "-" means the battery slot is absent -- not an error
    ESP_LOGV(TAG, "bat %d: %s is absent/invalid ('%s')", l.bat_num, name, token_buf);
    return false;
  };

  auto parse_str_field = [&](char *field, const char *name) -> bool {
    get_token(field);
    if (strlen(field) < 2) {
      ESP_LOGV(TAG, "bat %d: %s too short", l.bat_num, name);
      return false;
    }
    return true;
  };

  if (!parse_int_field(l.volt,  "Volt"))  return;
  if (!parse_int_field(l.curr,  "Curr"))  return;
  if (!parse_int_field(l.tempr, "Tempr")) return;
  if (!parse_int_field(l.tlow,  "Tlow"))  return;
  if (this->has_tlow_id_) get_token(token_buf);  // Skip Tlow.Id
  if (!parse_int_field(l.thigh, "Thigh")) return;
  if (this->has_tlow_id_) get_token(token_buf);  // Skip Thigh.Id
  if (!parse_int_field(l.vlow,  "Vlow"))  return;
  if (this->has_tlow_id_) get_token(token_buf);  // Skip Vlow.Id
  if (!parse_int_field(l.vhigh, "Vhigh")) return;
  if (this->has_tlow_id_) get_token(token_buf);  // Skip Vhigh.Id

  if (!parse_str_field(l.base_st, "Base.St")) return;
  if (!parse_str_field(l.volt_st, "Volt.St")) return;
  if (!parse_str_field(l.curr_st, "Curr.St")) return;
  if (!parse_str_field(l.temp_st, "Temp.St")) return;

  // Coulomb has a % suffix
  get_token(token_buf);
  for (char &c : token_buf) {
    if (c == '%') {
      c = 0;
      break;
    }
  }
  auto coul_val = parse_number<int>(token_buf);
  if (!coul_val.has_value()) {
    ESP_LOGD(TAG, "invalid Coulomb in line %s", buffer.c_str());
    return;
  }
  l.coulomb = coul_val.value();

  get_token(token_buf);  // Skip Date
  get_token(token_buf);  // Skip Time
  get_token(token_buf);  // Skip B.V.St
  get_token(token_buf);  // Skip B.T.St

  // MosTempr is optional -- older hardware reports "-"
  get_token(token_buf);
  if (strlen(token_buf) > 0 && strcmp(token_buf, "-") != 0) {
    auto val = parse_number<int>(token_buf);
    if (val.has_value()) {
      l.mostempr = val.value();
      l.has_mostempr = true;
    }
  }

  ESP_LOGD(TAG, "successful line %s", buffer.c_str());

  for (PylontechListener *listener : this->listeners_) {
    listener->on_line_read(&l);
  }
}

}  // namespace esphome::pylontech
