use serde::de::{self, MapAccess, SeqAccess, Visitor};
use serde::ser::SerializeMap;
use serde::{Deserialize, Deserializer, Serialize, Serializer};
use serde_json::Number;
use std::env;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process;

const BLOCK: usize = 4 * 1024 * 1024;
const USAGE: &str =
    "Usage:\n  steamarm-heroic-asar retarget IN.asar OUT.asar ARM64-BIN-DIR\n  steamarm-heroic-asar list IN.asar [PREFIX]";

#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Number(Number),
    String(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

impl Json {
    fn object(entries: Vec<(String, Json)>) -> Self {
        Self::Object(entries)
    }

    fn get(&self, name: &str) -> Option<&Json> {
        self.as_object()?.iter().find(|(key, _)| key == name).map(|(_, value)| value)
    }

    fn get_mut(&mut self, name: &str) -> Option<&mut Json> {
        self.as_object_mut()?.iter_mut().find(|(key, _)| key == name).map(|(_, value)| value)
    }

    fn as_object(&self) -> Option<&Vec<(String, Json)>> {
        match self {
            Self::Object(entries) => Some(entries),
            _ => None,
        }
    }

    fn as_object_mut(&mut self) -> Option<&mut Vec<(String, Json)>> {
        match self {
            Self::Object(entries) => Some(entries),
            _ => None,
        }
    }

    fn remove(&mut self, name: &str) -> Option<Json> {
        let entries = self.as_object_mut()?;
        let index = entries.iter().position(|(key, _)| key == name)?;
        Some(entries.remove(index).1)
    }

    fn insert(&mut self, name: impl Into<String>, value: Json) {
        let name = name.into();
        if let Some(entries) = self.as_object_mut() {
            if let Some((_, current)) = entries.iter_mut().find(|(key, _)| key == &name) {
                *current = value;
            } else {
                entries.push((name, value));
            }
        }
    }

    fn truthy(&self) -> bool {
        match self {
            Self::Null => false,
            Self::Bool(value) => *value,
            Self::Number(value) => value.as_f64().is_some_and(|number| number != 0.0),
            Self::String(value) => !value.is_empty(),
            Self::Array(value) => !value.is_empty(),
            Self::Object(value) => !value.is_empty(),
        }
    }

    fn integer(&self) -> Option<i64> {
        match self {
            Self::Number(number) => number.as_i64().or_else(|| number.as_u64().and_then(|n| i64::try_from(n).ok())),
            _ => None,
        }
    }
}

impl Serialize for Json {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            Self::Null => serializer.serialize_unit(),
            Self::Bool(value) => serializer.serialize_bool(*value),
            Self::Number(value) => value.serialize(serializer),
            Self::String(value) => serializer.serialize_str(value),
            Self::Array(values) => {
                use serde::ser::SerializeSeq;
                let mut sequence = serializer.serialize_seq(Some(values.len()))?;
                for value in values {
                    sequence.serialize_element(value)?;
                }
                sequence.end()
            }
            Self::Object(entries) => {
                let mut map = serializer.serialize_map(Some(entries.len()))?;
                for (key, value) in entries {
                    map.serialize_entry(key, value)?;
                }
                map.end()
            }
        }
    }
}

impl<'de> Deserialize<'de> for Json {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;

        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = Json;

            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("a JSON value")
            }

            fn visit_unit<E>(self) -> Result<Json, E> {
                Ok(Json::Null)
            }

            fn visit_none<E>(self) -> Result<Json, E> {
                Ok(Json::Null)
            }

            fn visit_bool<E>(self, value: bool) -> Result<Json, E> {
                Ok(Json::Bool(value))
            }

            fn visit_i64<E>(self, value: i64) -> Result<Json, E> {
                Ok(Json::Number(Number::from(value)))
            }

            fn visit_u64<E>(self, value: u64) -> Result<Json, E> {
                Ok(Json::Number(Number::from(value)))
            }

            fn visit_f64<E>(self, value: f64) -> Result<Json, E>
            where
                E: de::Error,
            {
                Number::from_f64(value)
                    .map(Json::Number)
                    .ok_or_else(|| E::custom("non-finite JSON number"))
            }

            fn visit_str<E>(self, value: &str) -> Result<Json, E> {
                Ok(Json::String(value.to_owned()))
            }

            fn visit_string<E>(self, value: String) -> Result<Json, E> {
                Ok(Json::String(value))
            }

            fn visit_seq<A>(self, mut sequence: A) -> Result<Json, A::Error>
            where
                A: SeqAccess<'de>,
            {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element()? {
                    values.push(value);
                }
                Ok(Json::Array(values))
            }

            fn visit_map<A>(self, mut map: A) -> Result<Json, A::Error>
            where
                A: MapAccess<'de>,
            {
                let mut entries: Vec<(String, Json)> = Vec::new();
                while let Some((key, value)) = map.next_entry::<String, Json>()? {
                    if let Some((_, current)) = entries.iter_mut().find(|(existing, _)| existing == &key) {
                        *current = value;
                    } else {
                        entries.push((key, value));
                    }
                }
                Ok(Json::Object(entries))
            }
        }

        deserializer.deserialize_any(JsonVisitor)
    }
}

struct Sha256 {
    state: [u32; 8],
    block: [u8; 64],
    used: usize,
    bytes: u64,
}

impl Sha256 {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];

    fn new() -> Self {
        Self {
            state: [
                0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
                0x1f83d9ab, 0x5be0cd19,
            ],
            block: [0; 64],
            used: 0,
            bytes: 0,
        }
    }

    fn update(&mut self, mut input: &[u8]) {
        self.bytes = self.bytes.wrapping_add(input.len() as u64);
        if self.used > 0 {
            let take = (64 - self.used).min(input.len());
            self.block[self.used..self.used + take].copy_from_slice(&input[..take]);
            self.used += take;
            input = &input[take..];
            if self.used == 64 {
                let block = self.block;
                self.compress(&block);
                self.used = 0;
            }
        }
        while input.len() >= 64 {
            let block: &[u8; 64] = input[..64].try_into().expect("fixed-size SHA-256 block");
            self.compress(block);
            input = &input[64..];
        }
        if !input.is_empty() {
            self.block[..input.len()].copy_from_slice(input);
            self.used = input.len();
        }
    }

    fn compress(&mut self, block: &[u8; 64]) {
        let mut schedule = [0u32; 64];
        for (index, word) in schedule[..16].iter_mut().enumerate() {
            let offset = index * 4;
            *word = u32::from_be_bytes(block[offset..offset + 4].try_into().expect("four bytes"));
        }
        for index in 16..64 {
            let x = schedule[index - 15];
            let y = schedule[index - 2];
            let s0 = x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3);
            let s1 = y.rotate_right(17) ^ y.rotate_right(19) ^ (y >> 10);
            schedule[index] = schedule[index - 16]
                .wrapping_add(s0)
                .wrapping_add(schedule[index - 7])
                .wrapping_add(s1);
        }

        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.state;
        for index in 0..64 {
            let sum1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let choice = (e & f) ^ (!e & g);
            let t1 = h
                .wrapping_add(sum1)
                .wrapping_add(choice)
                .wrapping_add(Self::K[index])
                .wrapping_add(schedule[index]);
            let sum0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let majority = (a & b) ^ (a & c) ^ (b & c);
            let t2 = sum0.wrapping_add(majority);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (state, value) in self.state.iter_mut().zip([a, b, c, d, e, f, g, h]) {
            *state = state.wrapping_add(value);
        }
    }

    fn finish(mut self) -> [u8; 32] {
        let bit_len = self.bytes.wrapping_mul(8);
        self.block[self.used] = 0x80;
        self.used += 1;
        if self.used > 56 {
            self.block[self.used..].fill(0);
            let block = self.block;
            self.compress(&block);
            self.block = [0; 64];
        } else {
            self.block[self.used..56].fill(0);
        }
        self.block[56..64].copy_from_slice(&bit_len.to_be_bytes());
        let block = self.block;
        self.compress(&block);

        let mut digest = [0u8; 32];
        for (index, word) in self.state.iter().enumerate() {
            digest[index * 4..index * 4 + 4].copy_from_slice(&word.to_be_bytes());
        }
        digest
    }
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        result.push(DIGITS[(byte >> 4) as usize] as char);
        result.push(DIGITS[(byte & 0x0f) as usize] as char);
    }
    result
}

fn integrity(path: &Path) -> Result<Json, String> {
    let mut file = File::open(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut whole = Sha256::new();
    let mut blocks = Vec::new();
    let mut buffer = vec![0u8; BLOCK];
    loop {
        let mut used = 0;
        while used < buffer.len() {
            let count = file
                .read(&mut buffer[used..])
                .map_err(|error| format!("{}: {error}", path.display()))?;
            if count == 0 {
                break;
            }
            used += count;
        }
        if used == 0 {
            break;
        }
        whole.update(&buffer[..used]);
        let mut block_hash = Sha256::new();
        block_hash.update(&buffer[..used]);
        blocks.push(Json::String(hex(&block_hash.finish())));
        if used < buffer.len() {
            break;
        }
    }
    if blocks.is_empty() {
        blocks.push(Json::String(hex(&Sha256::new().finish())));
    }
    Ok(Json::object(vec![
        ("algorithm".into(), Json::String("SHA256".into())),
        ("hash".into(), Json::String(hex(&whole.finish()))),
        ("blockSize".into(), Json::Number(Number::from(BLOCK as u64))),
        ("blocks".into(), Json::Array(blocks)),
    ]))
}

fn read_asar(path: &Path) -> Result<(Json, u64), String> {
    let mut file = File::open(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut prefix = [0u8; 8];
    file.read_exact(&mut prefix)
        .map_err(|error| format!("{}: {error}", path.display()))?;
    let marker = u32::from_le_bytes(prefix[..4].try_into().expect("four bytes"));
    let pickle_size = u32::from_le_bytes(prefix[4..].try_into().expect("four bytes")) as usize;
    if marker != 4 {
        return Err(format!("{}: not an asar archive", path.display()));
    }
    let mut pickle = vec![0u8; pickle_size];
    file.read_exact(&mut pickle)
        .map_err(|error| format!("{}: {error}", path.display()))?;
    if pickle.len() < 8 {
        return Err(format!("{}: invalid asar header pickle", path.display()));
    }
    let string_size = u32::from_le_bytes(pickle[4..8].try_into().expect("four bytes")) as usize;
    let string_end = 8usize
        .checked_add(string_size)
        .filter(|end| *end <= pickle.len())
        .ok_or_else(|| format!("{}: invalid asar header size", path.display()))?;
    let header: Json = serde_json::from_slice(&pickle[8..string_end])
        .map_err(|error| format!("{}: invalid asar header JSON: {error}", path.display()))?;
    Ok((header, 8 + pickle_size as u64))
}

fn encode(header: &Json) -> Result<Vec<u8>, String> {
    let json = serde_json::to_vec(header).map_err(|error| error.to_string())?;
    let padding = (4 - json.len() % 4) % 4;
    let string_size = u32::try_from(json.len()).map_err(|_| "asar header is too large".to_owned())?;
    let payload_size = u32::try_from(4 + json.len() + padding)
        .map_err(|_| "asar header is too large".to_owned())?;
    let pickle_size = u32::try_from(8 + json.len() + padding)
        .map_err(|_| "asar header is too large".to_owned())?;
    let mut output = Vec::with_capacity(8 + pickle_size as usize);
    output.extend_from_slice(&4u32.to_le_bytes());
    output.extend_from_slice(&pickle_size.to_le_bytes());
    output.extend_from_slice(&payload_size.to_le_bytes());
    output.extend_from_slice(&string_size.to_le_bytes());
    output.extend_from_slice(&json);
    output.resize(output.len() + padding, 0);
    Ok(output)
}

fn helpers(bindir: &Path) -> Result<Vec<(String, Json)>, String> {
    let entries = fs::read_dir(bindir).map_err(|error| format!("{}: {error}", bindir.display()))?;
    let mut files = Vec::new();
    for entry in entries {
        let entry = entry.map_err(|error| format!("{}: {error}", bindir.display()))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') {
            continue;
        }
        files.push((name, entry.path()));
    }
    files.sort_by(|a, b| a.0.cmp(&b.0));
    if files.is_empty() {
        return Err(format!("{}: no helpers", bindir.display()));
    }

    let mut result = Vec::with_capacity(files.len());
    for (name, path) in files {
        let size = fs::metadata(&path)
            .map_err(|error| format!("{}: {error}", path.display()))?
            .len();
        let metadata = Json::object(vec![
            ("size".into(), Json::Number(Number::from(size))),
            ("unpacked".into(), Json::Bool(true)),
            ("integrity".into(), integrity(&path)?),
        ]);
        result.push((name, metadata));
    }
    Ok(result)
}

fn named_files(root: Json) -> Json {
    Json::object(vec![("files".into(), root)])
}

fn retarget(src: &Path, dst: &Path, bindir: &Path) -> Result<(), String> {
    let (mut header, data_at) = read_asar(src)?;
    let bin_files = header
        .get_mut("files")
        .and_then(|files| files.get_mut("build"))
        .and_then(|build| build.get_mut("files"))
        .and_then(|files| files.get_mut("bin"))
        .and_then(|bin| bin.get_mut("files"))
        .ok_or_else(|| format!("{}: no build/bin/x64/linux in the archive", src.display()))?;
    let original = bin_files
        .as_object()
        .ok_or_else(|| format!("{}: no build/bin/x64/linux in the archive", src.display()))?
        .clone();
    let mut bins = original;
    let x64 = bins
        .iter_mut()
        .find(|(key, _)| key == "x64")
        .map(|(_, value)| value)
        .ok_or_else(|| format!("{}: no build/bin/x64/linux in the archive", src.display()))?;
    let x64_files = x64
        .get_mut("files")
        .ok_or_else(|| format!("{}: no build/bin/x64/linux in the archive", src.display()))?;
    if x64_files.remove("linux").is_none() {
        return Err(format!("{}: no build/bin/x64/linux in the archive", src.display()));
    }
    if bins.iter().any(|(key, _)| key == "arm64") {
        return Err(format!("{}: already has build/bin/arm64", src.display()));
    }

    let helper_entries = helpers(bindir)?;
    let helper_names = helper_entries
        .iter()
        .map(|(name, _)| name.as_str())
        .collect::<Vec<_>>()
        .join(" ");
    let linux = Json::Object(helper_entries);
    let arm64 = Json::object(vec![(
        "files".into(),
        Json::object(vec![("linux".into(), named_files(linux))]),
    )]);
    let mut rewritten = Vec::new();
    for (key, value) in bins {
        if key == "x64" {
            rewritten.push(("arm64".into(), arm64.clone()));
            if value.get("files").is_some_and(Json::truthy) {
                rewritten.push(("x64".into(), value));
            }
        } else {
            rewritten.push((key, value));
        }
    }
    bin_files.insert("files", Json::Object(rewritten));

    let encoded = encode(&header)?;
    let temporary = PathBuf::from(format!("{}.tmp", dst.display()));
    let mut output = File::create(&temporary)
        .map_err(|error| format!("{}: {error}", temporary.display()))?;
    output
        .write_all(&encoded)
        .map_err(|error| format!("{}: {error}", temporary.display()))?;
    let mut input = File::open(src).map_err(|error| format!("{}: {error}", src.display()))?;
    input
        .seek(SeekFrom::Start(data_at))
        .map_err(|error| format!("{}: {error}", src.display()))?;
    io::copy(&mut input, &mut output).map_err(|error| format!("{}: {error}", temporary.display()))?;
    output
        .flush()
        .map_err(|error| format!("{}: {error}", temporary.display()))?;
    drop(output);
    fs::rename(&temporary, dst).map_err(|error| format!("{}: {error}", dst.display()))?;

    println!(
        "retargeted {} -> {}: build/bin/arm64/linux = {helper_names}",
        src.display(),
        dst.display()
    );
    Ok(())
}

fn walk(node: &Json, base: &str, prefix: &str, output: &mut Vec<String>) {
    let Some(files) = node.get("files").and_then(Json::as_object) else {
        return;
    };
    for (name, value) in files {
        let path = if base.is_empty() {
            name.clone()
        } else {
            format!("{base}/{name}")
        };
        if value.get("files").is_some() {
            walk(value, &path, prefix, output);
        } else if path.starts_with(prefix) {
            let size = value.get("size").and_then(Json::integer).unwrap_or(0);
            let unpacked = value.get("unpacked").is_some_and(Json::truthy);
            output.push(format!("{path} {size}{}", if unpacked { " unpacked" } else { "" }));
        }
    }
}

fn listing(src: &Path, prefix: &str) -> Result<(), String> {
    let (header, _) = read_asar(src)?;
    let mut lines = Vec::new();
    walk(&header, "", prefix, &mut lines);
    for line in lines {
        println!("{line}");
    }
    Ok(())
}

fn run(args: &[String]) -> Result<i32, String> {
    match args {
        [command, src, dst, bindir] if command == "retarget" => {
            retarget(Path::new(src), Path::new(dst), Path::new(bindir))?;
            Ok(0)
        }
        [command, src] if command == "list" => {
            listing(Path::new(src), "")?;
            Ok(0)
        }
        [command, src, prefix] if command == "list" => {
            listing(Path::new(src), prefix)?;
            Ok(0)
        }
        _ => {
            eprintln!("{USAGE}");
            Ok(2)
        }
    }
}

fn main() {
    let args = env::args().skip(1).collect::<Vec<_>>();
    match run(&args) {
        Ok(status) => process::exit(status),
        Err(error) => {
            eprintln!("{error}");
            process::exit(1);
        }
    }
}
