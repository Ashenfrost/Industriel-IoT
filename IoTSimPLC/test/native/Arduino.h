// Minimal Arduino stand-in so buffer.cpp can be tested on a PC (test_buffer.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>

extern unsigned long fake_millis;
inline unsigned long millis() { return fake_millis; }
