/*
 * [Input] Serial port names/baud rates plus verified connection metadata.
 * [Output] Paired reader/writer handles (overlapped on Windows), retry-safe opens, and owned live connection state.
 * [Pos] OS handle and connection-ownership boundary beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use super::SerialMessage;
use serde_json::Value;
use std::io::{BufReader, Read, Write};
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

pub(super) struct ProbedSerialPort<W = Box<dyn serialport::SerialPort>, R = Box<dyn serialport::SerialPort>> {
    pub(super) writer: W,
    pub(super) reader: BufReader<R>,
    pub(super) baud: u32,
    pub(super) hello: SerialMessage,
}

impl<W, R: Read> ProbedSerialPort<W, R> {
    // Move the SAME reader across the verified handshake boundary. into_inner
    // would discard bytes already read from the driver after the hello line.
    pub(super) fn from_verified_reader(writer: W, reader: BufReader<R>, baud: u32, hello: SerialMessage) -> Self {
        Self { writer, reader, baud, hello }
    }
}

type SerialPortHandle = Box<dyn serialport::SerialPort>;
pub(super) type SerialPortPair = (SerialPortHandle, SerialPortHandle);
pub(super) type SerialPortPairResult = Result<SerialPortPair, String>;

#[cfg(windows)]
const SERIAL_IO_TIMEOUT: Duration = Duration::from_millis(20);
#[cfg(not(windows))]
const SERIAL_IO_TIMEOUT: Duration = Duration::from_millis(100);

pub(super) struct UsbConnection {
    pub(super) connection_id: u64,
    pub(super) port_name: String,
    pub(super) baud_rate: u32,
    pub(super) writer: Box<dyn Write + Send>,
    pub(super) board_device_id: String,
    pub(super) runtime: String,
    pub(super) device_model: String,
    pub(super) firmware: String,
    pub(super) build_id: String,
    pub(super) git_sha: String,
    pub(super) build_dirty: bool,
    pub(super) protocol_schema: u32,
    pub(super) wire_protocol: String,
    pub(super) capabilities: Value,
    pub(super) connected: bool,
    pub(super) cancel_reader: Arc<AtomicBool>,
    pub(super) capture_active: Arc<AtomicBool>,
}

pub(super) fn serial_open_error_is_transient(error: &str) -> bool {
    error.contains("Access is denied")
        || error.contains("Unable to acquire exclusive lock")
        || error.contains("Resource busy")
        || error.contains("Device busy")
        || error.contains("拒绝访问")
}

#[cfg(windows)]
fn clear_serial_handle_inheritance(port: &impl std::os::windows::io::AsRawHandle) -> Result<(), String> {
    use std::ffi::c_void;

    const HANDLE_FLAG_INHERIT: u32 = 0x0000_0001;

    #[link(name = "kernel32")]
    extern "system" {
        fn SetHandleInformation(handle: *mut c_void, mask: u32, flags: u32) -> i32;
    }

    let ok = unsafe { SetHandleInformation(port.as_raw_handle().cast(), HANDLE_FLAG_INHERIT, 0) };
    if ok == 0 {
        Err(format!(
            "failed to make serial handle non-inheritable: {}",
            std::io::Error::last_os_error()
        ))
    } else {
        Ok(())
    }
}

#[cfg(windows)]
fn open_serial_pair(port_name: &str, baud: u32) -> SerialPortPairResult {
    let port = super::windows_serial::WindowsSerialPort::open(port_name, baud, SERIAL_IO_TIMEOUT)
        .map_err(|error| error.to_string())?;
    clear_serial_handle_inheritance(&port)?;

    // serialport's Windows clone requests an inheritable duplicate, so clear
    // the flag on both handles before any Bridge/voice child can be spawned.
    let reader = port.try_clone_native().map_err(|error| error.to_string())?;
    clear_serial_handle_inheritance(&reader)?;
    Ok((Box::new(port), Box::new(reader)))
}

const SERIAL_OPEN_DEADLINE: Duration = Duration::from_secs(5);

#[cfg(not(windows))]
fn open_serial_pair(port_name: &str, baud: u32) -> SerialPortPairResult {
    // A wedged USB-serial adapter (e.g. a CH340 hung inside the driver) can
    // block open(2) indefinitely, pinning the caller in kernel U state. Run
    // the blocking open on a helper thread with a bounded deadline so the
    // connection path always returns. If we time out we abandon the helper;
    // as soon as the kernel unblocks it (device re-plug) it closes its fd,
    // so no port is held open by a zombie thread.
    let port_name = port_name.to_string();
    let port_name_for_msg = port_name.clone();
    let (tx, rx) = std::sync::mpsc::channel();
    thread::spawn(move || {
        let result: Result<(SerialPortHandle, SerialPortHandle), String> = (|| {
            let mut port = serialport::new(&port_name, baud)
                .timeout(SERIAL_IO_TIMEOUT)
                .dtr_on_open(false)
                .open()
                .map_err(|error| error.to_string())?;
            // Runtime discovery is not a flashing/reset operation. Best
            // effort keeps CDC drivers without modem-line ioctls compatible.
            let _ = port.write_request_to_send(false);
            let reader = port.try_clone().map_err(|error| error.to_string())?;
            Ok((port, reader))
        })();
        // If the caller already timed out this send fails and the port is
        // dropped here, releasing the fd.
        let _ = tx.send(result);
    });
    match rx.recv_timeout(SERIAL_OPEN_DEADLINE) {
        Ok(result) => result,
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => Err(format!(
            "open of {port_name_for_msg} timed out after {}s (device may be wedged, re-plug USB)",
            SERIAL_OPEN_DEADLINE.as_secs()
        )),
        Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
            Err("serial open helper failed".to_string())
        }
    }
}

pub(super) fn open_serial_pair_with_retry(port_name: &str, baud: u32) -> SerialPortPairResult {
    let mut last_open_error = String::new();
    for attempt in 0..6 {
        match open_serial_pair(port_name, baud) {
            Ok(ports) => return Ok(ports),
            Err(error) => {
                last_open_error = error;
                // A wedged-device open timeout will not heal within one probe
                // cycle; return it to the caller immediately instead of
                // burning the full retry budget on repeated 5s deadlines.
                let wedged = last_open_error.contains("timed out after");
                if !wedged && serial_open_error_is_transient(&last_open_error) && attempt < 5 {
                    thread::sleep(Duration::from_millis(220));
                    continue;
                }
                break;
            }
        }
    }
    Err(format!(
        "failed to open {port_name} at {baud} baud: {last_open_error}"
    ))
}
