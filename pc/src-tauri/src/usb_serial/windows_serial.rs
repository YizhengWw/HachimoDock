//! Windows duplex serial transport. Configuration uses serialport; data I/O must
//! use OVERLAPPED on the handle opened here, never COMPort::read/write.
//! Each clone owns its own manual-reset event. Pending requests are completed or
//! cancelled AND joined before their buffers/OVERLAPPED go out of scope.
use serialport::{COMPort, SerialPort, DataBits, FlowControl, Parity, StopBits, ClearBuffer};
use std::{io::{self, Read, Write}, os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle, RawHandle}, time::Duration};
use windows_sys::Win32::{
    Devices::Communication::{DCB, COMMTIMEOUTS, GetCommState, SetCommState, SetCommTimeouts},
    Foundation::{ERROR_IO_PENDING, ERROR_OPERATION_ABORTED, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE, SetHandleInformation, HANDLE_FLAG_INHERIT, WAIT_OBJECT_0, WAIT_TIMEOUT},
    Storage::FileSystem::{CreateFileW, ReadFile, WriteFile, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED, OPEN_EXISTING},
    System::{IO::{OVERLAPPED, CancelIoEx, GetOverlappedResult}, Threading::{CreateEventW, ResetEvent, WaitForSingleObject}},
};

pub(super) struct WindowsSerialPort {
    control: COMPort,
    event: OwnedHandle,
    name: String,
    timeout: Duration,
}

impl AsRawHandle for WindowsSerialPort {
    fn as_raw_handle(&self) -> RawHandle { self.control.as_raw_handle() }
}

impl WindowsSerialPort {
    pub(super) fn open(name: &str, baud: u32, timeout: Duration) -> serialport::Result<Self> {
        if name.contains('\0') { return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid serial port name").into()); }
        let device = if name.starts_with(r"\\.\") { name.to_owned() } else { format!(r"\\.\{name}") };
        let wide: Vec<u16> = device.encode_utf16().chain(Some(0)).collect();
        // Null security attributes: neither this handle nor its event is inherited.
        let handle = unsafe { CreateFileW(wide.as_ptr(), GENERIC_READ | GENERIC_WRITE, 0,
            std::ptr::null(), OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, std::ptr::null_mut()) };
        if handle == INVALID_HANDLE_VALUE { return Err(io::Error::last_os_error().into()); }
        // Transfer ownership immediately, including all error paths below.
        let control = unsafe { COMPort::from_raw_handle(handle) };
        let mut dcb: DCB = unsafe { std::mem::zeroed() };
        dcb.DCBlength = std::mem::size_of::<DCB>() as u32;
        if unsafe { GetCommState(handle, &mut dcb) } == 0 { return Err(io::Error::last_os_error().into()); }
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = 0;
        dcb.StopBits = 0;
        // Binary 8N1, no software/hardware flow control, DTR/RTS disabled.
        dcb._bitfield = 1;
        if unsafe { SetCommState(handle, &dcb) } == 0 { return Err(io::Error::last_os_error().into()); }
        let mut port = Self::wrap(control, name.to_owned(), timeout)?;
        port.set_timeout(timeout)?;
        Ok(port)
    }

    fn wrap(control: COMPort, name: String, timeout: Duration) -> serialport::Result<Self> {
        if unsafe { SetHandleInformation(control.as_raw_handle(), HANDLE_FLAG_INHERIT, 0) } == 0 {
            return Err(io::Error::last_os_error().into());
        }
        let event = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
        if event.is_null() { return Err(io::Error::last_os_error().into()); }
        Ok(Self { control, event: unsafe { OwnedHandle::from_raw_handle(event) }, name, timeout })
    }

    pub(super) fn try_clone_native(&self) -> serialport::Result<Self> {
        Self::wrap(self.control.try_clone_native()?, self.name.clone(), self.timeout)
    }

    fn handle(&self) -> HANDLE { self.control.as_raw_handle() }

    fn transfer(&mut self, buffer: *mut u8, size: usize, reading: bool) -> io::Result<usize> {
        if size == 0 { return Ok(0); }
        let event = self.event.as_raw_handle();
        if unsafe { ResetEvent(event) } == 0 { return Err(io::Error::last_os_error()); }
        let mut operation: OVERLAPPED = unsafe { std::mem::zeroed() };
        operation.hEvent = event;
        let mut transferred = 0u32;
        let size = size.min(u32::MAX as usize) as u32;
        // SAFETY: read/write borrow the buffer exclusively for this whole call.
        // Every pending path below joins completion before returning.
        let ok = unsafe {
            if reading { ReadFile(self.handle(), buffer, size, &mut transferred, &mut operation) }
            else { WriteFile(self.handle(), buffer.cast_const(), size, &mut transferred, &mut operation) }
        };
        if ok == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(ERROR_IO_PENDING as i32) { return Err(error); }
            // Driver read timeout is short; this deadline is a cancellation backstop.
            let wait_ms = self.timeout.as_millis().clamp(1, u32::MAX as u128 - 2001) as u32 + 2000;
            let wait = unsafe { WaitForSingleObject(event, wait_ms) };
            if wait != WAIT_OBJECT_0 {
                let wait_error = if wait == WAIT_TIMEOUT {
                    io::Error::new(io::ErrorKind::TimedOut, "serial overlapped I/O timed out")
                } else { io::Error::last_os_error() };
                // Cancel ONLY this operation: never cancel the simultaneous RX/TX.
                // CancelIoEx alone is not completion; join even if cancellation races.
                unsafe { CancelIoEx(self.handle(), &operation); }
                let completed = unsafe { GetOverlappedResult(self.handle(), &operation, &mut transferred, 1) };
                if completed != 0 { return Self::transfer_result(transferred, reading); }
                let error = io::Error::last_os_error();
                return Err(if error.raw_os_error() == Some(ERROR_OPERATION_ABORTED as i32) { wait_error } else { error });
            }
            if unsafe { GetOverlappedResult(self.handle(), &operation, &mut transferred, 1) } == 0 {
                return Err(io::Error::last_os_error());
            }
        }
        Self::transfer_result(transferred, reading)
    }

    fn transfer_result(bytes: u32, reading: bool) -> io::Result<usize> {
        if bytes == 0 && reading { Err(io::Error::new(io::ErrorKind::TimedOut, "serial read timed out")) }
        else { Ok(bytes as usize) }
    }
}

impl Read for WindowsSerialPort {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> { self.transfer(buffer.as_mut_ptr(), buffer.len(), true) }
}
impl Write for WindowsSerialPort {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> { self.transfer(buffer.as_ptr().cast_mut(), buffer.len(), false) }
    // Only non-live asset/firmware paths use flush; preserve their drain contract.
    fn flush(&mut self) -> io::Result<()> { self.control.flush() }
}

impl SerialPort for WindowsSerialPort {
    fn name(&self) -> Option<String> { Some(self.name.clone()) }
    fn baud_rate(&self) -> serialport::Result<u32> { self.control.baud_rate() }
    fn data_bits(&self) -> serialport::Result<DataBits> { self.control.data_bits() }
    fn flow_control(&self) -> serialport::Result<FlowControl> { self.control.flow_control() }
    fn parity(&self) -> serialport::Result<Parity> { self.control.parity() }
    fn stop_bits(&self) -> serialport::Result<StopBits> { self.control.stop_bits() }
    fn timeout(&self) -> Duration { self.timeout }
    fn set_baud_rate(&mut self, value: u32) -> serialport::Result<()> { self.control.set_baud_rate(value) }
    fn set_data_bits(&mut self, value: DataBits) -> serialport::Result<()> { self.control.set_data_bits(value) }
    fn set_flow_control(&mut self, value: FlowControl) -> serialport::Result<()> { self.control.set_flow_control(value) }
    fn set_parity(&mut self, value: Parity) -> serialport::Result<()> { self.control.set_parity(value) }
    fn set_stop_bits(&mut self, value: StopBits) -> serialport::Result<()> { self.control.set_stop_bits(value) }
    fn set_timeout(&mut self, value: Duration) -> serialport::Result<()> {
        let ms = value.as_millis().clamp(1, u32::MAX as u128 - 1) as u32;
        let timeouts = COMMTIMEOUTS { ReadIntervalTimeout: u32::MAX, ReadTotalTimeoutMultiplier: u32::MAX,
            ReadTotalTimeoutConstant: ms, WriteTotalTimeoutMultiplier: 0, WriteTotalTimeoutConstant: ms.max(500) };
        if unsafe { SetCommTimeouts(self.handle(), &timeouts) } == 0 { return Err(io::Error::last_os_error().into()); }
        self.timeout = value;
        Ok(())
    }
    fn write_request_to_send(&mut self, value: bool) -> serialport::Result<()> { self.control.write_request_to_send(value) }
    fn write_data_terminal_ready(&mut self, value: bool) -> serialport::Result<()> { self.control.write_data_terminal_ready(value) }
    fn read_clear_to_send(&mut self) -> serialport::Result<bool> { self.control.read_clear_to_send() }
    fn read_data_set_ready(&mut self) -> serialport::Result<bool> { self.control.read_data_set_ready() }
    fn read_ring_indicator(&mut self) -> serialport::Result<bool> { self.control.read_ring_indicator() }
    fn read_carrier_detect(&mut self) -> serialport::Result<bool> { self.control.read_carrier_detect() }
    fn bytes_to_read(&self) -> serialport::Result<u32> { self.control.bytes_to_read() }
    fn bytes_to_write(&self) -> serialport::Result<u32> { self.control.bytes_to_write() }
    fn clear(&self, value: ClearBuffer) -> serialport::Result<()> { self.control.clear(value) }
    fn try_clone(&self) -> serialport::Result<Box<dyn SerialPort>> { Ok(Box::new(self.try_clone_native()?)) }
    fn set_break(&self) -> serialport::Result<()> { self.control.set_break() }
    fn clear_break(&self) -> serialport::Result<()> { self.control.clear_break() }
}
