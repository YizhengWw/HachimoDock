/*
 * [Input] Serial port names/baud rates plus verified connection metadata.
 * [Output] Paired reader/writer handles, retry-safe opens, and owned live connection state.
 * [Pos] OS handle and connection-ownership boundary beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use super::SerialMessage;
use serde_json::Value;
use std::io::Write;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

pub(super) struct ProbedSerialPort {
    pub(super) writer: Box<dyn serialport::SerialPort>,
    pub(super) reader: Box<dyn serialport::SerialPort>,
    pub(super) baud: u32,
    pub(super) hello: SerialMessage,
}

type SerialPortHandle = Box<dyn serialport::SerialPort>;
type SerialPortPair = (SerialPortHandle, SerialPortHandle);
type SerialPortPairResult = Result<SerialPortPair, String>;

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
}

pub(super) fn serial_open_error_is_transient(error: &str) -> bool {
    error.contains("Access is denied")
        || error.contains("Unable to acquire exclusive lock")
        || error.contains("Resource busy")
        || error.contains("Device busy")
        || error.contains("拒绝访问")
}

#[cfg(windows)]
fn clear_serial_handle_inheritance(port: &serialport::COMPort) -> Result<(), String> {
    use std::ffi::c_void;
    use std::os::windows::io::AsRawHandle;

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
    let port = serialport::new(port_name, baud)
        .timeout(SERIAL_IO_TIMEOUT)
        .open_native()
        .map_err(|error| error.to_string())?;
    clear_serial_handle_inheritance(&port)?;

    // serialport's Windows clone requests an inheritable duplicate, so clear
    // the flag on both handles before any Bridge/voice child can be spawned.
    let reader = port.try_clone_native().map_err(|error| error.to_string())?;
    clear_serial_handle_inheritance(&reader)?;
    Ok((Box::new(port), Box::new(reader)))
}

#[cfg(not(windows))]
fn open_serial_pair(port_name: &str, baud: u32) -> SerialPortPairResult {
    let port = serialport::new(port_name, baud)
        .timeout(SERIAL_IO_TIMEOUT)
        .open()
        .map_err(|error| error.to_string())?;
    let reader = port.try_clone().map_err(|error| error.to_string())?;
    Ok((port, reader))
}

pub(super) fn open_serial_pair_with_retry(port_name: &str, baud: u32) -> SerialPortPairResult {
    let mut last_open_error = String::new();
    for attempt in 0..6 {
        match open_serial_pair(port_name, baud) {
            Ok(ports) => return Ok(ports),
            Err(error) => {
                last_open_error = error;
                if serial_open_error_is_transient(&last_open_error) && attempt < 5 {
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
