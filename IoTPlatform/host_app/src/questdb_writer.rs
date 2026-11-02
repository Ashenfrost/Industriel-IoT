//! Gemmer målinger i QuestDB via ILP (InfluxDB Line Protocol over TCP, port 9009).
//! Dette modul ved INTET om MQTT eller Sparkplug B - det modtager kun `SensorReading`.

use anyhow::{bail, Result};
use questdb::ingress::{Buffer, Sender, TimestampNanos};

use crate::model::{SensorReading, Writer};

const TABLE: &str = "PLC_Sensor_Data";

pub struct QuestDbWriter {
    sender: Sender,
    buffer: Buffer,
    /// false = brug serverens tid ved indskrivning (som nu)
    /// true  = brug tidspunktet fra målepunktet (når afsenderen leverer det)
    use_device_timestamp: bool,
}

impl QuestDbWriter {
    /// `conf` ser ud som: "tcp::addr=localhost:9009;"
    pub fn new(conf: &str, use_device_timestamp: bool) -> Result<Self> {
        let sender = Sender::from_conf(conf)?;
        let buffer = Buffer::new();
        Ok(Self { sender, buffer, use_device_timestamp })
    }

    /// Vores "prepared statement"-agtige kontrol: er målingen rimelig nok til at gemme?
    fn validate(r: &SensorReading) -> Result<()> {
        if r.device_id.is_empty() {
            bail!("tomt device_id");
        }
        let values = [
            ("top_temp", r.top_temp),
            ("middle_temp", r.middle_temp),
            ("bottom_temp", r.bottom_temp),
            ("top_rh", r.top_rh),
            ("middle_rh", r.middle_rh),
            ("bottom_rh", r.bottom_rh),
        ];
        for (name, v) in values {
            if !v.is_finite() {
                bail!("{name} er ikke et gyldigt tal");
            }
        }
        for (name, v) in [("top_rh", r.top_rh), ("middle_rh", r.middle_rh), ("bottom_rh", r.bottom_rh)] {
            if !(0.0..=100.0).contains(&v) {
                bail!("{name}={v} ligger uden for 0-100 %");
            }
        }
        Ok(())
    }
}

impl Writer for QuestDbWriter {
    fn write(&mut self, r: &SensorReading) -> Result<()> {
        Self::validate(r)?;

        let row = self
            .buffer
            .table(TABLE)?
            .column_str("device_id", &r.device_id)?
            .column_f64("top_temp", r.top_temp)?
            .column_f64("top_rh", r.top_rh)?
            .column_f64("middle_temp", r.middle_temp)?
            .column_f64("middle_rh", r.middle_rh)?
            .column_f64("bottom_temp", r.bottom_temp)?
            .column_f64("bottom_rh", r.bottom_rh)?;

        match (self.use_device_timestamp, r.timestamp_ms) {
            (true, Some(ms)) => row.at(TimestampNanos::new(ms * 1_000_000))?,
            _ => row.at(TimestampNanos::now())?,
        }

        self.sender.flush(&mut self.buffer)?;
        Ok(())
    }
}
