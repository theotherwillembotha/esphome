import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome import automation
from esphome.automation import Trigger

CODEOWNERS = ["@wjl"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = True

persistent_table_ns = cg.esphome_ns.namespace("persistent_table")
PersistentTableBase = persistent_table_ns.class_("PersistentTableBase", cg.Component)

CONF_NAMESPACE = "namespace"
CONF_MAX_ROWS = "max_rows"
CONF_COLUMNS = "columns"
CONF_KEY = "key"
CONF_MAX_LENGTH = "max_length"
CONF_DEFAULT = "default"
CONF_REST_API = "rest_api"
CONF_NAME = "name"
CONF_ON_ROW_ADDED = "on_row_added"
CONF_ON_ROW_UPDATED = "on_row_updated"
CONF_ON_ROW_DELETED = "on_row_deleted"

# (C++ type string, byte size) — string uses None/None (size depends on max_length)
COLUMN_TYPES = {
    "uint8": ("uint8_t", 1),
    "uint16": ("uint16_t", 2),
    "uint32": ("uint32_t", 4),
    "int32": ("int32_t", 4),
    "float": ("float", 4),
    "bool": ("bool", 1),
    "string": (None, None),
}

COLUMN_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME): cv.valid_name,
    cv.Required("type"): cv.one_of(*COLUMN_TYPES.keys(), lower=True),
    cv.Optional(CONF_KEY, default=False): cv.boolean,
    cv.Optional(CONF_MAX_LENGTH): cv.positive_int,
    cv.Optional(CONF_DEFAULT): cv.Any(cv.int_, cv.float_, cv.boolean, cv.string),
})


def _validate_columns(columns):
    key_cols = [c for c in columns if c.get(CONF_KEY, False)]
    if len(key_cols) != 1:
        raise cv.Invalid("Exactly one column must be marked with 'key: true'")
    for col in columns:
        if col["type"] == "string" and CONF_MAX_LENGTH not in col:
            raise cv.Invalid(
                f"Column '{col[CONF_NAME]}' of type 'string' requires 'max_length'"
            )
    return columns


# Use a generic Trigger placeholder in the schema; the real trigger types are
# generated per-table and the IDs are retyped in to_code().
_TriggerPlaceholder = Trigger.template()

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(PersistentTableBase),
    cv.Required(CONF_NAMESPACE): cv.All(cv.string, cv.Length(min=1, max=15)),
    cv.Optional(CONF_MAX_ROWS, default=100): cv.int_range(min=1, max=512),
    cv.Required(CONF_COLUMNS): cv.All(cv.ensure_list(COLUMN_SCHEMA), _validate_columns),
    cv.Optional(CONF_REST_API, default=True): cv.boolean,
    cv.Optional(CONF_ON_ROW_ADDED): automation.validate_automation(
        {cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(_TriggerPlaceholder)}
    ),
    cv.Optional(CONF_ON_ROW_UPDATED): automation.validate_automation(
        {cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(_TriggerPlaceholder)}
    ),
    cv.Optional(CONF_ON_ROW_DELETED): automation.validate_automation(
        {cv.GenerateID(automation.CONF_TRIGGER_ID): cv.declare_id(_TriggerPlaceholder)}
    ),
}).extend(cv.COMPONENT_SCHEMA)


# ---------------------------------------------------------------------------
# Code generation helpers
# ---------------------------------------------------------------------------

def _class_base(table_id_str: str) -> str:
    """'dsc_devices' -> 'DscDevices'"""
    return "".join(word.capitalize() for word in table_id_str.split("_"))


def _col_field_decl(col: dict) -> str:
    """Return the C++ field declaration for a column (without semicolon)."""
    col_type = col["type"]
    name = col[CONF_NAME]
    if col_type == "string":
        return f"char {name}[{col[CONF_MAX_LENGTH] + 1}]"
    return f"{COLUMN_TYPES[col_type][0]} {name}"


def _key_match_expr(col: dict, lhs: str, rhs: str) -> str:
    """Expression that returns true when the key column values are equal."""
    if col["type"] == "string":
        return f'strncmp({lhs}, {rhs}, sizeof({lhs})) == 0'
    return f"{lhs} == {rhs}"


def _generate_find(struct_name: str, key_col: dict) -> str:
    key = key_col[CONF_NAME]
    col_type = key_col["type"]
    cpp_type = "const char *" if col_type == "string" else COLUMN_TYPES[col_type][0]
    match = _key_match_expr(key_col, f"row->{key}", "key")
    return f"""\
  esphome::optional<{struct_name}> find({cpp_type} key) const {{
    uint8_t buf[sizeof({struct_name})];
    for (uint16_t i = 0; i < this->max_rows_; i++) {{
      if (!this->slot_occupied_(i)) continue;
      if (!this->read_row_(i, buf)) continue;
      const auto *row = reinterpret_cast<const {struct_name} *>(buf);
      if ({match}) return *row;
    }}
    return {{}};
  }}"""


def _generate_upsert(struct_name: str, key_col: dict) -> str:
    key = key_col[CONF_NAME]
    col_type = key_col["type"]
    if col_type == "string":
        match = f'strncmp(existing->{key}, row.{key}, sizeof(existing->{key})) == 0'
    else:
        match = f"existing->{key} == row.{key}"
    return f"""\
  bool upsert(const {struct_name} &row) {{
    uint8_t buf[sizeof({struct_name})];
    for (uint16_t i = 0; i < this->max_rows_; i++) {{
      if (!this->slot_occupied_(i)) continue;
      if (!this->read_row_(i, buf)) continue;
      const auto *existing = reinterpret_cast<const {struct_name} *>(buf);
      if ({match}) {{
        if (!this->write_row_(i, reinterpret_cast<const uint8_t *>(&row))) return false;
        this->on_row_updated_callback_.call(row);
        return true;
      }}
    }}
    int16_t slot = this->find_free_slot_();
    if (slot < 0) return false;
    if (!this->write_row_(slot, reinterpret_cast<const uint8_t *>(&row))) return false;
    this->set_slot_(slot, true);
    this->save_bitmap_();
    this->on_row_added_callback_.call(row);
    return true;
  }}"""


def _generate_remove(struct_name: str, key_col: dict) -> str:
    key = key_col[CONF_NAME]
    col_type = key_col["type"]
    cpp_type = "const char *" if col_type == "string" else COLUMN_TYPES[col_type][0]
    match = _key_match_expr(key_col, f"row->{key}", "key")
    return f"""\
  bool remove({cpp_type} key) {{
    uint8_t buf[sizeof({struct_name})];
    for (uint16_t i = 0; i < this->max_rows_; i++) {{
      if (!this->slot_occupied_(i)) continue;
      if (!this->read_row_(i, buf)) continue;
      const auto *row = reinterpret_cast<const {struct_name} *>(buf);
      if ({match}) {{
        this->on_row_deleted_callback_.call(*row);
        this->erase_row_(i);
        this->set_slot_(i, false);
        this->save_bitmap_();
        return true;
      }}
    }}
    return false;
  }}"""


def _generate_iterate(struct_name: str) -> str:
    return f"""\
  template<typename F>
  void iterate(F &&callback) const {{
    uint8_t buf[sizeof({struct_name})];
    for (uint16_t i = 0; i < this->max_rows_; i++) {{
      if (!this->slot_occupied_(i)) continue;
      if (!this->read_row_(i, buf)) continue;
      const auto *row = reinterpret_cast<const {struct_name} *>(buf);
      callback(*row);
    }}
  }}"""


def _generate_rest_get(struct_name: str, columns: list) -> str:
    lines = []
    for i, col in enumerate(columns):
        name = col[CONF_NAME]
        col_type = col["type"]
        if i > 0:
            lines.append('      response->print(",");')
        lines.append(f'      response->print("\\"{name}\\":");')
        if col_type == "string":
            lines.append(
                f"      esphome::persistent_table::write_json_string(response, row->{name});"
            )
        elif col_type == "bool":
            lines.append(f'      response->print(row->{name} ? "true" : "false");')
        elif col_type == "float":
            lines.append(f'      response->printf("%g", (double) row->{name});')
        else:
            lines.append(f'      response->printf("%ld", (long) row->{name});')
    fields_code = "\n".join(lines)
    return f"""\
  void rest_get_all_(AsyncWebServerRequest *request) override {{
    auto *response = request->beginResponseStream("application/json");
    response->print("[");
    bool first = true;
    uint8_t buf[sizeof({struct_name})];
    for (uint16_t i = 0; i < this->max_rows_; i++) {{
      if (!this->slot_occupied_(i)) continue;
      if (!this->read_row_(i, buf)) continue;
      const auto *row = reinterpret_cast<const {struct_name} *>(buf);
      if (!first) response->print(",");
      first = false;
      response->print("{{");
{fields_code}
      response->print("}}");
    }}
    response->print("]");
    request->send(response);
  }}"""


def _generate_rest_post(struct_name: str, columns: list) -> str:
    lines = [
        f"    {struct_name} row{{}};",
        "    DynamicJsonDocument doc(512);",
        "    if (deserializeJson(doc, json_body) != DeserializationError::Ok) return false;",
    ]
    key_col = next(c for c in columns if c.get(CONF_KEY, False))
    lines.append(f'    if (!doc.containsKey("{key_col[CONF_NAME]}")) return false;')

    for col in columns:
        name = col[CONF_NAME]
        col_type = col["type"]
        default = col.get(CONF_DEFAULT)

        if col_type == "string":
            default_val = f'"{default}"' if default is not None else '""'
            lines += [
                "    {",
                f'      const char *v = doc["{name}"] | {default_val};',
                f"      strncpy(row.{name}, v, sizeof(row.{name}) - 1);",
                f"      row.{name}[sizeof(row.{name}) - 1] = '\\0';",
                "    }",
            ]
        elif col_type == "bool":
            default_val = "true" if default else "false"
            lines.append(f'    row.{name} = doc["{name}"] | {default_val};')
        elif col_type == "float":
            default_val = str(float(default)) if default is not None else "0.0f"
            lines.append(f'    row.{name} = doc["{name}"] | {default_val};')
        else:
            cpp_type = COLUMN_TYPES[col_type][0]
            default_val = str(int(default)) if default is not None else "0"
            lines.append(
                f'    row.{name} = doc["{name}"] | ({cpp_type}) {default_val};'
            )

    lines.append("    return this->upsert(row);")
    body = "\n".join(lines)
    return f"""\
  bool rest_post_body_(const char *json_body) override {{
{body}
  }}"""


def _generate_rest_delete(key_col: dict) -> str:
    key = key_col[CONF_NAME]
    col_type = key_col["type"]
    if col_type == "string":
        parse = f"    return this->remove(key_str);"
    elif col_type in ("uint8", "uint16", "uint32"):
        cpp_type = COLUMN_TYPES[col_type][0]
        parse = (
            f"    {cpp_type} key = static_cast<{cpp_type}>(strtoul(key_str, nullptr, 0));\n"
            f"    return this->remove(key);"
        )
    else:
        cpp_type = COLUMN_TYPES[col_type][0]
        parse = (
            f"    {cpp_type} key = static_cast<{cpp_type}>(strtol(key_str, nullptr, 0));\n"
            f"    return this->remove(key);"
        )
    return f"""\
  bool rest_delete_key_(const char *key_str) override {{
{parse}
  }}"""


def _generate_triggers(struct_name: str, class_name: str) -> str:
    result = ""
    for event, cb in [
        ("RowAdded", "add_on_row_added_callback"),
        ("RowUpdated", "add_on_row_updated_callback"),
        ("RowDeleted", "add_on_row_deleted_callback"),
    ]:
        result += f"""
class {class_name}{event}Trigger : public esphome::Trigger<const {struct_name} &> {{
 public:
  explicit {class_name}{event}Trigger({class_name} *parent) {{
    parent->{cb}([this](const {struct_name} &row) {{ this->trigger(row); }});
  }}
}};"""
    return result


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

async def to_code(config):
    config_id = config[CONF_ID]
    table_id_str = str(config_id)
    nvs_namespace = config[CONF_NAMESPACE]
    max_rows = config[CONF_MAX_ROWS]
    columns = config[CONF_COLUMNS]
    rest_api = config[CONF_REST_API]

    base = _class_base(table_id_str)
    struct_name = f"{base}Row"
    class_name = f"{base}Table"
    key_col = next(c for c in columns if c.get(CONF_KEY, False))
    key_col_type = key_col["type"]
    key_cpp_type = "const char *" if key_col_type == "string" else COLUMN_TYPES[key_col_type][0]

    # ---- Row struct --------------------------------------------------------
    fields = "\n".join(f"  {_col_field_decl(c)};" for c in columns)
    struct_code = f"""
struct {struct_name} {{
{fields}
}};"""

    # ---- REST method bodies (inside class) ---------------------------------
    rest_section = ""
    if rest_api:
        cg.add_define("USE_PERSISTENT_TABLE_REST")
        rest_section = f"""
#ifdef USE_PERSISTENT_TABLE_REST
  // REST: GET /api/table/{table_id_str}
{_generate_rest_get(struct_name, columns)}
  // REST: POST /api/table/{table_id_str}  (body: JSON object)
{_generate_rest_post(struct_name, columns)}
  // REST: DELETE /api/table/{table_id_str}/{{key}}
{_generate_rest_delete(key_col)}
#endif  // USE_PERSISTENT_TABLE_REST"""

    # ---- Full typed table class --------------------------------------------
    class_code = f"""
class {class_name} : public esphome::persistent_table::PersistentTableBase {{
 public:
  {class_name}()
      : PersistentTableBase("{nvs_namespace}", "{table_id_str}", sizeof({struct_name}), {max_rows}) {{}}

  // Typed lookup — returns the row matching key, or empty optional.
{_generate_find(struct_name, key_col)}

  // Insert or update a row. Fires on_row_added / on_row_updated trigger.
{_generate_upsert(struct_name, key_col)}

  // Delete row by key. Fires on_row_deleted trigger.
{_generate_remove(struct_name, key_col)}

  // Call callback(row) for every occupied row (no heap allocation).
{_generate_iterate(struct_name)}

  template<typename F> void add_on_row_added_callback(F &&f) {{
    this->on_row_added_callback_.add(std::forward<F>(f));
  }}
  template<typename F> void add_on_row_updated_callback(F &&f) {{
    this->on_row_updated_callback_.add(std::forward<F>(f));
  }}
  template<typename F> void add_on_row_deleted_callback(F &&f) {{
    this->on_row_deleted_callback_.add(std::forward<F>(f));
  }}
{rest_section}
 private:
  esphome::LazyCallbackManager<void(const {struct_name} &)> on_row_added_callback_;
  esphome::LazyCallbackManager<void(const {struct_name} &)> on_row_updated_callback_;
  esphome::LazyCallbackManager<void(const {struct_name} &)> on_row_deleted_callback_;
}};

// Trigger classes — one per lifecycle event.
{_generate_triggers(struct_name, class_name)}"""

    cg.add_global(cg.RawExpression(struct_code))
    cg.add_global(cg.RawExpression(class_code))

    # Override the declared ID type to the concrete generated class so that
    # id(table_id) in lambdas returns the fully-typed pointer.
    table_type = cg.global_ns.class_(class_name)
    config_id.type = table_type
    var = cg.new_Pvariable(config_id)
    await cg.register_component(var, config)

    # Wire up triggers
    row_ref = cg.RawExpression(f"const {struct_name} &")

    for conf in config.get(CONF_ON_ROW_ADDED, []):
        trigger_type = cg.global_ns.class_(f"{class_name}RowAddedTrigger")
        conf[automation.CONF_TRIGGER_ID].type = trigger_type
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(row_ref, "row")], conf)

    for conf in config.get(CONF_ON_ROW_UPDATED, []):
        trigger_type = cg.global_ns.class_(f"{class_name}RowUpdatedTrigger")
        conf[automation.CONF_TRIGGER_ID].type = trigger_type
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(row_ref, "row")], conf)

    for conf in config.get(CONF_ON_ROW_DELETED, []):
        trigger_type = cg.global_ns.class_(f"{class_name}RowDeletedTrigger")
        conf[automation.CONF_TRIGGER_ID].type = trigger_type
        trigger = cg.new_Pvariable(conf[automation.CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(row_ref, "row")], conf)
