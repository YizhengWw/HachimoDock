/*
 * [Input] Optional target-specific PET_P4_UART_BAUD compiler definition.
 * [Output] One shared UART baud contract for firmware initialization and hello capability metadata.
 * [Pos] ESP32-P4 target-isolated transport build configuration.
 * [Sync] If this file changes, update `firmware/.folder.md`, README.md, and protocol.md.
 */

#pragma once

#ifndef PET_P4_UART_BAUD
#define PET_P4_UART_BAUD 4000000
#endif
