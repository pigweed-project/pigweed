// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#![no_std]
#![no_main]

use boot as _;
use pw_boot::entry;
use pw_sync_mutex::SystemTimedMutex;
use pw_time::{Clock, Duration, Instant, SystemClock};

#[entry]
fn entry() -> ! {
    pw_log::info!("Rust mutex example");

    let counter_mutex = SystemTimedMutex::new(0);

    // --- 1. Non-blocking try_lock() ---
    // Test success (when unlocked)
    if let Some(mut counter) = counter_mutex.try_lock() {
        *counter += 1;
        pw_log::info!("try_lock() succeeded as expected, counter = {}", *counter);
    } else {
        pw_log::error!("try_lock() failed unexpectedly!");
    }

    // Test failure (while locked)
    {
        let _guard = counter_mutex.lock();
        if counter_mutex.try_lock().is_none() {
            pw_log::info!("try_lock() failed while locked as expected");
        } else {
            pw_log::error!("try_lock() unexpectedly succeeded while locked!");
        }
    }

    // --- 2. Timed try_lock_for() ---
    // Test success (when unlocked)
    if let Some(mut counter) = counter_mutex.try_lock_for(Duration::from_millis(50)) {
        *counter += 1;
        pw_log::info!(
            "try_lock_for() succeeded as expected, counter = {}",
            *counter
        );
    } else {
        pw_log::error!("try_lock_for() failed unexpectedly!");
    }

    // Test failure / timeout (while locked)
    {
        let _guard = counter_mutex.lock();
        let start = SystemClock::now();
        if counter_mutex
            .try_lock_for(Duration::from_millis(50))
            .is_none()
        {
            let elapsed_ms = (SystemClock::now() - start).as_millis();
            pw_log::info!(
                "try_lock_for() timed out while locked as expected (elapsed: {} ms)",
                elapsed_ms
            );
        } else {
            pw_log::error!("try_lock_for() unexpectedly succeeded while locked!");
        }
    }

    // --- 3. Timed try_lock_until() ---
    // Test success (when unlocked)
    let deadline = SystemClock::now() + Duration::from_millis(50);
    if let Some(mut counter) = counter_mutex.try_lock_until(deadline) {
        *counter += 1;
        pw_log::info!(
            "try_lock_until() succeeded as expected, counter = {}",
            *counter
        );
    } else {
        pw_log::error!("try_lock_until() failed unexpectedly!");
    }

    // Test failure / timeout (while locked)
    {
        let _guard = counter_mutex.lock();
        let deadline = SystemClock::now() + Duration::from_millis(50);
        let start = SystemClock::now();
        if counter_mutex.try_lock_until(deadline).is_none() {
            let elapsed_ms = (SystemClock::now() - start).as_millis();
            pw_log::info!(
                "try_lock_until() timed out while locked as expected (elapsed: {} ms)",
                elapsed_ms
            );
        } else {
            pw_log::error!("try_lock_until() unexpectedly succeeded while locked!");
        }
    }

    for _ in 0..10 {
        {
            let mut counter = counter_mutex.lock();
            *counter += 1;
            pw_log::info!("lock(): counter = {}", *counter);
        }
    }

    pw_log::info!("Example done.  Sleeping.");
    loop {
        pw_thread::sleep_until(Instant::MAX);
    }
}
