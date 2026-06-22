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
}
