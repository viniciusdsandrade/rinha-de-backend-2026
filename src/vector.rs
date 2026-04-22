use crate::payload::Payload;

const MAX_AMOUNT: f32 = 10_000.0;
const MAX_INSTALLMENTS: f32 = 12.0;
const AMOUNT_VS_AVG_RATIO: f32 = 10.0;
const MAX_MINUTES: f32 = 1_440.0;
const MAX_KM: f32 = 1_000.0;
const MAX_TX_COUNT_24H: f32 = 20.0;
const MAX_MERCHANT_AVG_AMOUNT: f32 = 10_000.0;
const SECONDS_PER_MINUTE: i64 = 60;
const SECONDS_PER_DAY: i64 = 86_400;

pub fn vectorize(payload: &Payload) -> Result<[f32; 14], String> {
    let requested_at = parse_timestamp(&payload.transaction.requested_at)?;
    let (minutes_since_last_tx, km_from_last_tx) = match &payload.last_transaction {
        Some(last_transaction) => {
            let last_timestamp = parse_timestamp(&last_transaction.timestamp)?;
            let elapsed = ((requested_at.total_seconds - last_timestamp.total_seconds)
                / SECONDS_PER_MINUTE)
                .max(0) as f32;
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
        clamp01(amount_vs_avg(
            payload.transaction.amount,
            payload.customer.avg_amount,
        )),
        requested_at.hour as f32 / 23.0,
        requested_at.weekday_monday0 as f32 / 6.0,
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

fn parse_timestamp(value: &str) -> Result<ParsedTimestamp, String> {
    let bytes = value.as_bytes();
    if bytes.len() != 20
        || bytes[4] != b'-'
        || bytes[7] != b'-'
        || bytes[10] != b'T'
        || bytes[13] != b':'
        || bytes[16] != b':'
        || bytes[19] != b'Z'
    {
        return Err(format!(
            "timestamp inválido {value}: formato RFC3339 não suportado"
        ));
    }

    let year = parse_digits(&bytes[0..4])? as i32;
    let month = parse_digits(&bytes[5..7])? as u8;
    let day = parse_digits(&bytes[8..10])? as u8;
    let hour = parse_digits(&bytes[11..13])? as u8;
    let minute = parse_digits(&bytes[14..16])? as u8;
    let second = parse_digits(&bytes[17..19])? as u8;

    if !(1..=12).contains(&month) {
        return Err(format!("timestamp inválido {value}: mês fora do intervalo"));
    }

    let max_day = days_in_month(year, month);
    if day == 0 || day > max_day {
        return Err(format!("timestamp inválido {value}: dia fora do intervalo"));
    }

    if hour > 23 || minute > 59 || second > 59 {
        return Err(format!(
            "timestamp inválido {value}: horário fora do intervalo"
        ));
    }

    let days_since_unix_epoch = days_from_civil(year, month, day);
    let total_seconds = (days_since_unix_epoch * SECONDS_PER_DAY)
        + (hour as i64 * 3_600)
        + (minute as i64 * SECONDS_PER_MINUTE)
        + second as i64;
    let weekday_monday0 = (days_since_unix_epoch + 3).rem_euclid(7) as u8;

    Ok(ParsedTimestamp {
        total_seconds,
        hour,
        weekday_monday0,
    })
}

fn parse_digits(bytes: &[u8]) -> Result<u32, String> {
    let mut value = 0u32;

    for byte in bytes {
        if !byte.is_ascii_digit() {
            return Err(format!("caractere inválido {}", *byte as char));
        }

        value = (value * 10) + u32::from(byte - b'0');
    }

    Ok(value)
}

fn days_in_month(year: i32, month: u8) -> u8 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if is_leap_year(year) => 29,
        2 => 28,
        _ => unreachable!("mês já validado"),
    }
}

fn is_leap_year(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)
}

fn days_from_civil(year: i32, month: u8, day: u8) -> i64 {
    let adjusted_year = year - i32::from(month <= 2);
    let era = if adjusted_year >= 0 {
        adjusted_year / 400
    } else {
        (adjusted_year - 399) / 400
    };
    let year_of_era = adjusted_year - (era * 400);
    let shifted_month = i32::from(month) + if month > 2 { -3 } else { 9 };
    let day_of_year = ((153 * shifted_month) + 2) / 5 + i32::from(day) - 1;
    let day_of_era = (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;

    i64::from((era * 146_097) + day_of_era - 719_468)
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

#[inline(always)]
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

#[inline(always)]
fn bool_to_f32(value: bool) -> f32 {
    if value { 1.0 } else { 0.0 }
}

#[inline(always)]
fn clamp01(value: f32) -> f32 {
    value.clamp(0.0, 1.0)
}

#[derive(Debug, Clone, Copy)]
struct ParsedTimestamp {
    total_seconds: i64,
    hour: u8,
    weekday_monday0: u8,
}

#[cfg(test)]
mod tests {
    use super::vectorize;
    use crate::payload::{Customer, LastTransaction, Merchant, Payload, Terminal, Transaction};

    #[test]
    fn vectorizes_last_transaction_across_midnight() {
        let payload = Payload {
            id: "tx-test".to_string(),
            transaction: Transaction {
                amount: 100.0,
                installments: 1,
                requested_at: "2026-03-11T00:02:00Z".to_string(),
            },
            customer: Customer {
                avg_amount: 100.0,
                tx_count_24h: 0,
                known_merchants: vec!["merchant-1".to_string()],
            },
            merchant: Merchant {
                id: "merchant-1".to_string(),
                mcc: "5411".to_string(),
                avg_amount: 100.0,
            },
            terminal: Terminal {
                is_online: false,
                card_present: true,
                km_from_home: 0.0,
            },
            last_transaction: Some(LastTransaction {
                timestamp: "2026-03-10T23:58:30Z".to_string(),
                km_from_current: 25.0,
            }),
        };

        let actual = vectorize(&payload).unwrap();

        assert!(
            (actual[3] - 0.0).abs() <= 0.0001,
            "hora incorreta: {}",
            actual[3]
        );
        assert!(
            (actual[4] - (2.0 / 6.0)).abs() <= 0.0001,
            "weekday incorreto: {}",
            actual[4]
        );
        assert!(
            (actual[5] - (3.0 / 1_440.0)).abs() <= 0.0001,
            "minutos desde última transação incorretos: {}",
            actual[5]
        );
        assert!(
            (actual[6] - 0.025).abs() <= 0.0001,
            "distância da última transação incorreta: {}",
            actual[6]
        );
    }

    #[test]
    fn rejects_invalid_calendar_dates() {
        let payload = Payload {
            id: "tx-test".to_string(),
            transaction: Transaction {
                amount: 100.0,
                installments: 1,
                requested_at: "2026-02-30T12:00:00Z".to_string(),
            },
            customer: Customer {
                avg_amount: 100.0,
                tx_count_24h: 0,
                known_merchants: vec![],
            },
            merchant: Merchant {
                id: "merchant-1".to_string(),
                mcc: "5411".to_string(),
                avg_amount: 100.0,
            },
            terminal: Terminal {
                is_online: false,
                card_present: true,
                km_from_home: 0.0,
            },
            last_transaction: None,
        };

        let error = vectorize(&payload).unwrap_err();
        assert!(
            error.contains("dia fora do intervalo"),
            "erro inesperado: {error}"
        );
    }
}
