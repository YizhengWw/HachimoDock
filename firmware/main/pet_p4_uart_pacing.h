/* Control replies and screenshots only; never the no-drain audio/media path.
 * Fixed build-time bounds, not an input-controlled transport knob. */
#pragma once
#ifndef PET_P4_UART_CONTROL_PACING
#define PET_P4_UART_CONTROL_PACING 0
#endif
#if PET_P4_UART_CONTROL_PACING != 0 && PET_P4_UART_CONTROL_PACING != 1
#error "Invalid control UART pacing profile"
#endif
#ifndef PET_P4_UART_PACED_BYTES
#define PET_P4_UART_PACED_BYTES 64
#endif
#if PET_P4_UART_PACED_BYTES != 64 && PET_P4_UART_PACED_BYTES != 256
#error "Unaudited private UART burst size"
#endif
#if PET_P4_UART_CONTROL_PACING
#define PET_P4_UART_PACING_ACTIVE 1
#define PET_P4_UART_TX_BURST_BYTES 256
#elif defined(PET_P4_UART_PACED_REFERENCE) && PET_P4_UART_PACED_REFERENCE
#define PET_P4_UART_PACING_ACTIVE 1
#define PET_P4_UART_TX_BURST_BYTES PET_P4_UART_PACED_BYTES
#else
#define PET_P4_UART_PACING_ACTIVE 0
#define PET_P4_UART_TX_BURST_BYTES PET_P4_UART_PACED_BYTES
#endif
