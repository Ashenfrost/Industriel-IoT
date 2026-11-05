//! Styring af programmet. Her - og kun her - vælger vi, hvilke moduler der bruges.
//!
//!   MQTT-broker -> [mqtt_subscriber] -> kanal -> [Decoder] -> [Writer] -> QuestDB
//!
//! Vil I skifte decoder eller database, er det KUN de to linjer markeret "VALG" nedenfor,
//! der skal ændres.

mod model;
mod mqtt_subscriber;
mod questdb_writer;
mod sparkplug_b_decoder;

use std::env;
use std::sync::mpsc;
use std::thread;

use anyhow::Result;

use model::{Decoder, RawMessage, Writer};
use mqtt_subscriber::MqttConfig;
use questdb_writer::QuestDbWriter;
use sparkplug_b_decoder::SparkplugBDecoder;

/// Læs en indstilling fra miljøvariabel, ellers brug standardværdi.
fn setting(name: &str, default: &str) -> String {
    env::var(name).unwrap_or_else(|_| default.to_string())
}

fn main() -> Result<()> {
    // --- Konfiguration (kan ændres uden at kompilere om) ---
    let mqtt_cfg = MqttConfig {
        host: setting("MQTT_HOST", "localhost"),
        port: setting("MQTT_PORT", "1883").parse()?,
        client_id: setting("MQTT_CLIENT_ID", "host_app"),
        topic: setting("MQTT_TOPIC", "spBv1.0/#"),
        username: env::var("MQTT_USER").ok(),
        password: env::var("MQTT_PASS").ok(),
    };
    let questdb_conf = setting("QUESTDB_CONF", "tcp::addr=localhost:9009;");
    let use_device_ts = setting("USE_DEVICE_TIMESTAMP", "false") == "true";

    // --- VALG: hvilke moduler bruger vi? ---
    let decoder: Box<dyn Decoder> = Box::new(SparkplugBDecoder::new()); // VALG 1
    let mut writer: Box<dyn Writer> = Box::new(QuestDbWriter::new(&questdb_conf, use_device_ts)?); // VALG 2

    // --- Start MQTT i egen tråd, modtag beskeder her i main ---
    let (tx, rx) = mpsc::channel::<RawMessage>();
    thread::spawn(move || {
        if let Err(e) = mqtt_subscriber::run(mqtt_cfg, tx) {
            eprintln!("[mqtt] stoppede: {e:#}");
        }
    });

    println!("[main] klar - venter på beskeder");
    for msg in rx {
        match decoder.decode(&msg) {
            Ok(reading) => match writer.write(&reading) {
                Ok(()) => println!("[main] gemt: {reading:?}"),
                Err(e) => eprintln!("[main] kunne ikke gemme: {e:#}"),
            },
            Err(e) => eprintln!("[main] droppede besked på '{}': {e:#}", msg.topic),
        }
    }
    Ok(())
}
