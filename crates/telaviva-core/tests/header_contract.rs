//! Header-versus-ABI contract test for `include/telaviva_core.h`.
//!
//! Each exported `tv_*` function is pinned by an explicitly typed function-pointer binding, so a
//! signature change in `ffi.rs` fails to compile here. The header is read as text and compared
//! with that table, and `ffi.rs` is scanned so that an exported function without a table entry
//! also fails the test. Only the public `telaviva_core::ffi` surface is used.

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::c_void;
use telaviva_core::ffi::{self, TvBuffer, TvCameraLifecycle, TvCore, TvSnapshotReady};

const HEADER: &str = include_str!("../../../include/telaviva_core.h");
const FFI_SOURCE: &str = include_str!("../src/ffi.rs");

/// One exported C function: its name, its canonical C prototype (parameter names removed) and
/// the address of the Rust function, taken through an explicitly typed binding.
struct Export {
    name: &'static str,
    prototype: &'static str,
    address: usize,
}

/// Every `tv_*` function exported by `ffi.rs`. The typed bindings fix each Rust signature at
/// compile time, so a drift in `ffi.rs` fails to compile here.
fn exports() -> Vec<Export> {
    let abi_version: extern "C" fn() -> u32 = ffi::tv_abi_version;
    let create: unsafe extern "C" fn(
        u32,
        *const u8,
        usize,
        TvSnapshotReady,
        *mut c_void,
        *mut *mut TvCore,
    ) -> i32 = ffi::tv_create;
    let submit: unsafe extern "C" fn(*mut TvCore, *const u8, usize) -> i32 = ffi::tv_submit;
    let snapshot: unsafe extern "C" fn(*mut TvCore, *mut TvBuffer) -> i32 = ffi::tv_snapshot;
    let shutdown: unsafe extern "C" fn(*mut TvCore) -> i32 = ffi::tv_shutdown;
    let destroy: unsafe extern "C" fn(*mut TvCore) = ffi::tv_destroy;
    let buffer_free: unsafe extern "C" fn(*mut TvBuffer) = ffi::tv_buffer_free;
    let camera_create: unsafe extern "C" fn(*mut *mut TvCameraLifecycle) -> i32 =
        ffi::tv_camera_lifecycle_create;
    let camera_step: unsafe extern "C" fn(
        *mut TvCameraLifecycle,
        *const u8,
        usize,
        *mut TvBuffer,
    ) -> i32 = ffi::tv_camera_lifecycle_step;
    let camera_destroy: unsafe extern "C" fn(*mut TvCameraLifecycle) =
        ffi::tv_camera_lifecycle_destroy;
    let core_version: unsafe extern "C" fn(*mut TvBuffer) -> i32 = ffi::tv_core_version;
    let update_evaluate: unsafe extern "C" fn(
        *const u8,
        usize,
        u32,
        *const u8,
        usize,
        *mut TvBuffer,
    ) -> i32 = ffi::tv_update_evaluate;
    vec![
        Export {
            name: "tv_abi_version",
            prototype: "uint32_t tv_abi_version(void)",
            address: abi_version as usize,
        },
        Export {
            name: "tv_create",
            prototype: "int32_t tv_create(uint32_t, const uint8_t *, size_t, TvSnapshotReady, void *, TvCore **)",
            address: create as usize,
        },
        Export {
            name: "tv_submit",
            prototype: "int32_t tv_submit(TvCore *, const uint8_t *, size_t)",
            address: submit as usize,
        },
        Export {
            name: "tv_snapshot",
            prototype: "int32_t tv_snapshot(TvCore *, TvBuffer *)",
            address: snapshot as usize,
        },
        Export {
            name: "tv_shutdown",
            prototype: "int32_t tv_shutdown(TvCore *)",
            address: shutdown as usize,
        },
        Export {
            name: "tv_destroy",
            prototype: "void tv_destroy(TvCore *)",
            address: destroy as usize,
        },
        Export {
            name: "tv_buffer_free",
            prototype: "void tv_buffer_free(TvBuffer *)",
            address: buffer_free as usize,
        },
        Export {
            name: "tv_camera_lifecycle_create",
            prototype: "int32_t tv_camera_lifecycle_create(TvCameraLifecycle **)",
            address: camera_create as usize,
        },
        Export {
            name: "tv_camera_lifecycle_step",
            prototype: "int32_t tv_camera_lifecycle_step(TvCameraLifecycle *, const uint8_t *, size_t, TvBuffer *)",
            address: camera_step as usize,
        },
        Export {
            name: "tv_camera_lifecycle_destroy",
            prototype: "void tv_camera_lifecycle_destroy(TvCameraLifecycle *)",
            address: camera_destroy as usize,
        },
        Export {
            name: "tv_core_version",
            prototype: "int32_t tv_core_version(TvBuffer *)",
            address: core_version as usize,
        },
        Export {
            name: "tv_update_evaluate",
            prototype: "int32_t tv_update_evaluate(const uint8_t *, size_t, uint32_t, const uint8_t *, size_t, TvBuffer *)",
            address: update_evaluate as usize,
        },
    ]
}

/// One header constant, its expected value and the Rust constant that must equal it.
struct Constant {
    header: &'static str,
    rust_name: &'static str,
    value: i64,
    rust: i64,
}

/// Every constant the header defines (`#define` or anonymous enum), with its Rust counterpart.
fn constants() -> Vec<Constant> {
    vec![
        Constant {
            header: "TV_CORE_ABI_VERSION",
            rust_name: "ABI_VERSION",
            value: 1,
            rust: ffi::ABI_VERSION as i64,
        },
        Constant {
            header: "TV_OK",
            rust_name: "TV_OK",
            value: 0,
            rust: ffi::TV_OK as i64,
        },
        Constant {
            header: "TV_INVALID_ARGUMENT",
            rust_name: "TV_INVALID_ARGUMENT",
            value: 1,
            rust: ffi::TV_INVALID_ARGUMENT as i64,
        },
        Constant {
            header: "TV_ABI_MISMATCH",
            rust_name: "TV_ABI_MISMATCH",
            value: 2,
            rust: ffi::TV_ABI_MISMATCH as i64,
        },
        Constant {
            header: "TV_CLOSED",
            rust_name: "TV_CLOSED",
            value: 3,
            rust: ffi::TV_CLOSED as i64,
        },
        Constant {
            header: "TV_INTERNAL",
            rust_name: "TV_INTERNAL",
            value: 4,
            rust: ffi::TV_INTERNAL as i64,
        },
        Constant {
            header: "TV_SIZE_LIMIT",
            rust_name: "TV_SIZE_LIMIT",
            value: 5,
            rust: ffi::TV_SIZE_LIMIT as i64,
        },
    ]
}

/// What the test reads from a header: function prototypes by name, constant values, struct
/// fields (C type and field name, in declaration order) and typedef names that are incomplete.
#[derive(Default)]
struct Header {
    functions: BTreeMap<String, String>,
    constants: BTreeMap<String, i64>,
    structs: BTreeMap<String, Vec<(String, String)>>,
    opaque: BTreeSet<String>,
}

/// Parses header text. Block and line comments and the `extern "C"` C++ guard are removed first,
/// then `#define` integers are recorded and the remaining code is split into top-level statements.
fn parse_header(text: &str) -> Header {
    let mut header = Header::default();
    let mut code = String::new();
    let mut in_cplusplus_guard = false;
    for raw_line in strip_block_comments(text).lines() {
        let before_comment = match raw_line.find("//") {
            Some(index) => &raw_line[..index],
            None => raw_line,
        };
        let line = before_comment.trim();
        if in_cplusplus_guard {
            in_cplusplus_guard = !line.starts_with("#endif");
        } else if line.starts_with("#ifdef __cplusplus") {
            in_cplusplus_guard = true;
        } else if let Some(directive) = line.strip_prefix("#define ") {
            record_define(directive, &mut header.constants);
        } else if !line.starts_with('#') {
            code.push_str(line);
            code.push('\n');
        }
    }
    for statement in split_statements(&code) {
        if let Some(declaration) = statement.strip_prefix("typedef ") {
            parse_typedef(declaration, &mut header);
        } else if let Some(body) = statement.strip_prefix("enum ") {
            parse_enum(body, &mut header.constants);
        } else if statement.contains('(') {
            let (name, prototype) = canonical_prototype(&statement);
            header.functions.insert(name, prototype);
        }
    }
    header
}

fn strip_block_comments(text: &str) -> String {
    let mut stripped = String::with_capacity(text.len());
    let mut rest = text;
    while let Some(start) = rest.find("/*") {
        stripped.push_str(&rest[..start]);
        rest = match rest[start..].find("*/") {
            Some(end) => &rest[start + end + 2..],
            None => "",
        };
    }
    stripped.push_str(rest);
    stripped
}

/// Records `NAME value` from a `#define` body when the value is an integer, ignoring a `u` suffix.
fn record_define(directive: &str, constants: &mut BTreeMap<String, i64>) {
    let parts: Vec<&str> = directive.split_whitespace().collect();
    let [name, value] = parts.as_slice() else {
        return;
    };
    let Ok(number) = value.trim_end_matches(['u', 'U']).parse::<i64>() else {
        return;
    };
    constants.insert(name.to_string(), number);
}

/// Splits code on `;` outside braces and normalizes the whitespace of each statement.
fn split_statements(code: &str) -> Vec<String> {
    let mut statements = Vec::new();
    let mut current = String::new();
    let mut depth = 0_i32;
    for ch in code.chars() {
        match ch {
            '{' => depth += 1,
            '}' => depth -= 1,
            ';' if depth == 0 => {
                statements.push(std::mem::take(&mut current));
                continue;
            }
            _ => {}
        }
        current.push(ch);
    }
    statements.push(current);
    statements
        .iter()
        .map(|statement| statement.split_whitespace().collect::<Vec<_>>().join(" "))
        .filter(|statement| !statement.is_empty())
        .collect()
}

/// Handles `struct Tag { fields } Alias` (recorded under the alias) and the incomplete form
/// `struct Tag Tag`. Other typedefs, such as the callback typedef, are ignored.
fn parse_typedef(declaration: &str, header: &mut Header) {
    let Some(definition) = declaration.strip_prefix("struct ") else {
        return;
    };
    match definition.split_once('{') {
        Some((_, after_brace)) => {
            let (body, alias) = after_brace
                .rsplit_once('}')
                .expect("a struct typedef closes its body");
            let fields: Vec<(String, String)> = body
                .split(';')
                .map(str::trim)
                .filter(|field| !field.is_empty())
                .map(split_field)
                .collect();
            header.structs.insert(alias.trim().to_string(), fields);
        }
        None => {
            let words: Vec<&str> = definition.split_whitespace().collect();
            if words.len() == 2 && words[0] == words[1] {
                header.opaque.insert(words[1].to_string());
            }
        }
    }
}

/// Parses `NAME = value` entries of an anonymous enum body.
fn parse_enum(body: &str, constants: &mut BTreeMap<String, i64>) {
    let inner = body.trim().trim_start_matches('{').trim_end_matches('}');
    for entry in inner
        .split(',')
        .map(str::trim)
        .filter(|entry| !entry.is_empty())
    {
        let (name, value) = entry
            .split_once('=')
            .unwrap_or_else(|| panic!("enum entry without a value: {entry}"));
        let number: i64 = value
            .trim()
            .parse()
            .unwrap_or_else(|_| panic!("enum entry is not an integer: {entry}"));
        constants.insert(name.trim().to_string(), number);
    }
}

/// Splits a struct field into its canonical C type and its name (the last token).
fn split_field(declaration: &str) -> (String, String) {
    let mut tokens = tokenize(declaration);
    let name = tokens.pop().expect("a struct field has a name");
    (render_type(&tokens), name)
}

/// Canonical form of a function declaration: `ret name(param, ...)` with parameter names
/// removed and each `*` attached to its type. Returns the name and the canonical prototype.
fn canonical_prototype(declaration: &str) -> (String, String) {
    let open = declaration
        .find('(')
        .expect("a function declaration has a parameter list");
    let close = declaration
        .rfind(')')
        .expect("a function declaration closes its parameter list");
    let head = tokenize(&declaration[..open]);
    let (name, return_tokens) = head
        .split_last()
        .expect("a function declaration has a name");
    let return_type = render_type(return_tokens);
    let params: Vec<String> = split_top_level_commas(&declaration[open + 1..close])
        .into_iter()
        .filter(|param| !param.trim().is_empty())
        .map(canonical_parameter)
        .collect();
    let params = params.join(", ");
    let prototype = format!("{return_type} {name}({params})");
    (name.clone(), prototype)
}

/// Removes the trailing parameter name when the parameter has at least two non-`*` tokens, so
/// `const uint8_t *json` becomes `const uint8_t *` and a lone `void` stays `void`.
fn canonical_parameter(parameter: &str) -> String {
    let mut tokens = tokenize(parameter);
    let typed_parts = tokens.iter().filter(|token| token.as_str() != "*").count();
    let trailing_name = tokens.iter().rposition(|token| token.as_str() != "*");
    if let Some(index) = trailing_name.filter(|_| typed_parts >= 2) {
        tokens.remove(index);
    }
    render_type(&tokens)
}

/// Splits a parameter list on commas that are not nested inside parentheses.
fn split_top_level_commas(text: &str) -> Vec<&str> {
    let mut parts = Vec::new();
    let mut depth = 0_i32;
    let mut start = 0;
    for (index, ch) in text.char_indices() {
        match ch {
            '(' => depth += 1,
            ')' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(&text[start..index]);
                start = index + 1;
            }
            _ => {}
        }
    }
    parts.push(&text[start..]);
    parts
}

/// Splits text into identifier tokens and `*` tokens; all other characters separate tokens.
fn tokenize(text: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut word = String::new();
    for ch in text.chars() {
        if ch.is_ascii_alphanumeric() || ch == '_' {
            word.push(ch);
            continue;
        }
        if !word.is_empty() {
            tokens.push(std::mem::take(&mut word));
        }
        if ch == '*' {
            tokens.push("*".to_string());
        }
    }
    if !word.is_empty() {
        tokens.push(word);
    }
    tokens
}

/// Joins type tokens with single spaces and attaches each `*` to the type before it:
/// `["TvCore", "*", "*"]` becomes `TvCore **`.
fn render_type(tokens: &[String]) -> String {
    let mut rendered = String::new();
    for token in tokens {
        if token.as_str() == "*" {
            if !rendered.ends_with('*') {
                rendered.push(' ');
            }
            rendered.push('*');
        } else {
            if !rendered.is_empty() {
                rendered.push(' ');
            }
            rendered.push_str(token);
        }
    }
    rendered
}

/// The fixed map from the header's C types to the Rust types used by the pinned structs.
fn rust_type(c_type: &str) -> &'static str {
    match c_type {
        "uint8_t *" => "*mut u8",
        "size_t" => "usize",
        "int32_t" => "i32",
        "float" => "f32",
        "uint32_t" => "u32",
        "uint64_t" => "u64",
        other => panic!("no Rust type is mapped for the C type `{other}`"),
    }
}

/// Returns the identifier at the start of `text` (empty when `text` does not start with one).
fn identifier_at(text: &str) -> &str {
    let end = text
        .find(|ch: char| !(ch.is_ascii_alphanumeric() || ch == '_'))
        .unwrap_or(text.len());
    &text[..end]
}

/// Names of the `extern "C" fn tv_*` functions defined in a Rust source file.
fn exported_names(source: &str) -> BTreeSet<String> {
    source
        .match_indices("extern \"C\" fn ")
        .map(|(index, keyword)| identifier_at(&source[index + keyword.len()..]))
        .filter(|name| name.starts_with("tv_"))
        .map(str::to_string)
        .collect()
}

#[test]
fn header_prototypes_match_every_rust_export() {
    let header = parse_header(HEADER);
    let table = exports();
    for export in &table {
        assert_ne!(export.address, 0, "{} has no address", export.name);
        let declared = header.functions.get(export.name).unwrap_or_else(|| {
            panic!(
                "{} is exported by ffi.rs but not declared in include/telaviva_core.h",
                export.name
            )
        });
        assert_eq!(
            declared.as_str(),
            export.prototype,
            "{} differs between include/telaviva_core.h and the Rust export",
            export.name
        );
    }
    for name in header.functions.keys() {
        assert!(
            table.iter().any(|export| export.name == name.as_str()),
            "{name} is declared in include/telaviva_core.h but missing from the export table"
        );
    }
}

#[test]
fn header_buffer_fields_match_rust_struct() {
    let header = parse_header(HEADER);
    let fields = header
        .structs
        .get("TvBuffer")
        .expect("include/telaviva_core.h must define struct TvBuffer");
    let rust_fields: Vec<(&str, &str)> = fields
        .iter()
        .map(|(c_type, name)| (rust_type(c_type), name.as_str()))
        .collect();
    assert_eq!(
        rust_fields,
        [("*mut u8", "data"), ("usize", "len")],
        "TvBuffer fields differ between include/telaviva_core.h and ffi::TvBuffer"
    );
    // Pins the Rust field names and types, so a change in ffi.rs fails to compile here.
    let _: fn(*mut u8, usize) -> TvBuffer = |data, len| TvBuffer { data, len };
}

#[test]
fn header_constants_match_rust_status_codes() {
    let header = parse_header(HEADER);
    let table = constants();
    for constant in &table {
        let declared = header.constants.get(constant.header).unwrap_or_else(|| {
            panic!(
                "{} is missing from include/telaviva_core.h",
                constant.header
            )
        });
        assert_eq!(
            *declared, constant.value,
            "{} in include/telaviva_core.h differs from its expected value",
            constant.header
        );
        assert_eq!(
            constant.rust, constant.value,
            "Rust {} differs from {}",
            constant.rust_name, constant.header
        );
    }
    let table_names: BTreeSet<&str> = table.iter().map(|constant| constant.header).collect();
    for name in header.constants.keys() {
        assert!(
            table_names.contains(name.as_str()),
            "{name} is defined in include/telaviva_core.h but missing from the constant table"
        );
    }
}

#[test]
fn header_declares_opaque_handles_as_incomplete_types() {
    let header = parse_header(HEADER);
    for handle in ["TvCore", "TvCameraLifecycle"] {
        assert!(
            header.opaque.contains(handle),
            "include/telaviva_core.h must declare {handle} as an incomplete typedef"
        );
        assert!(
            !header.structs.contains_key(handle),
            "include/telaviva_core.h must not give {handle} a field list"
        );
    }
}

#[test]
fn rust_source_exports_exactly_the_tabled_functions() {
    let table = exports();
    let table_names: BTreeSet<&str> = table.iter().map(|export| export.name).collect();
    let source_names = exported_names(FFI_SOURCE);
    for name in &source_names {
        assert!(
            table_names.contains(name.as_str()),
            "{name} is exported by ffi.rs but missing from the export table in header_contract.rs"
        );
    }
    for name in &table_names {
        assert!(
            source_names.contains(*name),
            "{name} is in the export table but ffi.rs no longer exports it"
        );
    }
}

#[test]
fn canonicalizer_drops_parameter_names_and_keeps_void_and_pointers() {
    let (name, prototype) = canonical_prototype(
        "int32_t tv_submit(TvCore *core, const uint8_t *json, size_t json_len)",
    );
    assert_eq!(name, "tv_submit");
    assert_eq!(
        prototype,
        "int32_t tv_submit(TvCore *, const uint8_t *, size_t)"
    );
    let (_, prototype) = canonical_prototype("uint32_t tv_abi_version(void)");
    assert_eq!(prototype, "uint32_t tv_abi_version(void)");
    let (_, prototype) =
        canonical_prototype("int32_t tv_create(uint32_t abi_version, TvCore **out)");
    assert_eq!(prototype, "int32_t tv_create(uint32_t, TvCore **)");
}

#[test]
fn parser_ignores_comments_and_splits_only_top_level_statements() {
    let header = parse_header(
        "/* a; b */\n#define TV_X 2u\ntypedef struct TvY { int32_t a; float b; } TvY;\nint32_t tv_z(void); // tail;\n",
    );
    assert_eq!(header.constants.get("TV_X"), Some(&2));
    let fields: Vec<(&str, &str)> = header.structs["TvY"]
        .iter()
        .map(|(c_type, name)| (c_type.as_str(), name.as_str()))
        .collect();
    assert_eq!(fields, [("int32_t", "a"), ("float", "b")]);
    assert_eq!(
        header.functions.get("tv_z").map(String::as_str),
        Some("int32_t tv_z(void)")
    );
}
