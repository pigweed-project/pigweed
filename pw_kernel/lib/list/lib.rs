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
#![no_std]
#![cfg_attr(test, no_main)]

use core::cmp::Ordering;
use core::marker::PhantomData;
use core::ptr::NonNull;

use foreign_box::ForeignBox;

pub mod unsafe_list;

pub type Iter<'a, T, A> = unsafe_list::Iter<'a, T, A>;

pub use unsafe_list::{Adapter, Link, UnsafeList};

pub struct ForeignList<T, A: Adapter<T>> {
    list: UnsafeList<T, A>,
}

// Safety:
// A given node can only be in a single [`ForeignList`] at a time. This list
// logically owns the node for the duration of its membership in the list. All
// mutation of the node's `Link` pointers are done while the node is in the
// list. There is no API to get a mutable reference to a node while it is in the
// list nor directly manipulate its membership or position in the list.
unsafe impl<T: Send, A: Adapter<T>> Send for ForeignList<T, A> {}
unsafe impl<T: Sync, A: Adapter<T>> Sync for ForeignList<T, A> {}

impl<T, A: Adapter<T>> Default for ForeignList<T, A> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, A: Adapter<T>> ForeignList<T, A> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            list: UnsafeList::new(),
        }
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        unsafe { self.list.is_empty() }
    }

    pub fn push_front(&mut self, element: ForeignBox<T>) {
        let element = element.consume();
        unsafe { self.list.push_front_unchecked(element) }
    }

    pub fn push_back(&mut self, element: ForeignBox<T>) {
        let element = element.consume();
        unsafe { self.list.push_back_unchecked(element) }
    }

    pub fn pop_head(&mut self) -> Option<ForeignBox<T>> {
        unsafe { self.list.pop_head().map(|element| ForeignBox::new(element)) }
    }

    pub fn for_each<E, F: FnMut(&T) -> Result<(), E>>(&self, callback: F) -> Result<(), E> {
        unsafe { self.list.for_each(callback) }
    }

    #[must_use]
    pub fn iter(&self) -> Iter<'_, T, A> {
        self.list.iter()
    }

    // Note: `iter_mut()` is intentionally not provided. Providing `&mut T` would
    // expose the internal `Link` field, allowing callers to corrupt the list's
    // structure or violate Rust's sharing rules (since intrusive elements are
    // often shared via raw pointers).

    /// # Safety
    /// Caller ensures the element is a valid pointer to an instance of T.
    pub unsafe fn remove_element(&mut self, element: NonNull<T>) -> Option<ForeignBox<T>> {
        unsafe {
            self.list
                .unlink_element(element)
                .map(|element| ForeignBox::new(element))
        }
    }
}

impl<T, A: Adapter<T>> Drop for ForeignList<T, A> {
    fn drop(&mut self) {
        if !self.is_empty() {
            pw_assert::panic!("ForeignList dropped while non-empty");
        }
    }
}

impl<T, A: Adapter<T>> ForeignList<T, A> {
    pub fn sorted_insert_by_key<F: FnMut(&T) -> K, K: Ord>(
        &mut self,
        element: ForeignBox<T>,
        mut f: F,
    ) {
        self.sorted_insert_by(element, move |a, b| f(a).cmp(&f(b)))
    }

    pub fn sorted_insert_by<F: FnMut(&T, &T) -> Ordering>(
        &mut self,
        element: ForeignBox<T>,
        compare: F,
    ) {
        let element = element.consume();
        unsafe { self.list.sorted_insert_by_unchecked(element, compare) }
    }
}

/// A key used to remove an element from a [`RandomAccessForeignList`]
pub struct RandomAccessKey<T, A: Adapter<T>> {
    // `new()` ensure that this is a valid, well-aligned, pointer to a `T`.
    ptr: NonNull<T>,
    _phantom: PhantomData<A>,
}

impl<T, A: Adapter<T>> RandomAccessKey<T, A> {
    /// # Safety
    /// Caller ensures the element is a valid pointer to an instance of T.
    unsafe fn new(element: &mut ForeignBox<T>) -> Self {
        Self {
            ptr: unsafe { NonNull::new_unchecked(element.as_mut_ptr()) },
            _phantom: PhantomData,
        }
    }
}

/// A variant of [`ForeignList`] that allows safely removal of arbitrary nodes
/// in exchange no pop operations.
///
/// When a node is added to a [`RandomAccessForeignList`], a [`RandomAccessKey`]
/// is returned.  The node may only be removed from this list by passing that
/// key to [`RandomAccessForeignList::remove_element()`].
///
/// # Example
/// ```
/// use core::ptr::NonNull;
///
/// use foreign_box::ForeignBox;
/// use list::{Link, RandomAccessForeignList, define_adapter};
///
/// struct Element {
///     value: u32,
///     link: Link,
/// }
///
/// define_adapter!(Adapter => Element::link);
///
/// let mut element1 = Element {
///     value: 1,
///     link: Link::new(),
/// };
/// let mut element2 = Element {
///     value: 2,
///     link: Link::new(),
/// };
/// let mut element3 = Element {
///     value: 3,
///     link: Link::new(),
/// };
///
/// let mut list = RandomAccessForeignList::<Element, Adapter>::new();
/// let key3 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
/// let key2 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
/// let key1 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });
///
/// let removed_element = list.remove_element(key2);
/// assert_eq!(removed_element.consume(), NonNull::new(&raw mut element2).unwrap());
///
/// let _ = list.remove_element(key1).consume();
/// let _ = list.remove_element(key3).consume();
/// ```
///
pub struct RandomAccessForeignList<T, A: Adapter<T>> {
    list: ForeignList<T, A>,
}

impl<T, A: Adapter<T>> RandomAccessForeignList<T, A> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            list: ForeignList::new(),
        }
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }

    #[must_use]
    pub fn push_front(&mut self, mut element: ForeignBox<T>) -> RandomAccessKey<T, A> {
        // SAFETY: Caller ensures the element is a valid pointer to an instance of T.
        let key = unsafe { RandomAccessKey::new(&mut element) };
        self.list.push_front(element);
        key
    }

    #[must_use]
    pub fn push_back(&mut self, mut element: ForeignBox<T>) -> RandomAccessKey<T, A> {
        // SAFETY: Caller ensures the element is a valid pointer to an instance of T.
        let key = unsafe { RandomAccessKey::new(&mut element) };
        self.list.push_back(element);
        key
    }

    pub fn for_each<E, F: FnMut(&T) -> Result<(), E>>(&self, callback: F) -> Result<(), E> {
        self.list.for_each(callback)
    }

    #[must_use]
    pub fn iter(&self) -> Iter<'_, T, A> {
        self.list.iter()
    }

    // Note: `iter_mut()` is intentionally not provided. Providing `&mut T` would
    // expose the internal `Link` field, allowing callers to corrupt the list's
    // structure or violate Rust's sharing rules (since intrusive elements are
    // often shared via raw pointers).

    #[must_use]
    pub fn remove_element(&mut self, key: RandomAccessKey<T, A>) -> ForeignBox<T> {
        // SAFETY: Caller ensures the element is a valid pointer to an instance of T.
        unsafe { self.list.remove_element(key.ptr).unwrap_unchecked() }
    }
}

#[cfg(test)]
mod tests {
    use core::ptr::NonNull;

    use foreign_box::ForeignBox;
    use unittest::test;

    use super::*;

    // `#[repr(C)]` is used to ensure that `link` is at a non-zero offset.
    // Previously, without this, the compiler was putting link at the beginning of
    // the struct causing `LINK_OFFSET` in the adapter to be zero which obfuscated
    // some pointer math bugs.
    #[repr(C)]
    struct TestMember {
        value: u32,
        link: Link,
    }

    impl TestMember {
        fn get_key(&self) -> u32 {
            self.value
        }
    }

    define_adapter!(TestAdapter => TestMember::link);

    fn validate_list(
        list: &ForeignList<TestMember, TestAdapter>,
        expected_values: &[u32],
    ) -> unittest::Result<()> {
        let mut index = 0;
        list.for_each(|element| {
            unittest::assert_eq!(element.value, expected_values[index]);
            index += 1;
            Ok(())
        })?;

        unittest::assert_eq!(index, expected_values.len());
        Ok(())
    }

    fn drain_list(list: &mut ForeignList<TestMember, TestAdapter>) {
        while let Some(element) = list.pop_head() {
            element.consume();
        }
    }

    #[test]
    fn new_list_is_empty() -> unittest::Result<()> {
        let list = ForeignList::<TestMember, TestAdapter>::new();
        unittest::assert_true!(list.is_empty());
        Ok(())
    }

    #[test]
    fn iter_traverses_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });
        list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });

        let mut index = 0;
        let expected = [1, 2];
        for element in list.iter() {
            unittest::assert_eq!(element.value, expected[index]);
            index += 1;
        }
        unittest::assert_eq!(index, 2);

        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn push_front_adds_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        unittest::assert_false!(list.is_empty());

        validate_list(&list, &[1, 2])?;

        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn push_back_adds_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        unittest::assert_false!(list.is_empty());

        validate_list(&list, &[2, 1])?;

        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn pop_head_removes_correctly() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });

        let e = list.pop_head();
        unittest::assert_true!(e.is_some());
        let e = e.unwrap();
        unittest::assert_eq!(e.value, 3);
        unittest::assert_true!(e.link.is_unlinked());
        e.consume();

        let e = list.pop_head();
        unittest::assert_true!(e.is_some());
        let e = e.unwrap();
        unittest::assert_eq!(e.value, 2);
        unittest::assert_true!(e.link.is_unlinked());
        e.consume();

        let e = list.pop_head();
        unittest::assert_true!(e.is_some());
        let e = e.unwrap();
        unittest::assert_eq!(e.value, 1);
        unittest::assert_true!(e.link.is_unlinked());
        e.consume();

        validate_list(&list, &[])
    }

    #[test]
    fn remove_element_can_remove_head() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element =
            unsafe { list.remove_element(NonNull::new(&raw mut element1).unwrap()) };
        unittest::assert_true!(removed_element.is_some());
        unittest::assert_eq!(
            removed_element.unwrap().consume(),
            NonNull::new(&raw mut element1).unwrap()
        );

        validate_list(&list, &[2, 3])?;
        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn remove_element_can_remove_middle() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element =
            unsafe { list.remove_element(NonNull::new(&raw mut element2).unwrap()) };
        unittest::assert_true!(removed_element.is_some());
        unittest::assert_eq!(
            removed_element.unwrap().consume(),
            NonNull::new(&raw mut element2).unwrap()
        );

        validate_list(&list, &[1, 3])?;
        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn remove_element_can_remove_tail() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element =
            unsafe { list.remove_element(NonNull::new(&raw mut element3).unwrap()) };
        unittest::assert_true!(removed_element.is_some());
        unittest::assert_eq!(
            removed_element.unwrap().consume(),
            NonNull::new(&raw mut element3).unwrap()
        );

        validate_list(&list, &[1, 2])?;
        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn sorted_insert_inserts_sorted_items_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element3) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element2) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element1) },
            TestMember::get_key,
        );
        validate_list(&list, &[1, 2, 3])?;
        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn sorted_insert_inserts_reverse_sorted_items_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element1) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element2) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element3) },
            TestMember::get_key,
        );
        validate_list(&list, &[1, 2, 3])?;
        drain_list(&mut list);
        Ok(())
    }

    #[test]
    fn sorted_insert_inserts_unsorted_items_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element2_2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = ForeignList::<TestMember, TestAdapter>::new();
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element2) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element1) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element3) },
            TestMember::get_key,
        );
        list.sorted_insert_by_key(
            unsafe { ForeignBox::new_from_ptr(&raw mut element2_2) },
            TestMember::get_key,
        );
        validate_list(&list, &[1, 2, 2, 3])?;
        drain_list(&mut list);
        Ok(())
    }

    fn validate_random_access_list(
        list: &RandomAccessForeignList<TestMember, TestAdapter>,
        expected_values: &[u32],
    ) -> unittest::Result<()> {
        let mut index = 0;
        list.for_each(|element| {
            unittest::assert_eq!(element.value, expected_values[index]);
            index += 1;
            Ok(())
        })?;

        unittest::assert_eq!(index, expected_values.len());
        Ok(())
    }

    #[test]
    fn new_random_access_list_is_empty() -> unittest::Result<()> {
        let list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        unittest::assert_true!(list.is_empty());
        Ok(())
    }

    #[test]
    fn random_access_push_front_adds_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };

        let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        let key2 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        let key1 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        unittest::assert_false!(list.is_empty());

        validate_random_access_list(&list, &[1, 2])?;

        for key in [key1, key2] {
            let _ = list.remove_element(key).consume();
        }
        Ok(())
    }

    #[test]
    fn random_access_push_back_adds_in_correct_order() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };

        let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        let key2 = list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        let key1 = list.push_back(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        unittest::assert_false!(list.is_empty());

        validate_random_access_list(&list, &[2, 1])?;

        for key in [key1, key2] {
            let _ = list.remove_element(key).consume();
        }
        Ok(())
    }

    #[test]
    fn random_access_remove_element_can_remove_head() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        let key3 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        let key2 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        let key1 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element = list.remove_element(key1);
        unittest::assert_eq!(
            removed_element.consume(),
            NonNull::new(&raw mut element1).unwrap()
        );

        validate_random_access_list(&list, &[2, 3])?;
        for key in [key2, key3] {
            let _ = list.remove_element(key).consume();
        }
        Ok(())
    }

    #[test]
    fn random_access_remove_element_can_remove_middle() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        let key3 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        let key2 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        let key1 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element = list.remove_element(key2);
        unittest::assert_eq!(
            removed_element.consume(),
            NonNull::new(&raw mut element2).unwrap()
        );

        validate_random_access_list(&list, &[1, 3])?;
        for key in [key1, key3] {
            let _ = list.remove_element(key).consume();
        }
        Ok(())
    }

    #[test]
    fn random_access_remove_element_can_remove_tail() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };
        let mut element2 = TestMember {
            value: 2,
            link: Link::new(),
        };
        let mut element3 = TestMember {
            value: 3,
            link: Link::new(),
        };

        let mut list = RandomAccessForeignList::<TestMember, TestAdapter>::new();
        let key3 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element3) });
        let key2 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element2) });
        let key1 = list.push_front(unsafe { ForeignBox::new_from_ptr(&raw mut element1) });

        let removed_element = list.remove_element(key3);
        unittest::assert_eq!(
            removed_element.consume(),
            NonNull::new(&raw mut element3).unwrap()
        );

        validate_random_access_list(&list, &[1, 2])?;
        for key in [key1, key2] {
            let _ = list.remove_element(key).consume();
        }
        Ok(())
    }
}
