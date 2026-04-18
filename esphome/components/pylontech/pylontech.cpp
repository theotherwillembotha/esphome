#include "pylontech.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace pylontech {

static const char *const TAG = "pylontech";
static const uint8_t ASCII_LF = 0x0A;

// End-of-response marker the battery sends after every command
static const char *const RESPONSE_END = "$$";

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
  rx_buffer_.reserve(6144);
}

void PylontechComponent::update() {
  rx_buffer_.clear();
  response_complete_ = false;
  this->write_str("pwr\n");
}

void PylontechComponent::loop() {
  // Phase 1: drain UART hardware buffer into rx_buffer_ as fast as possible
  // Read at most 128 bytes per loop() call to stay within ESPHome's time budget
  int bytes_read = 0;
  while (this->available() && bytes_read < 128) {
    int c = this->read();
    if (c >= 0) {
      rx_buffer_ += (char) c;
      bytes_read++;
    }
  }

  // Phase 2: check if complete response has arrived (look for $$ marker)
  if (!response_complete_ && rx_buffer_.find("$$") != std::string::npos) {
    response_complete_ = true;
    this->parse_response_();
  }
}

void PylontechComponent::parse_response_() {
  ESP_LOGD(TAG, "Parsing complete response (%d bytes)", rx_buffer_.size());

  // Split into lines and process each one
  size_t pos = 0;
  while (pos < rx_buffer_.size()) {
    size_t end = rx_buffer_.find('\n', pos);
    if (end == std::string::npos) {
      end = rx_buffer_.size();
    }
    std::string line = rx_buffer_.substr(pos, end - pos);
    pos = end + 1;

    // Strip \r characters
    line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

    if (!line.empty()) {
      this->process_line_(line);
    }
  }
}

void PylontechComponent::process_line_(std::string &buffer) {
  ESP_LOGV(TAG, "Line: %s", buffer.c_str());

  PylontechListener::LineContents l{};

  const char *cursor = buffer.c_str();
  char token_buf[TEXT_SENSOR_MAX_LEN] = {0};

  auto get_token = [&](char *out) -> void {
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '\0') { out[0] = 0; return; }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r') cursor++;
    size_t len = std::min(static_cast<size_t>(cursor - start),
                          static_cast<size_t>(TEXT_SENSOR_MAX_LEN - 1));
    memcpy(out, start, len);
    out[len] = 0;
  };

  // Parse battery number
  get_token(token_buf);
  auto bat_val = parse_number<int>(token_buf);
  if (bat_val.has_value() && bat_val.value() > 0) {
    l.bat_num = bat_val.value();
  } else if (strcmp(token_buf, "Power") == 0) {
    this->has_tlow_id_ = buffer.find("Tlow.Id") != std::string::npos;
    ESP_LOGD(TAG, "header: %s Tlow.Id", this->has_tlow_id_ ? "with" : "without");
    return;
  } else {
    ESP_LOGV(TAG, "skip: %s", buffer.c_str());
    return;
  }

  // Helper lambdas
  auto parse_int_field = [&](int &field, const char *name) -> bool {
    get_token(token_buf);
    auto val = parse_number<int>(token_buf);
    if (val.has_value()) {
      field = val.value();
      return true;
    }
    // "-" means absent (Absent batteries) — not an error worth logging at WARN
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

  // Parse all fields — abort silently for Absent batteries (all fields will be "-")
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

  // Coulomb (has % suffix)
  get_token(token_buf);
  for (char &c : token_buf) { if (c == '%') { c = 0; break; } }
  auto coul_val = parse_number<int>(token_buf);
  if (!coul_val.has_value()) {
    ESP_LOGV(TAG, "bat %d: Coulomb invalid", l.bat_num);
    return;
  }
  l.coulomb = coul_val.value();

  get_token(token_buf);  // Skip Date
  get_token(token_buf);  // Skip Time
  get_token(token_buf);  // Skip B.V.St
  get_token(token_buf);  // Skip B.T.St

  // MosTempr — optional, older hardware reports "-"
  get_token(token_buf);
  if (strlen(token_buf) > 0 && strcmp(token_buf, "-") != 0) {
    auto val = parse_number<int>(token_buf);
    if (val.has_value()) {
      l.mostempr = val.value();
      l.has_mostempr = true;
    }
  }

  ESP_LOGD(TAG, "bat %d: volt=%d curr=%d soc=%d%% mostempr=%s",
           l.bat_num, l.volt, l.curr, l.coulomb,
           l.has_mostempr ? std::to_string(l.mostempr).c_str() : "-");

  for (PylontechListener *listener : this->listeners_) {
    listener->on_line_read(&l);
  }
}

}  // namespace pylontech
}  // namespace esphome
