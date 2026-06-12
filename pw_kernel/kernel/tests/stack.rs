// Copyright 2025 The Pigweed Authors
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
    use core::mem::MaybeUninit;

    use kernel::scheduler::{Stack, StackStorage};
    use unittest::test;

    #[test]
    fn stack_initialize_fills_stack_with_pattern() -> unittest::Result<()> {
        const STACK_SIZE: usize = 1024;
        let storage = StackStorage::<STACK_SIZE> {
            stack: [MaybeUninit::uninit(); STACK_SIZE],
        };

        let stack = Stack::from_slice(&storage.stack);
        stack.initialize();

        let stack_slice = unsafe {
            core::slice::from_raw_parts(
                storage.stack.as_ptr().cast::<u32>(),
                STACK_SIZE / size_of::<u32>(),
            )
        };

        for &val in stack_slice.iter() {
            unittest::assert_eq!(val, magic_values::UNUSED_STACK_PATTERN);
        }

        Ok(())
    }
}
