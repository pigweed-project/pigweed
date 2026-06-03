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

use foreign_box::ForeignBox;
use list::{ForeignList, Link, RandomAccessForeignList, define_adapter};

// `#[repr(C)]` is used to ensure that `link` is at a non-zero offset.
#[repr(C)]
struct TestMember {
    value: u32,
    link: Link,
}

define_adapter!(TestAdapter => TestMember::link);

#[should_panic]
#[test]
fn foreign_list_panics_when_dropped_non_empty() {
    let mut element = core::mem::ManuallyDrop::new(TestMember {
        value: 1,
        link: Link::new(),
    });
    let mut list = ForeignList::<TestMember, TestAdapter>::new();
    list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut *element) });
}

#[should_panic]
#[test]
fn random_access_foreign_list_panics_when_dropped_non_empty() {
    let mut element = core::mem::ManuallyDrop::new(TestMember {
        value: 1,
        link: Link::new(),
    });
    let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
    let _key = list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut *element) });
}
