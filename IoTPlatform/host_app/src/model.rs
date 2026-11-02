//! Fælles datatyper og "kontrakter" (traits) mellem modulerne.
//!
//! Alle moduler kender kun til dennee fil - de kender IKKE hinanden.
//! Det er det, der gør det muligt at skifte et modul ud uden at røre de andre.

use anyhow::Result;

/// En rå besked præcis som den kom fra MQTT-brokeren.
#[derive(Debug, Clone)]
pub struct RawMessage {
    pub topic: String,
    pub payload: Vec<u8>,
}

/// En færdig måling, klar til at blive gemt. Matcher tabellen PLC_Sensor_Data.
#[derive(Debug, Clone, PartialEq)]
pub struct SensorReading {
    pub device_id: String,
    /// Tidspunkt fra målepunktet (millisekunder siden 1970), hvis beskeden har et.
    pub timestamp_ms: Option<i64>,
    pub top_temp: f64,
    pub top_rh: f64,
    pub middle_temp: f64,
    pub middle_rh: f64,
    pub bottom_temp: f64,
    pub bottom_rh: f64,
}

/// Kontrakt for noget, der kan omsætte en rå besked til en måling.
/// Sparkplug B er EN implementering. En JSON-decoder kunne være en anden.
pub trait Decoder {
    fn decode(&self, msg: &RawMessage) -> Result<SensorReading>;
}

/// Kontrakt for noget, der kan gemme en måling.
/// QuestDB er EN implementering. En fil, MariaDB eller en test-stub kunne være andre.
pub trait Writer {
    fn write(&mut self, reading: &SensorReading) -> Result<()>;
}
