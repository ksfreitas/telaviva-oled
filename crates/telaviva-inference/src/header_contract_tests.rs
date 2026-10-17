//! Header-versus-ABI contract test for `include/telaviva_inference.h`.
//!
//! This module lives inside the crate because the status and provider constants are private to
//! `lib.rs`; they are read here without being made public. Each exported `tv_inference_*`
//! function is pinned by an explicitly typed function-pointer binding, so a signature change in
//! `lib.rs` fails to compile here. The header is read as text and compared with that table, and
//! `lib.rs` is scanned so that an exported function or a new status constant without an entry
//! also fails the test.

use super::*;
use std::collections::{BTreeMap, BTreeSet};

const HEADER: &str = include_str!("../../../include/telaviva_inference.h");
const LIB_SOURCE: &str = include_str!("lib.rs");

/// `i32` constants of `lib.rs` that have no header name. `SUCCESS` (0) is returned by
/// `tv_inference_create` and `tv_inference_assess_light_bgra`, but the header defines no constant
/// for it, so it is allowlisted here instead of being compared.
const RUST_ONLY_ALLOWLIST: &[&str] = &["SUCCESS"];

/// `#[repr(C)]` structs of `lib.rs` that are not compared as C structs. `TvInferenceHandle` is
/// `#[repr(C)]` in `lib.rs` but opaque to C: the header declares it as an incomplete typedef, so
/// there is no field list to compare.
const REPR_C_ALLOWLIST: &[&str] = &["TvInferenceHandle"];

/// One exported C function: its name, its canonical C prototype (parameter names removed) and
/// the address of the Rust function, taken through an explicitly typed binding.
struct Export {
    name: &'static str,
    prototype: &'static str,
    address: usize,
}

/// Every `tv_inference_*` function exported by `lib.rs`. The typed bindings fix each Rust
/// signature at compile time, so a drift in `lib.rs` fails to compile here.
fn exports() -> Vec<Export> {
    let create: unsafe extern "C" fn(
        *const std::ffi::c_char,
        *const std::ffi::c_char,
        *mut *mut TvInferenceHandle,
    ) -> i32 = tv_inference_create;
    let create_with_provider: unsafe extern "C" fn(
        *const std::ffi::c_char,
        *const std::ffi::c_char,
        i32,
        *mut *mut TvInferenceHandle,
    ) -> i32 = tv_inference_create_with_provider;
    let detect: unsafe extern "C" fn(
        *mut TvInferenceHandle,
        *const u8,
        usize,
        u32,
        u32,
        usize,
        *mut TvInferenceResult,
    ) -> i32 = tv_inference_detect_bgra;
    let assess: unsafe extern "C" fn(*const u8, usize, u32, u32, usize) -> i32 =
        tv_inference_assess_light_bgra;
    let coreml_available: unsafe extern "C" fn() -> i32 = tv_inference_coreml_available;
    let selected_provider: unsafe extern "C" fn(*const TvInferenceHandle) -> i32 =
        tv_inference_selected_provider;
    let destroy: unsafe extern "C" fn(*mut TvInferenceHandle) = tv_inference_destroy;
    vec![
        Export {
            name: "tv_inference_create",
            prototype: "int32_t tv_inference_create(const char *, const char *, TvInferenceHandle **)",
            address: create as usize,
        },
        Export {
            name: "tv_inference_create_with_provider",
            prototype: "int32_t tv_inference_create_with_provider(const char *, const char *, int32_t, TvInferenceHandle **)",
            address: create_with_provider as usize,
        },
        Export {
            name: "tv_inference_detect_bgra",
            prototype: "int32_t tv_inference_detect_bgra(TvInferenceHandle *, const uint8_t *, size_t, uint32_t, uint32_t, size_t, TvInferenceResult *)",
            address: detect as usize,
        },
        Export {
            name: "tv_inference_assess_light_bgra",
            prototype: "int32_t tv_inference_assess_light_bgra(const uint8_t *, size_t, uint32_t, uint32_t, size_t)",
            address: assess as usize,
        },
        Export {
            name: "tv_inference_coreml_available",
            prototype: "int32_t tv_inference_coreml_available(void)",
            address: coreml_available as usize,
        },
        Export {
            name: "tv_inference_selected_provider",
            prototype: "int32_t tv_inference_selected_provider(const TvInferenceHandle *)",
            address: selected_provider as usize,
        },
        Export {
            name: "tv_inference_destroy",
            prototype: "void tv_inference_destroy(TvInferenceHandle *)",
            address: destroy as usize,
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

/// Every constant the header defines in its anonymous enums, with its Rust counterpart. The
/// header names of the error codes differ from the private Rust names, so each pair is explicit.
fn constants() -> Vec<Constant> {
    vec![
        Constant {
            header: "TV_INFERENCE_ABSENT",
            rust_name: "ABSENT",
            value: 0,
            rust: ABSENT as i64,
        },
        Constant {
            header: "TV_INFERENCE_PRESENT",
            rust_name: "PRESENT",
            value: 1,
            rust: PRESENT as i64,
        },
        Constant {
            header: "TV_INFERENCE_INSUFFICIENT_LIGHT",
            rust_name: "INSUFFICIENT_LIGHT",
            value: 2,
            rust: INSUFFICIENT_LIGHT as i64,
        },
        Constant {
            header: "TV_INFERENCE_UNAVAILABLE",
            rust_name: "UNAVAILABLE",
            value: 3,
            rust: UNAVAILABLE as i64,
        },
        Constant {
            header: "TV_INFERENCE_ERROR_INVALID_ARGUMENT",
            rust_name: "INVALID_ARGUMENT",
            value: -1,
            rust: INVALID_ARGUMENT as i64,
        },
        Constant {
            header: "TV_INFERENCE_ERROR_RUNTIME",
            rust_name: "RUNTIME_ERROR",
            value: -2,
            rust: RUNTIME_ERROR as i64,
        },
        Constant {
            header: "TV_INFERENCE_ERROR_MODEL",
            rust_name: "MODEL_ERROR",
            value: -3,
            rust: MODEL_ERROR as i64,
        },
        Constant {
            header: "TV_INFERENCE_ERROR_PANIC",
            rust_name: "PANIC_ERROR",
            value: -4,
            rust: PANIC_ERROR as i64,
        },
        Constant {
            header: "TV_INFERENCE_PROVIDER_CPU",
            rust_name: "PROVIDER_CPU",
            value: 0,
            rust: PROVIDER_CPU as i64,
        },
        Constant {
            header: "TV_INFERENCE_PROVIDER_COREML_CANDIDATE",
            rust_name: "PROVIDER_COREML_CANDIDATE",
            value: 1,
            rust: PROVIDER_COREML_CANDIDATE as i64,
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
/// `["TvInferenceHandle", "*", "*"]` becomes `TvInferenceHandle **`.
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

/// Names of the structs that carry `#[repr(C)]` in a Rust source file.
fn repr_c_struct_names(source: &str) -> BTreeSet<String> {
    source
        .match_indices("#[repr(C)]")
        .filter_map(|(index, attribute)| {
            let after_attribute = &source[index + attribute.len()..];
            let (_, after_keyword) = after_attribute.split_once("struct ")?;
            Some(identifier_at(after_keyword).to_string())
        })
        .collect()
}

/// Names of the module-level `i32` constants in a Rust source file.
fn i32_constant_names(source: &str) -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    for line in source.lines() {
        let declaration = line.trim_start();
        let declaration = declaration.strip_prefix("pub ").unwrap_or(declaration);
        let Some(rest) = declaration.strip_prefix("const ") else {
            continue;
        };
        let Some((name, ty)) = rest.split_once(':') else {
            continue;
        };
        if ty.trim_start().starts_with("i32") {
            names.insert(name.trim().to_string());
        }
    }
    names
}

#[test]
fn header_prototypes_match_every_rust_export() {
    let header = parse_header(HEADER);
    let table = exports();
    for export in &table {
        assert_ne!(export.address, 0, "{} has no address", export.name);
        let declared = header.functions.get(export.name).unwrap_or_else(|| {
            panic!(
                "{} is exported by lib.rs but not declared in include/telaviva_inference.h",
                export.name
            )
        });
        assert_eq!(
            declared.as_str(),
            export.prototype,
            "{} differs between include/telaviva_inference.h and the Rust export",
            export.name
        );
    }
    for name in header.functions.keys() {
        assert!(
            table.iter().any(|export| export.name == name.as_str()),
            "{name} is declared in include/telaviva_inference.h but missing from the export table"
        );
    }
}

#[test]
fn header_result_fields_match_rust_struct() {
    let header = parse_header(HEADER);
    let fields = header
        .structs
        .get("TvInferenceResult")
        .expect("include/telaviva_inference.h must define struct TvInferenceResult");
    let rust_fields: Vec<(&str, &str)> = fields
        .iter()
        .map(|(c_type, name)| (rust_type(c_type), name.as_str()))
        .collect();
    assert_eq!(
        rust_fields,
        [("i32", "status"), ("f32", "confidence")],
        "TvInferenceResult fields differ between include/telaviva_inference.h and lib.rs"
    );
    // Pins the Rust field names and types, so a change in lib.rs fails to compile here.
    let _: fn(i32, f32) -> TvInferenceResult =
        |status, confidence| TvInferenceResult { status, confidence };
}

#[test]
fn header_constants_match_rust_status_codes() {
    let header = parse_header(HEADER);
    let table = constants();
    for constant in &table {
        let declared = header.constants.get(constant.header).unwrap_or_else(|| {
            panic!(
                "{} is missing from include/telaviva_inference.h",
                constant.header
            )
        });
        assert_eq!(
            *declared, constant.value,
            "{} in include/telaviva_inference.h differs from its expected value",
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
            "{name} is defined in include/telaviva_inference.h but missing from the constant table"
        );
    }
}

#[test]
fn header_declares_handle_as_opaque_type() {
    let header = parse_header(HEADER);
    assert!(
        header.opaque.contains("TvInferenceHandle"),
        "include/telaviva_inference.h must declare TvInferenceHandle as an incomplete typedef"
    );
    assert!(
        !header.structs.contains_key("TvInferenceHandle"),
        "include/telaviva_inference.h must not give TvInferenceHandle a field list"
    );
}

#[test]
fn rust_repr_c_structs_are_checked_or_allowlisted() {
    let header = parse_header(HEADER);
    for name in repr_c_struct_names(LIB_SOURCE) {
        let checked = header.structs.contains_key(name.as_str());
        let allowlisted = REPR_C_ALLOWLIST.contains(&name.as_str());
        assert!(
            checked || allowlisted,
            "{name} is #[repr(C)] in lib.rs but is neither a struct in the header nor allowlisted"
        );
    }
}

#[test]
fn rust_status_constants_are_tabled_or_allowlisted() {
    let table: BTreeSet<&str> = constants()
        .iter()
        .map(|constant| constant.rust_name)
        .collect();
    for name in i32_constant_names(LIB_SOURCE) {
        let tabled = table.contains(name.as_str());
        let allowed = RUST_ONLY_ALLOWLIST.contains(&name.as_str());
        assert!(
            tabled || allowed,
            "{name} is an i32 constant in lib.rs with no constant-table or allowlist entry"
        );
    }
}

#[test]
fn rust_source_exports_exactly_the_tabled_functions() {
    let table = exports();
    let table_names: BTreeSet<&str> = table.iter().map(|export| export.name).collect();
    let source_names = exported_names(LIB_SOURCE);
    for name in &source_names {
        assert!(
            table_names.contains(name.as_str()),
            "{name} is exported by lib.rs but missing from the export table in header_contract_tests.rs"
        );
    }
    for name in &table_names {
        assert!(
            source_names.contains(*name),
            "{name} is in the export table but lib.rs no longer exports it"
        );
    }
}

#[test]
fn canonicalizer_drops_parameter_names_and_keeps_void_and_pointers() {
    let (name, prototype) =
        canonical_prototype("void tv_inference_destroy(TvInferenceHandle *handle)");
    assert_eq!(name, "tv_inference_destroy");
    assert_eq!(prototype, "void tv_inference_destroy(TvInferenceHandle *)");
    let (_, prototype) = canonical_prototype("int32_t tv_inference_coreml_available(void)");
    assert_eq!(prototype, "int32_t tv_inference_coreml_available(void)");
    let (_, prototype) = canonical_prototype(
        "int32_t tv_inference_create(const char *runtime, TvInferenceHandle **out)",
    );
    assert_eq!(
        prototype,
        "int32_t tv_inference_create(const char *, TvInferenceHandle **)"
    );
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
