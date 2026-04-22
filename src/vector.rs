use crate::payload::Payload;
use time::{OffsetDateTime, format_description::well_known::Rfc3339};

const MAX_AMOUNT: f32 = 10_000.0;
const MAX_INSTALLMENTS: f32 = 12.0;
const AMOUNT_VS_AVG_RATIO: f32 = 10.0;
const MAX_MINUTES: f32 = 1_440.0;
const MAX_KM: f32 = 1_000.0;
const MAX_TX_COUNT_24H: f32 = 20.0;
const MAX_MERCHANT_AVG_AMOUNT: f32 = 10_000.0;

pub fn vectorize(payload: &Payload) -> Result<[f32; 14], String> {
    let requested_at = parse_timestamp(&payload.transaction.requested_at)?;
    let (minutes_since_last_tx, km_from_last_tx) = match &payload.last_transaction {
        Some(last_transaction) => {
            let last_timestamp = parse_timestamp(&last_transaction.timestamp)?;
            let elapsed = (requested_at - last_timestamp).whole_minutes().max(0) as f32;
            (
                clamp01(elapsed / MAX_MINUTES),
                clamp01(last_transaction.km_from_current / MAX_KM),
            )
        }
        None => (-1.0, -1.0),
    };

    Ok([
        clamp01(payload.transaction.amount / MAX_AMOUNT),
        clamp01(payload.transaction.installments as f32 / MAX_INSTALLMENTS),
        clamp01(amount_vs_avg(payload.transaction.amount, payload.customer.avg_amount)),
        requested_at.hour() as f32 / 23.0,
        requested_at.weekday().number_days_from_monday() as f32 / 6.0,
        minutes_since_last_tx,
        km_from_last_tx,
        clamp01(payload.terminal.km_from_home / MAX_KM),
        clamp01(payload.customer.tx_count_24h as f32 / MAX_TX_COUNT_24H),
        bool_to_f32(payload.terminal.is_online),
        bool_to_f32(payload.terminal.card_present),
        bool_to_f32(!is_known_merchant(payload)),
        mcc_risk(&payload.merchant.mcc),
        clamp01(payload.merchant.avg_amount / MAX_MERCHANT_AVG_AMOUNT),
    ])
}

fn parse_timestamp(value: &str) -> Result<OffsetDateTime, String> {
    OffsetDateTime::parse(value, &Rfc3339)
        .map_err(|error| format!("timestamp inválido {value}: {error}"))
}

fn amount_vs_avg(amount: f32, avg_amount: f32) -> f32 {
    if avg_amount <= 0.0 {
        return if amount <= 0.0 { 0.0 } else { 1.0 };
    }

    (amount / avg_amount) / AMOUNT_VS_AVG_RATIO
}

fn is_known_merchant(payload: &Payload) -> bool {
    payload
        .customer
        .known_merchants
        .iter()
        .any(|merchant_id| merchant_id == &payload.merchant.id)
}

fn mcc_risk(mcc: &str) -> f32 {
    match mcc {
        "5411" => 0.15,
        "5812" => 0.30,
        "5912" => 0.20,
        "5944" => 0.45,
        "7801" => 0.80,
        "7802" => 0.75,
        "7995" => 0.85,
        "4511" => 0.35,
        "5311" => 0.25,
        "5999" => 0.50,
        _ => 0.50,
    }
}

fn bool_to_f32(value: bool) -> f32 {
    if value { 1.0 } else { 0.0 }
}

fn clamp01(value: f32) -> f32 {
    value.clamp(0.0, 1.0)
}
