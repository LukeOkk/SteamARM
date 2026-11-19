use std::env;
use std::fs;
use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const DEFAULT_HOST: &str = "127.0.0.1";
const DEFAULT_PORT: u16 = 5901;

fn state_dir() -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_STATE").filter(|v| !v.is_empty()) {
        let path = PathBuf::from(path);
        if path == Path::new("~") {
            return home_dir();
        }
        if let Ok(rest) = path.strip_prefix("~") {
            return home_dir().join(rest);
        }
        return path;
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn read_password() -> Result<Vec<u8>, String> {
    if let Some(value) = env::var_os("STEAMARM_VNC_PASSWORD").filter(|v| !v.is_empty()) {
        return Ok(value.to_string_lossy().as_bytes().to_vec());
    }
    let path = state_dir().join("vncpasswd.txt");
    let value = fs::read_to_string(&path)
        .map_err(|e| format!("read VNC password {}: {e}", path.display()))?;
    Ok(value.trim().as_bytes().to_vec())
}

fn ensure_password_files() -> Result<(), String> {
    let state = state_dir();
    let text_path = state.join("vncpasswd.txt");
    let auth_path = state.join("vncpasswd");
    if fs::metadata(&auth_path).is_ok_and(|metadata| metadata.len() > 0) {
        return Ok(());
    }
    fs::create_dir_all(&state)
        .map_err(|error| format!("create VNC state directory {}: {error}", state.display()))?;
    let password = env::var("STEAMARM_VNC_PASSWORD")
        .ok()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "steamarm".to_owned());
    let mut display_password = password.as_bytes().to_vec();
    display_password.push(b'\n');
    fs::write(&text_path, display_password)
        .map_err(|error| format!("write VNC password {}: {error}", text_path.display()))?;
    fs::set_permissions(&text_path, fs::Permissions::from_mode(0o600))
        .map_err(|error| format!("protect VNC password {}: {error}", text_path.display()))?;

    let mut child = Command::new("openssl")
        .args([
            "enc",
            "-des-ecb",
            "-K",
            "e84ad660c4721ae0",
            "-nopad",
            "-nosalt",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("start openssl DES: {error}"))?;
    let mut plain = [0u8; 8];
    for (destination, source) in plain.iter_mut().zip(password.as_bytes().iter().copied()) {
        *destination = if source == b' ' { 0 } else { source };
    }
    child
        .stdin
        .take()
        .ok_or("openssl stdin unavailable")?
        .write_all(&plain)
        .map_err(display_io)?;
    let encrypted = child.wait_with_output().map_err(display_io)?;
    if !encrypted.status.success() || encrypted.stdout.len() != 8 {
        return Err(format!(
            "openssl DES failed: {}",
            String::from_utf8_lossy(&encrypted.stderr).trim()
        ));
    }
    fs::write(&auth_path, encrypted.stdout).map_err(|error| {
        format!(
            "write VNC authentication file {}: {error}",
            auth_path.display()
        )
    })?;
    fs::set_permissions(&auth_path, fs::Permissions::from_mode(0o600)).map_err(|error| {
        format!(
            "protect VNC authentication file {}: {error}",
            auth_path.display()
        )
    })?;
    Ok(())
}

fn connect(host: &str, port: u16) -> io::Result<TcpStream> {
    let addresses = (host, port).to_socket_addrs()?.collect::<Vec<_>>();
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut last_error = None;
    for address in addresses {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            break;
        }
        match TcpStream::connect_timeout(&address, remaining) {
            Ok(stream) => {
                stream.set_read_timeout(Some(Duration::from_secs(20)))?;
                stream.set_write_timeout(Some(Duration::from_secs(20)))?;
                return Ok(stream);
            }
            Err(error) => last_error = Some(error),
        }
    }
    Err(last_error.unwrap_or_else(|| io::Error::new(io::ErrorKind::AddrNotAvailable, "no address")))
}

fn read_u16(bytes: &[u8]) -> u16 {
    u16::from_be_bytes([bytes[0], bytes[1]])
}

fn read_u32(bytes: &[u8]) -> u32 {
    u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn authenticate(stream: &mut TcpStream) -> Result<(), String> {
    let mut version = [0; 12];
    stream.read_exact(&mut version).map_err(display_io)?;
    stream.write_all(b"RFB 003.008\n").map_err(display_io)?;
    let mut count = [0; 1];
    stream.read_exact(&mut count).map_err(display_io)?;
    if count[0] == 0 {
        let mut size = [0; 4];
        stream.read_exact(&mut size).map_err(display_io)?;
        let length = read_u32(&size) as usize;
        let mut reason = vec![0; length.min(4096)];
        stream.read_exact(&mut reason).map_err(display_io)?;
        return Err(format!(
            "VNC server rejected protocol: {}",
            String::from_utf8_lossy(&reason)
        ));
    }
    let mut security = vec![0; count[0] as usize];
    stream.read_exact(&mut security).map_err(display_io)?;
    if security.contains(&1) {
        stream.write_all(&[1]).map_err(display_io)?;
    } else if security.contains(&2) {
        stream.write_all(&[2]).map_err(display_io)?;
        let mut challenge = [0; 16];
        stream.read_exact(&mut challenge).map_err(display_io)?;
        let password = read_password()?;
        let mut key = [0; 8];
        for (destination, source) in key.iter_mut().zip(password.iter().copied()) {
            *destination = source.reverse_bits();
        }
        let key_hex = key
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        let mut child = Command::new("openssl")
            .args(["enc", "-des-ecb", "-K", &key_hex, "-nopad", "-nosalt"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("start openssl DES: {e}"))?;
        child
            .stdin
            .take()
            .ok_or("openssl stdin unavailable")?
            .write_all(&challenge)
            .map_err(display_io)?;
        let response = child.wait_with_output().map_err(display_io)?;
        if !response.status.success() || response.stdout.len() != 16 {
            return Err(format!(
                "openssl DES failed: {}",
                String::from_utf8_lossy(&response.stderr).trim()
            ));
        }
        stream.write_all(&response.stdout).map_err(display_io)?;
    } else {
        return Err(format!("unsupported VNC security types {security:?}"));
    }
    let mut result = [0; 4];
    stream.read_exact(&mut result).map_err(display_io)?;
    if read_u32(&result) != 0 {
        return Err("VNC authentication failed".into());
    }
    Ok(())
}

fn display_io(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn discard_exact(stream: &mut TcpStream, mut length: usize) -> Result<(), String> {
    let mut buffer = [0; 8192];
    while length > 0 {
        let count = length.min(buffer.len());
        stream
            .read_exact(&mut buffer[..count])
            .map_err(display_io)?;
        length -= count;
    }
    Ok(())
}

fn server_init(stream: &mut TcpStream) -> Result<(u16, u16, String), String> {
    stream.write_all(&[1]).map_err(display_io)?;
    let mut header = [0; 24];
    stream.read_exact(&mut header).map_err(display_io)?;
    let width = read_u16(&header[0..2]);
    let height = read_u16(&header[2..4]);
    if width == 0 || height == 0 {
        return Err("VNC server reported an empty framebuffer".into());
    }
    let name_len = read_u32(&header[20..24]) as usize;
    if name_len > 1_048_576 {
        return Err("VNC desktop name exceeds 1 MiB".into());
    }
    let mut name = vec![0; name_len];
    stream.read_exact(&mut name).map_err(display_io)?;
    Ok((width, height, String::from_utf8_lossy(&name).into_owned()))
}

fn connect_session(host: &str, port: u16) -> Result<(TcpStream, u16, u16, String), String> {
    let mut stream = connect(host, port).map_err(display_io)?;
    authenticate(&mut stream)?;
    let (width, height, name) = server_init(&mut stream)?;
    Ok((stream, width, height, name))
}

fn snapshot(host: &str, port: u16, output: &Path) -> Result<(), String> {
    let (mut stream, width, height, name) = connect_session(host, port)?;
    let pixel_bytes = usize::from(width)
        .checked_mul(usize::from(height))
        .and_then(|pixels| pixels.checked_mul(4))
        .filter(|bytes| *bytes <= 128 * 1024 * 1024)
        .ok_or("VNC framebuffer exceeds 128 MiB")?;
    // 32 bpp, depth 24, little-endian BGRX with true colour.
    stream
        .write_all(&[
            0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0,
        ])
        .map_err(display_io)?;
    stream
        .write_all(&[2, 0, 0, 1, 0, 0, 0, 0])
        .map_err(display_io)?; // Raw encoding
    let mut request = [0; 10];
    request[0] = 3;
    request[4..6].copy_from_slice(&width.to_be_bytes());
    request[6..8].copy_from_slice(&height.to_be_bytes());
    stream.write_all(&request).map_err(display_io)?;

    let mut framebuffer = vec![0; pixel_bytes];
    loop {
        let mut message = [0; 1];
        stream.read_exact(&mut message).map_err(display_io)?;
        match message[0] {
            0 => {
                let mut header = [0; 3];
                stream.read_exact(&mut header).map_err(display_io)?;
                let rectangles = read_u16(&header[1..3]);
                for _ in 0..rectangles {
                    let mut rect = [0; 12];
                    stream.read_exact(&mut rect).map_err(display_io)?;
                    let x = usize::from(read_u16(&rect[0..2]));
                    let y = usize::from(read_u16(&rect[2..4]));
                    let rw = usize::from(read_u16(&rect[4..6]));
                    let rh = usize::from(read_u16(&rect[6..8]));
                    let encoding = i32::from_be_bytes(rect[8..12].try_into().unwrap());
                    if encoding != 0 {
                        return Err(format!("unexpected VNC encoding {encoding}"));
                    }
                    if x.checked_add(rw)
                        .is_none_or(|right| right > usize::from(width))
                        || y.checked_add(rh)
                            .is_none_or(|bottom| bottom > usize::from(height))
                    {
                        return Err("VNC rectangle lies outside the framebuffer".into());
                    }
                    let bytes = rw
                        .checked_mul(rh)
                        .and_then(|n| n.checked_mul(4))
                        .ok_or("invalid VNC rectangle size")?;
                    let mut data = vec![0; bytes];
                    stream.read_exact(&mut data).map_err(display_io)?;
                    for row in 0..rh {
                        let target = ((y + row) * usize::from(width) + x) * 4;
                        let start = row * rw * 4;
                        framebuffer[target..target + rw * 4]
                            .copy_from_slice(&data[start..start + rw * 4]);
                    }
                }
                break;
            }
            1 => {
                // SetColourMapEntries
                let mut header = [0; 5];
                stream.read_exact(&mut header).map_err(display_io)?;
                let count = usize::from(read_u16(&header[3..5]));
                let mut colors = vec![0; count * 6];
                stream.read_exact(&mut colors).map_err(display_io)?;
            }
            2 => {} // Bell
            3 => {
                // ServerCutText
                let mut header = [0; 7];
                stream.read_exact(&mut header).map_err(display_io)?;
                let length = read_u32(&header[3..7]) as usize;
                discard_exact(&mut stream, length)?;
            }
            kind => return Err(format!("unexpected VNC server message {kind}")),
        }
    }
    write_png(output, width, height, &framebuffer)?;
    let sampled = framebuffer
        .chunks_exact(4)
        .step_by(97)
        .filter(|pixel| pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)
        .count();
    println!(
        "{name}: {width}x{height} -> {} (sampled non-black pixels: {sampled})",
        output.display()
    );
    Ok(())
}

fn write_png(path: &Path, width: u16, height: u16, framebuffer: &[u8]) -> Result<(), String> {
    let w = usize::from(width);
    let h = usize::from(height);
    let mut scanlines = Vec::with_capacity(h * (1 + w * 3));
    for y in 0..h {
        scanlines.push(0); // PNG filter None
        let row = &framebuffer[y * w * 4..(y + 1) * w * 4];
        for pixel in row.chunks_exact(4) {
            scanlines.extend_from_slice(&[pixel[2], pixel[1], pixel[0]]);
        }
    }
    let compressed = zlib_store(&scanlines);
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = Vec::with_capacity(13);
    ihdr.extend_from_slice(&u32::from(width).to_be_bytes());
    ihdr.extend_from_slice(&u32::from(height).to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    png_chunk(&mut png, b"IHDR", &ihdr);
    png_chunk(&mut png, b"IDAT", &compressed);
    png_chunk(&mut png, b"IEND", &[]);
    fs::write(path, png).map_err(|e| format!("write {}: {e}", path.display()))
}

fn zlib_store(data: &[u8]) -> Vec<u8> {
    let mut out = vec![0x78, 0x01];
    if data.is_empty() {
        out.extend_from_slice(&[1, 0, 0, 255, 255]);
    } else {
        let chunks = data.chunks(u16::MAX as usize).collect::<Vec<_>>();
        for (index, chunk) in chunks.iter().enumerate() {
            out.push(u8::from(index + 1 == chunks.len()));
            let len = chunk.len() as u16;
            out.extend_from_slice(&len.to_le_bytes());
            out.extend_from_slice(&(!len).to_le_bytes());
            out.extend_from_slice(chunk);
        }
    }
    let mut a = 1u32;
    let mut b = 0u32;
    for byte in data {
        a = (a + u32::from(*byte)) % 65_521;
        b = (b + a) % 65_521;
    }
    out.extend_from_slice(&((b << 16) | a).to_be_bytes());
    out
}

fn png_chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    let mut crc = !0u32;
    for byte in kind.iter().chain(data) {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xedb8_8320
            } else {
                crc >> 1
            };
        }
    }
    out.extend_from_slice(&(!crc).to_be_bytes());
}

fn click(host: &str, port: u16, x: u16, y: u16) -> Result<(), String> {
    let (mut stream, _, _, _) = connect_session(host, port)?;
    for mask in [0u8, 1, 0] {
        let mut message = [0; 6];
        message[0] = 5;
        message[1] = mask;
        message[2..4].copy_from_slice(&x.to_be_bytes());
        message[4..6].copy_from_slice(&y.to_be_bytes());
        stream.write_all(&message).map_err(display_io)?;
        thread::sleep(Duration::from_millis(80));
    }
    thread::sleep(Duration::from_millis(200));
    Ok(())
}

fn key(host: &str, port: u16, keysym: u32) -> Result<(), String> {
    let (mut stream, _, _, _) = connect_session(host, port)?;
    for down in [1u8, 0] {
        let mut message = [0; 8];
        message[0] = 4;
        message[1] = down;
        message[4..8].copy_from_slice(&keysym.to_be_bytes());
        stream.write_all(&message).map_err(display_io)?;
        thread::sleep(Duration::from_millis(50));
    }
    thread::sleep(Duration::from_millis(200));
    Ok(())
}

fn parse_port(value: Option<&String>) -> Result<u16, String> {
    value.map_or(Ok(DEFAULT_PORT), |text| {
        text.parse().map_err(|_| format!("invalid port {text:?}"))
    })
}

fn run(args: &[String]) -> Result<(), String> {
    match args.first().map(String::as_str) {
        Some("password") if args.len() == 1 => ensure_password_files(),
        Some("snapshot") if args.len() <= 4 => {
            let host = args.get(1).map(String::as_str).unwrap_or(DEFAULT_HOST);
            let port = parse_port(args.get(2))?;
            let output = args
                .get(3)
                .map(PathBuf::from)
                .unwrap_or_else(|| PathBuf::from("vnc.png"));
            snapshot(host, port, &output)
        }
        Some("click") if args.len() >= 3 && args.len() <= 5 => {
            let x = args[1]
                .parse()
                .map_err(|_| format!("invalid x coordinate {:?}", args[1]))?;
            let y = args[2]
                .parse()
                .map_err(|_| format!("invalid y coordinate {:?}", args[2]))?;
            let host = args.get(3).map(String::as_str).unwrap_or(DEFAULT_HOST);
            let port = parse_port(args.get(4))?;
            click(host, port, x, y)
        }
        Some("key") if args.len() >= 2 && args.len() <= 4 => {
            let value = args[1].strip_prefix("0x").unwrap_or(&args[1]);
            let keysym =
                u32::from_str_radix(value, 16).map_err(|_| format!("invalid keysym {value:?}"))?;
            let host = args.get(2).map(String::as_str).unwrap_or(DEFAULT_HOST);
            let port = parse_port(args.get(3))?;
            key(host, port, keysym)
        }
        Some("--help") | Some("-h") | None => {
            println!("Usage:\n  steamarm-vnc-control password\n  steamarm-vnc-control snapshot [host] [port] [out.png]\n  steamarm-vnc-control click <x> <y> [host] [port]\n  steamarm-vnc-control key <keysym-hex> [host] [port]");
            Ok(())
        }
        _ => Err("invalid arguments; use --help".into()),
    }
}

fn main() -> ExitCode {
    match run(&env::args().skip(1).collect::<Vec<_>>()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("steamarm-vnc-control: {error}");
            ExitCode::from(1)
        }
    }
}
