// End-to-end check for iceoryx2 #2163: cost of the first write (publisher)
// and first read (subscriber) of large samples, with and without prefaulting.
// Prefault mode is selected by the IOX2_PREFAULT env var in the prototype patch.
use iceoryx2::prelude::*;
use std::time::Instant;

fn minflt() -> i64 {
    let mut r: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut r) };
    r.ru_minflt as i64
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let size_mb: usize = std::env::args().nth(1).unwrap_or("4".into()).parse()?;
    let k: usize = std::env::args().nth(2).unwrap_or("4".into()).parse()?;
    let rounds = 3;
    let size = size_mb << 20;
    let src = vec![0xabu8; size];
    let mode = std::env::var("IOX2_PREFAULT").unwrap_or("none".into());
    set_log_level(LogLevel::Error);

    let node = NodeBuilder::new().create::<ipc::Service>()?;
    let name = format!("prefault_bench_{}", std::process::id());
    let service = node
        .service_builder(&name.as_str().try_into()?)
        .publish_subscribe::<[u8]>()
        .subscriber_max_buffer_size(k)
        .subscriber_max_borrowed_samples(k)
        .history_size(0)
        .max_publishers(1)
        .max_subscribers(1)
        .create()?;
    let subscriber = service.subscriber_builder().create()?;

    let t = Instant::now();
    let publisher = service
        .publisher_builder()
        .initial_max_slice_len(size)
        .max_loaned_samples(k)
        .create()?;
    let pub_create_us = t.elapsed().as_micros();

    // round, side, sample idx, us, faults
    let mut rows: Vec<(usize, &str, usize, f64, i64)> = vec![];
    let mut sub_first_receive_us = 0.0;
    for round in 0..rounds {
        let mut loans = Vec::with_capacity(k);
        for _ in 0..k {
            loans.push(publisher.loan_slice_uninit(size)?);
        }
        for (i, s) in loans.into_iter().enumerate() {
            let f0 = minflt();
            let t = Instant::now();
            let s = s.write_from_slice(&src);
            let us = t.elapsed().as_secs_f64() * 1e6;
            rows.push((round, "pub_write", i, us, minflt() - f0));
            s.send()?;
        }
        let mut held = Vec::with_capacity(k);
        for i in 0..k {
            let t = Instant::now();
            let s = subscriber.receive()?.expect("sample");
            if round == 0 && i == 0 {
                sub_first_receive_us = t.elapsed().as_secs_f64() * 1e6;
            }
            let f0 = minflt();
            let t = Instant::now();
            let mut sum = 0u64;
            for b in s.payload().iter().step_by(64) {
                sum = sum.wrapping_add(*b as u64);
            }
            std::hint::black_box(sum);
            let us = t.elapsed().as_secs_f64() * 1e6;
            rows.push((round, "sub_read", i, us, minflt() - f0));
            held.push(s);
        }
        drop(held);
    }
    for side in ["pub_write", "sub_read"] {
        for round in 0..rounds {
            let r: Vec<_> = rows.iter().filter(|x| x.0 == round && x.1 == side).collect();
            let us: f64 = r.iter().map(|x| x.3).sum::<f64>() / r.len() as f64;
            let f: i64 = r.iter().map(|x| x.4).sum::<i64>() / r.len() as i64;
            println!("{mode},{size_mb},{side},{round},{us:.0},{f}");
        }
    }
    println!("{mode},{size_mb},pub_create,-,{pub_create_us},-");
    println!("{mode},{size_mb},sub_first_receive,-,{sub_first_receive_us:.0},-");
    Ok(())
}
