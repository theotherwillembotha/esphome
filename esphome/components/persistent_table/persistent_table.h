#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/optional.h"

#include "nvs_flash.h"
#include "nvs.h"

#ifdef USE_PERSISTENT_TABLE_REST
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace persistent_table {

// Write a JSON-encoded string to the response stream, including surrounding quotes.
// Escapes double-quote, backslash, and common control characters.
inline void write_json_string(AsyncResponseStream *response, const char *str) {
  response->print('"');
  for (const char *p = str; *p != '\0'; p++) {
    switch (*p) {
      case '"':
      case '\\':
        response->print('\\');
        response->print(*p);
        break;
      case '\n':
        response->print('\\');
        response->print('n');
        break;
      case '\r':
        response->print('\\');
        response->print('r');
        break;
      case '\t':
        response->print('\\');
        response->print('t');
        break;
      default:
        response->print(*p);
        break;
    }
  }
  response->print('"');
}

}  // namespace persistent_table
}  // namespace esphome

#endif  // USE_PERSISTENT_TABLE_REST

namespace esphome {
namespace persistent_table {

class PersistentTableBase : public Component
#ifdef USE_PERSISTENT_TABLE_REST
    ,
    public AsyncWebHandler
#endif
{
 public:
  PersistentTableBase(const char *nvs_namespace, const char *table_id, size_t row_size, uint16_t max_rows);

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /// Read a row from NVS into buf. buf must be at least row_size_ bytes.
  bool read_row_(uint16_t index, uint8_t *buf) const;
  /// Write a row from buf to NVS. buf must be at least row_size_ bytes.
  bool write_row_(uint16_t index, const uint8_t *buf);
  /// Erase a row from NVS.
  bool erase_row_(uint16_t index);

  bool slot_occupied_(uint16_t index) const;
  int16_t find_free_slot_() const;
  void set_slot_(uint16_t index, bool occupied);
  void save_bitmap_();

  const char *nvs_namespace_;
  const char *table_id_;
  size_t row_size_;
  uint16_t max_rows_;
  mutable nvs_handle_t nvs_handle_{0};

  // One bit per row slot — kept in RAM, persisted to NVS key "_meta".
  // Supports up to 512 rows (64 bytes × 8 bits).
  static constexpr uint8_t BITMAP_BYTES = 64;
  uint8_t bitmap_[BITMAP_BYTES]{};

#ifdef USE_PERSISTENT_TABLE_REST
  // AsyncWebHandler — routes /api/table/{table_id}[/{key}]
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                  size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

  // Implemented by the generated subclass for type-specific serialization.
  virtual void rest_get_all_(AsyncWebServerRequest *request) = 0;
  virtual bool rest_post_body_(const char *json_body) = 0;
  virtual bool rest_delete_key_(const char *key_str) = 0;
#endif
};

}  // namespace persistent_table
}  // namespace esphome
