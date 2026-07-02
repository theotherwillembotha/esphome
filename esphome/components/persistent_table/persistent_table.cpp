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
    // Register the singleton index handler the first time any table sets up.
    auto &index = TablesIndexHandler::instance();
    index.register_table(this->table_id_);
    if (!index.is_registered()) {
      index.mark_registered();
      web_server_base::global_web_server_base->add_handler(&index);
    }
    web_server_base::global_web_server_base->add_handler(this);
    ESP_LOGD(TAG, "Table '%s': REST /api/table/%s  UI /table/%s", this->table_id_, this->table_id_,
             this->table_id_);
  } else {
    ESP_LOGW(TAG, "Table '%s': web_server_base not available, REST/UI disabled", this->table_id_);
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
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  request->url_to(url_buf);
  size_t id_len = strlen(this->table_id_);

  // Match /api/table/{id}  or  /api/table/{id}/{key}
  static const char API[] = "/api/table/";
  static const size_t API_LEN = sizeof(API) - 1;
  if (strncmp(url_buf, API, API_LEN) == 0) {
    const char *after = url_buf + API_LEN;
    if (strncmp(after, this->table_id_, id_len) == 0) {
      char next = after[id_len];
      return next == '\0' || next == '/';
    }
  }

  // Match /table/{id}  (UI editor)
  static const char UI[] = "/table/";
  static const size_t UI_LEN = sizeof(UI) - 1;
  if (strncmp(url_buf, UI, UI_LEN) == 0) {
    const char *after = url_buf + UI_LEN;
    return strncmp(after, this->table_id_, id_len) == 0 && after[id_len] == '\0';
  }

  return false;
}

void PersistentTableBase::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  request->url_to(url_buf);

  // Serve the HTML editor for /table/{id}
  static const char UI[] = "/table/";
  static const size_t UI_LEN = sizeof(UI) - 1;
  if (strncmp(url_buf, UI, UI_LEN) == 0) {
    request->send(200, "text/html", this->get_ui_html_());
    return;
  }

  switch (request->method()) {
    case HTTP_GET:
      this->rest_get_all_(request);
      break;

    case HTTP_POST: {
      // On ESP-IDF the web server only calls handleBody() for
      // application/x-www-form-urlencoded.  For application/json we must read
      // the body ourselves via the underlying IDF request handle.
      std::string json_body;
      httpd_req_t *raw_req = static_cast<httpd_req_t *>(*request);
      size_t content_len = raw_req->content_len;
      if (content_len > 0 && content_len <= 4096) {
        json_body.resize(content_len);
        int ret = httpd_req_recv(raw_req, &json_body[0], static_cast<int>(content_len));
        if (ret > 0) {
          json_body.resize(static_cast<size_t>(ret));
        } else {
          json_body.clear();
        }
      }
      // Fallback: body accumulated via handleBody() (Arduino/non-IDF path).
      if (json_body.empty() && !this->body_buf_.empty()) {
        json_body = std::move(this->body_buf_);
      }
      this->body_buf_.clear();
      bool ok = !json_body.empty() && this->rest_post_body_(json_body.c_str());
      if (ok) {
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"invalid data\"}");
      }
      break;
    }

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
    this->body_buf_.clear();
    this->body_buf_.reserve(total);
  }
  this->body_buf_.append(reinterpret_cast<const char *>(data), len);
}

// ---------------------------------------------------------------------------
// TablesIndexHandler — /tables discovery page
// ---------------------------------------------------------------------------

void TablesIndexHandler::handleRequest(AsyncWebServerRequest *request) {
  auto *resp = request->beginResponseStream("text/html");
  resp->print(
      "<!DOCTYPE html><html><head>"
      "<meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>ESPHome Tables</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:32px}"
      "h1{color:#333;margin-bottom:4px}"
      "p{color:#666;margin-bottom:20px}"
      "ul{list-style:none;padding:0}"
      "li{margin:10px 0}"
      "a{display:inline-block;padding:10px 20px;background:#2196F3;color:#fff;"
      "text-decoration:none;border-radius:6px;font-size:.95em}"
      "a:hover{background:#1976D2}"
      ".foot{margin-top:32px;font-size:.8em;color:#aaa}"
      "</style></head><body>"
      "<h1>&#128204; Persistent Tables</h1>"
      "<p>Select a table to view or edit its contents:</p>"
      "<ul>");
  for (const char *id : this->table_ids_) {
    resp->printf("<li><a href=\"/table/%s\">%s</a></li>", id, id);
  }
  resp->print(
      "</ul>"
      "<p class=\"foot\">ESPHome persistent_table &mdash; "
      "REST API at <code>/api/table/{id}</code></p>"
      "</body></html>");
  request->send(resp);
}

#endif  // USE_PERSISTENT_TABLE_REST

}  // namespace persistent_table
}  // namespace esphome
