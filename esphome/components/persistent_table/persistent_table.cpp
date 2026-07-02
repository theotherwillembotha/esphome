#include "persistent_table.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace persistent_table {

static const char *const TAG = "persistent_table";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PersistentTableBase::PersistentTableBase(const char *nvs_namespace, const char *table_id, size_t row_size,
                                         uint16_t max_rows)
    : nvs_namespace_(nvs_namespace), table_id_(table_id), row_size_(row_size), max_rows_(max_rows) {}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void PersistentTableBase::setup() {
  esp_err_t err = nvs_open(this->nvs_namespace_, NVS_READWRITE, &this->nvs_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Table '%s': failed to open NVS namespace '%s': %s", this->table_id_, this->nvs_namespace_,
             esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // Load the occupancy bitmap from NVS.
  size_t bitmap_size = BITMAP_BYTES;
  err = nvs_get_blob(this->nvs_handle_, "_meta", this->bitmap_, &bitmap_size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGD(TAG, "Table '%s': no existing data, starting fresh", this->table_id_);
  } else if (err != ESP_OK) {
    ESP_LOGW(TAG, "Table '%s': failed to load bitmap: %s", this->table_id_, esp_err_to_name(err));
  } else {
    uint16_t count = 0;
    for (uint16_t i = 0; i < this->max_rows_; i++) {
      if (this->slot_occupied_(i))
        count++;
    }
    ESP_LOGD(TAG, "Table '%s': loaded with %u rows", this->table_id_, count);
  }

#ifdef USE_PERSISTENT_TABLE_REST
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->add_handler(this);
    ESP_LOGD(TAG, "Table '%s': REST API registered at /api/table/%s", this->table_id_, this->table_id_);
  } else {
    ESP_LOGW(TAG, "Table '%s': web_server_base not available, REST API disabled", this->table_id_);
  }
#endif
}

void PersistentTableBase::dump_config() {
  ESP_LOGCONFIG(TAG, "Persistent Table '%s':", this->table_id_);
  ESP_LOGCONFIG(TAG, "  NVS Namespace: %s", this->nvs_namespace_);
  ESP_LOGCONFIG(TAG, "  Row Size: %u bytes", (unsigned) this->row_size_);
  uint16_t count = 0;
  for (uint16_t i = 0; i < this->max_rows_; i++) {
    if (this->slot_occupied_(i))
      count++;
  }
  ESP_LOGCONFIG(TAG, "  Rows: %u / %u", count, this->max_rows_);
#ifdef USE_PERSISTENT_TABLE_REST
  ESP_LOGCONFIG(TAG, "  REST API: /api/table/%s", this->table_id_);
#endif
}

// ---------------------------------------------------------------------------
// NVS row operations
// ---------------------------------------------------------------------------

bool PersistentTableBase::read_row_(uint16_t index, uint8_t *buf) const {
  char key[8];
  snprintf(key, sizeof(key), "r%u", index);
  size_t size = this->row_size_;
  esp_err_t err = nvs_get_blob(this->nvs_handle_, key, buf, &size);
  if (err != ESP_OK) {
    ESP_LOGV(TAG, "Table '%s': read_row %u failed: %s", this->table_id_, index, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool PersistentTableBase::write_row_(uint16_t index, const uint8_t *buf) {
  char key[8];
  snprintf(key, sizeof(key), "r%u", index);
  esp_err_t err = nvs_set_blob(this->nvs_handle_, key, buf, this->row_size_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Table '%s': write_row %u failed: %s", this->table_id_, index, esp_err_to_name(err));
    return false;
  }
  err = nvs_commit(this->nvs_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Table '%s': commit row %u failed: %s", this->table_id_, index, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool PersistentTableBase::erase_row_(uint16_t index) {
  char key[8];
  snprintf(key, sizeof(key), "r%u", index);
  esp_err_t err = nvs_erase_key(this->nvs_handle_, key);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGE(TAG, "Table '%s': erase_row %u failed: %s", this->table_id_, index, esp_err_to_name(err));
    return false;
  }
  nvs_commit(this->nvs_handle_);
  return true;
}

// ---------------------------------------------------------------------------
// Bitmap operations
// ---------------------------------------------------------------------------

bool PersistentTableBase::slot_occupied_(uint16_t index) const {
  if (index >= this->max_rows_)
    return false;
  return (this->bitmap_[index / 8] >> (index % 8)) & 1u;
}

int16_t PersistentTableBase::find_free_slot_() const {
  for (uint16_t i = 0; i < this->max_rows_; i++) {
    if (!this->slot_occupied_(i))
      return static_cast<int16_t>(i);
  }
  return -1;
}

void PersistentTableBase::set_slot_(uint16_t index, bool occupied) {
  if (index >= this->max_rows_)
    return;
  if (occupied) {
    this->bitmap_[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
  } else {
    this->bitmap_[index / 8] &= static_cast<uint8_t>(~(1u << (index % 8)));
  }
}

void PersistentTableBase::save_bitmap_() {
  esp_err_t err = nvs_set_blob(this->nvs_handle_, "_meta", this->bitmap_, BITMAP_BYTES);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Table '%s': failed to save bitmap: %s", this->table_id_, esp_err_to_name(err));
    return;
  }
  nvs_commit(this->nvs_handle_);
}

// ---------------------------------------------------------------------------
// REST handler (AsyncWebHandler)
// ---------------------------------------------------------------------------

#ifdef USE_PERSISTENT_TABLE_REST

bool PersistentTableBase::canHandle(AsyncWebServerRequest *request) const {
  // Match /api/table/{table_id} or /api/table/{table_id}/{key}
  const char *url = request->url().c_str();
  static const char PREFIX[] = "/api/table/";
  static const size_t PREFIX_LEN = sizeof(PREFIX) - 1;
  if (strncmp(url, PREFIX, PREFIX_LEN) != 0)
    return false;
  const char *after = url + PREFIX_LEN;
  size_t id_len = strlen(this->table_id_);
  if (strncmp(after, this->table_id_, id_len) != 0)
    return false;
  char next = after[id_len];
  return next == '\0' || next == '/';
}

void PersistentTableBase::handleRequest(AsyncWebServerRequest *request) {
  const char *url = request->url().c_str();
  static const char PREFIX[] = "/api/table/";
  static const size_t PREFIX_LEN = sizeof(PREFIX) - 1;
  const char *after_id = url + PREFIX_LEN + strlen(this->table_id_);

  switch (request->method()) {
    case HTTP_GET:
      this->rest_get_all_(request);
      break;

    case HTTP_POST: {
      const char *body = (request->_tempObject != nullptr) ? static_cast<const char *>(request->_tempObject) : "";
      bool ok = this->rest_post_body_(body);
      if (request->_tempObject != nullptr) {
        delete[] static_cast<char *>(request->_tempObject);
        request->_tempObject = nullptr;
      }
      if (ok) {
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"invalid data\"}");
      }
      break;
    }

    case HTTP_DELETE:
      if (*after_id == '/') {
        const char *key_str = after_id + 1;
        if (this->rest_delete_key_(key_str)) {
          request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
          request->send(404, "application/json", "{\"error\":\"not found\"}");
        }
      } else {
        request->send(400, "application/json", "{\"error\":\"key required in URL\"}");
      }
      break;

    default:
      request->send(405, "text/plain", "Method Not Allowed");
      break;
  }
}

void PersistentTableBase::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                     size_t total) {
  if (total == 0)
    return;
  if (index == 0) {
    request->_tempObject = new char[total + 1];
  }
  if (request->_tempObject != nullptr) {
    memcpy(static_cast<char *>(request->_tempObject) + index, data, len);
    if (index + len == total) {
      static_cast<char *>(request->_tempObject)[total] = '\0';
    }
  }
}

#endif  // USE_PERSISTENT_TABLE_REST

}  // namespace persistent_table
}  // namespace esphome
