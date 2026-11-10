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

#[cfg(test)]
mod tests {
    use pw_sync_spinlock::InterruptSpinLock;

    #[test]
    fn test_lock_unlock() {
        let lock = InterruptSpinLock::new(42);
        {
            let mut guard = lock.lock();
            assert_eq!(*guard, 42);
            *guard = 43;
        }
        {
            let guard = lock.lock();
            assert_eq!(*guard, 43);
        }
    }

    #[test]
    fn test_try_lock() {
        let lock = InterruptSpinLock::new(10);
        let guard1 = lock.try_lock();
        assert!(guard1.is_some());

        let guard2 = lock.try_lock();
        assert!(guard2.is_none());

        drop(guard1);

        let guard3 = lock.try_lock();
        assert!(guard3.is_some());
    }

    #[test]
    fn test_with_mutates_data() {
        let lock = InterruptSpinLock::new(42);
        lock.with(|data| {
            assert_eq!(*data, 42);
            *data = 43;
        });

        let guard = lock.lock();
        assert_eq!(*guard, 43);
    }

    #[test]
    fn test_with_returns_closure_value() {
        let lock = InterruptSpinLock::new(7);
        let doubled = lock.with(|data| *data * 2);
        assert_eq!(doubled, 14);

        let message = lock.with(|data| if *data == 7 { "seven" } else { "other" });
        assert_eq!(message, "seven");
    }

    #[test]
    fn test_with_releases_lock_on_return() {
        let lock = InterruptSpinLock::new(0);
        lock.with(|data| *data += 1);

        let guard = lock.try_lock();
        assert!(guard.is_some());
    }

    #[test]
    fn test_try_with_mutates_data() {
        let lock = InterruptSpinLock::new(42);
        let result = lock.try_with(|data| {
            assert_eq!(*data, 42);
            *data = 43;
            "modified"
        });
        assert_eq!(result, Some("modified"));

        let guard = lock.lock();
        assert_eq!(*guard, 43);
    }

    #[test]
    fn test_try_with_when_locked_returns_none() {
        let lock = InterruptSpinLock::new(10);
        let guard = lock.lock();

        let mut closure_ran = false;
        let result = lock.try_with(|data| {
            closure_ran = true;
            *data += 1;
        });

        assert_eq!(result, None);
        assert!(!closure_ran);

        drop(guard);
        assert_eq!(*lock.lock(), 10);
    }

    #[test]
    fn test_try_with_releases_lock_on_return() {
        let lock = InterruptSpinLock::new(0);
        let result = lock.try_with(|data| *data += 1);
        assert_eq!(result, Some(()));

        let guard = lock.try_lock();
        assert!(guard.is_some());
    }
}
