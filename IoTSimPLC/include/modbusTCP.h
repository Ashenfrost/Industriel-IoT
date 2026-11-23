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
//
//   Sample buffer and clock (see buffer.h). R = read (03), W = write (06/16).
//     100  UTC seconds, high word   R/W  write 100+101 together (function 16);
//     101  UTC seconds, low word    R/W  the clock is set when 101 is written
//     102  Clock set (1) or not (0)  R
//     103  Samples in the buffer     R
//     104  Oldest sequence number    R   0 = buffer empty
//     105  Age of the oldest, s      R   65535 = older than that
//     106  Acknowledge               W   removes every sample up to and
//                                        including this seq; reads back the last one
//     107  Samples dropped (full)    R   since boot, 65535 = more
//     108  Window start seq          R/W which sample the window starts at
//     109  Newest sequence number    R   0 = buffer empty
//
//     200..319  Window: 12 samples of 10 registers, from the seq in 108.
//               Read it in one request (120 registers = the most that fits
//               in one Modbus TCP frame of this library).
//       +0 seq (0 = no sample here)  +1 UTC high  +2 UTC low (0 = clock not set)
//       +3 status  +4 top temp  +5 top hum  +6 middle temp  +7 middle hum
//       +8 bottom temp  +9 bottom hum   (x10, temperature int16)
//
//   Reading the whole buffer: write 108 = oldest seq (104), read 200..319,
//   write 108 = last seq read + 1 (wrapping 65535 -> 1), and so on.

// Start the Modbus TCP server. Call after network_initialize().
void modbusInit(void);

// Call every loop(). Handles requests, applies coils to outputs,
// and copies the latest sensor values into the registers.
void modbusUpdate(void);
