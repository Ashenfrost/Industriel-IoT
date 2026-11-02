//! Sparkplug B-decoder.
//!
//! Sparkplug B sender en Protobuf-besked på topics som
//!   spBv1.0/<group>/DDATA/<edge_node>/<device>
//! Vi har kun defineret de felter af Sparkplug-skemaet, vi faktisk bruger.
//! Felt-numrene (tags) er fra den officielle sparkplug_b.proto.

use anyhow::{anyhow, bail, Context, Result};
use prost::Message;

use crate::model::{Decoder, RawMessage, SensorReading};

// ---------- Protobuf-skema (delmængde) ----------

#[derive(Clone, PartialEq, Message)]
pub struct Payload {
    #[prost(uint64, optional, tag = "1")]
    pub timestamp: Option<u64>,
    #[prost(message, repeated, tag = "2")]
    pub metrics: Vec<Metric>,
    #[prost(uint64, optional, tag = "3")]
    pub seq: Option<u64>,
}

#[derive(Clone, PartialEq, Message)]
pub struct Metric {
    #[prost(string, optional, tag = "1")]
    pub name: Option<String>,
    #[prost(uint64, optional, tag = "3")]
    pub timestamp: Option<u64>,
    #[prost(uint32, optional, tag = "4")]
    pub datatype: Option<u32>,
    #[prost(oneof = "metric::Value", tags = "10, 11, 12, 13, 14, 15")]
    pub value: Option<metric::Value>,
}

pub mod metric {
    #[derive(Clone, PartialEq, ::prost::Oneof)]
    pub enum Value {
        #[prost(uint32, tag = "10")]
        IntValue(u32),
        #[prost(uint64, tag = "11")]
        LongValue(u64),
        #[prost(float, tag = "12")]
        FloatValue(f32),
        #[prost(double, tag = "13")]
        DoubleValue(f64),
        #[prost(bool, tag = "14")]
        BooleanValue(bool),
        #[prost(string, tag = "15")]
        StringValue(String),
    }
}

impl Metric {
    /// Værdien som f64, uanset om afsenderen brugte int, float eller double.
    fn as_f64(&self) -> Option<f64> {
        match self.value.as_ref()? {
            metric::Value::IntValue(v) => Some(*v as f64),
            metric::Value::LongValue(v) => Some(*v as f64),
            metric::Value::FloatValue(v) => Some(*v as f64),
            metric::Value::DoubleValue(v) => Some(*v),
            _ => None,
        }
    }
}

// ---------- Decoderen ----------

pub struct SparkplugBDecoder;

impl SparkplugBDecoder {
    pub fn new() -> Self {
        SparkplugBDecoder
    }
}

impl Decoder for SparkplugBDecoder {
    fn decode(&self, msg: &RawMessage) -> Result<SensorReading> {
        // 1. Device-id står i topic'en: spBv1.0/group/DDATA/node/device
        let parts: Vec<&str> = msg.topic.split('/').collect();
        if parts.len() < 5 || parts[0] != "spBv1.0" {
            bail!("ikke en Sparkplug B-topic: {}", msg.topic);
        }
        if parts[2] != "DDATA" && parts[2] != "NDATA" {
            bail!("ignorerer beskedtype {}", parts[2]);
        }
        let device_id = parts[4].to_string();

        // 2. Protobuf -> Payload
        let payload = Payload::decode(msg.payload.as_slice())
            .context("kunne ikke dekode Sparkplug B-payload")?;

        // 3. Find de seks metrics vi skal bruge ud fra deres navn
        let get = |name: &str| -> Result<f64> {
            payload
                .metrics
                .iter()
                .find(|m| m.name.as_deref() == Some(name))
                .ok_or_else(|| anyhow!("metric '{name}' mangler"))?
                .as_f64()
                .ok_or_else(|| anyhow!("metric '{name}' er ikke et tal"))
        };

        Ok(SensorReading {
            device_id,
            timestamp_ms: payload.timestamp.map(|t| t as i64),
            top_temp: get("top_temp")?,
            top_rh: get("top_rh")?,
            middle_temp: get("middle_temp")?,
            middle_rh: get("middle_rh")?,
            bottom_temp: get("bottom_temp")?,
            bottom_rh: get("bottom_rh")?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn metric(name: &str, v: f64) -> Metric {
        Metric {
            name: Some(name.into()),
            timestamp: None,
            datatype: Some(10),
            value: Some(metric::Value::DoubleValue(v)),
        }
    }

    fn sample_message(topic: &str) -> RawMessage {
        let p = Payload {
            timestamp: Some(1_762_000_000_000),
            seq: Some(1),
            metrics: vec![
                metric("top_temp", 26.2),
                metric("top_rh", 47.0),
                metric("middle_temp", 26.2),
                metric("middle_rh", 44.0),
                metric("bottom_temp", 26.7),
                metric("bottom_rh", 48.0),
            ],
        };
        RawMessage { topic: topic.into(), payload: p.encode_to_vec() }
    }

    #[test]
    fn decoder_gyldig_besked() {
        let r = SparkplugBDecoder::new()
            .decode(&sample_message("spBv1.0/lab/DDATA/esp32c6/plc1"))
            .unwrap();
        assert_eq!(r.device_id, "plc1");
        assert_eq!(r.top_temp, 26.2);
        assert_eq!(r.bottom_rh, 48.0);
        assert_eq!(r.timestamp_ms, Some(1_762_000_000_000));
    }

    #[test]
    fn decoder_afviser_forkert_topic() {
        assert!(SparkplugBDecoder::new()
            .decode(&sample_message("noget/andet"))
            .is_err());
    }

    #[test]
    fn decoder_afviser_manglende_metric() {
        let mut msg = sample_message("spBv1.0/lab/DDATA/esp32c6/plc1");
        let mut p = Payload::decode(msg.payload.as_slice()).unwrap();
        p.metrics.pop();
        msg.payload = p.encode_to_vec();
        assert!(SparkplugBDecoder::new().decode(&msg).is_err());
    }
}
