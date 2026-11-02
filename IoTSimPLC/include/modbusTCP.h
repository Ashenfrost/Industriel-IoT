#pragma once

// Modbus TCP server (port 502) for the simulated PLC.
// Single device, so the Unit ID is not used.
//
// Register map:
//
//   Coils (read/write, function 01/05/15)
//     0  Top    overheating     1  Middle overheating     2  Bottom overheating
//     3  Top    too humid       4  Middle too humid       5  Bottom too humid
//     All written by IoTEdge. Buzzer, display and serial follow these coils.
//
//   Holding registers (read, function 03). Values are x10, temperature is int16.
//     0  Top    temperature        1  Top    humidity
//     2  Middle temperature        3  Middle humidity
//     4  Bottom temperature        5  Bottom humidity
//     6  Heartbeat (increments once per scan, 0 = no scan yet)
//     7  Status bitmask (bit 0..2 = sensor top/middle/bottom read error)

// Start the Modbus TCP server. Call after network_initialize().
void modbusInit(void);

// Call every loop(). Handles requests, applies coils to outputs,
// and copies the latest sensor values into the registers.
void modbusUpdate(void);
