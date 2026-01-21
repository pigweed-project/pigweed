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
#![allow(dead_code)]
use core::cmp::Ordering;
use core::marker::PhantomData;
use core::ptr::NonNull;

// Intrusive link structures are particularly tricky in Rust because mutable
// references are expected to be globally unique.  Accessing the data through
// other methods is UB.  There is a good writeup of Tokio's soundness challenges
// at: https://gist.github.com/Darksonn/1567538f56af1a8038ecc3c664a42462.
//
// Here we adapt the strategy that Tokio uses:
// * The link pointer structure is marked with `PhantomPinned`.  This does two
//   things.  First it "poisons" the containing structure so that it's
//   unpinnable (immovable).  Second it triggers the heuristic from
//   https://github.com/rust-lang/rust/pull/82834 which will cause the compiler
//   to omit emitting a `noalias` annotation on mutable references to disable
//   compiler optimization that assume the mutable reference is unique.
// * `next` and `prev` themselves are accessed by `Link` through direct pointer
//   math on the `LinkInner` struct.  This avoids creating mutable references to
//   those fields themselves which can not be annotated with `PhantomPinned`.
//   `LinkInner` is `#[repr(C)]` to make the pointer math deterministic.
//   `LinkInner` is declared in an `inner` module so that `next` and `prev` can
//   not be accessed directly be the rest of the code.
//
// TODO: konkers - Understand if we need to annotate the alignment of LinkInner.
mod link {
    use core::cell::Cell;
    use core::marker::PhantomPinned;
    use core::ptr::NonNull;

    #[repr(C)]
    pub struct Link {
        // TODO: The use of `Cell` here is *explicitly* unsound in combination
        // with the `Send` and `Sync` impls below!
        next: Cell<Option<NonNull<Link>>>,
        prev: Cell<Option<NonNull<Link>>>,
        _pin: PhantomPinned,
    }

    impl Link {
        const UNLINKED_VALUE: Option<NonNull<Link>> =
            Some(NonNull::new(usize::MAX as *mut Link).unwrap());

        #[must_use]
        pub const fn new() -> Self {
            Self {
                next: Cell::new(Self::UNLINKED_VALUE),
                prev: Cell::new(Self::UNLINKED_VALUE),
                _pin: PhantomPinned,
            }
        }

        pub(super) const fn get_next(&self) -> Option<NonNull<Link>> {
            self.next.get()
        }

        pub(super) const fn get_prev(&self) -> Option<NonNull<Link>> {
            self.prev.get()
        }

        pub(super) fn set_next(&self, next: Option<NonNull<Link>>) {
            self.next.set(next);
        }

        pub(super) fn set_prev(&self, prev: Option<NonNull<Link>>) {
            self.prev.set(prev);
        }

        #[inline]
        #[must_use]
        pub fn is_unlinked(&self) -> bool {
            self.get_next() == Link::UNLINKED_VALUE && self.get_prev() == Link::UNLINKED_VALUE
        }

        #[inline]
        #[must_use]
        pub fn is_linked(&self) -> bool {
            !self.is_unlinked()
        }

        pub(super) fn set_unlinked(&self) {
            self.set_next(Link::UNLINKED_VALUE);
            self.set_prev(Link::UNLINKED_VALUE);
        }
    }
}
pub use link::Link;

// SAFETY:
//
// When used with a ForeignList (or RandomAccessForeignList), a given node can
// only be in a single list at a time.  This list logically owns the node for
// the duration of its membership in the list.  All mutation of the node's
// `Link` pointers are done while the node is in the list.  There is no API to
// get a mutable reference to a node while it is in the list nor directly
// manipulate its membership or position in the list.
//
// When used with an `UnsafeList` it is the users responsibility to ensure that
// Rust's read-shared/write-exclusive semantics are upheld across all access
// to a given node including when that node is also in a ForeignList
// RandomAccessForeignList.
unsafe impl Send for Link {}
unsafe impl Sync for Link {}

impl Default for Link {
    fn default() -> Self {
        Self::new()
    }
}

// `UnsafeList` uses None to mark the beginning and end of lists as opposed to
// pointers to the base list node.  This means that there are never pointers to
// `UnsafeList` and the same care is not needed to avoid mutable references as
// is taken with the `Link` structure.
pub struct UnsafeList<T, A: Adapter<T>> {
    inner: UnsafeListInner,
    _phantom_type: PhantomData<T>,
    _phantom_adapter: PhantomData<A>,
}

// We separate this out so that most logic can be implemented as methods on this
// type. These methods are concrete (with no generic type parameters), and
// equivalent, generic methods on `UnsafeList` call into them. By keeping most
// logic in concrete functions, we cut down on monomorphization size bloat.
struct UnsafeListInner {
    head: Option<NonNull<Link>>,
    tail: Option<NonNull<Link>>,
}

pub trait Adapter<T> {
    #[doc(hidden)]
    const LINK_OFFSET: usize;

    #[doc(hidden)]
    #[must_use]
    unsafe fn get_link(element: NonNull<T>) -> NonNull<Link>;
    #[doc(hidden)]
    #[must_use]
    #[inline(always)]
    unsafe fn get_element(link: NonNull<Link>) -> NonNull<T> {
        unsafe { link.byte_sub(Self::LINK_OFFSET).cast::<T>() }
    }
}

/// Defines an adapter type and implements [`Adapter`] for it.
///
/// # Usage
///
/// `define_adapter!` accepts a syntax like the following (where type arguments
/// are optional):
///
/// ```
/// use list::{Link, define_adapter};
///
/// struct Node<T: Copy> {
///     data: T,
///     link: Link,
/// }
/// define_adapter!(pub Adapter<T: Copy> => Node<T>::link);
/// ```
#[macro_export]
macro_rules! define_adapter {
    ($vis:vis $name:ident $(<$($tyvar:ident $(: $bound:path)?),*>)? => $node:ident $(<$($node_tyvar:ident),*>)? :: $link:ident) => {
        $vis struct $name $(<$($tyvar $(: $bound)?),*>)? {
            $(
                _marker: core::marker::PhantomData<$($tyvar),*>,
            )?
        }

        const _: () = {
            use core::ptr::NonNull;
            use core::mem::offset_of;

            impl $(<$($tyvar $(: $bound)?),*>)? $crate::Adapter<$node $(<$($node_tyvar),*>)?> for $name $(<$($tyvar),*>)? {
                const LINK_OFFSET: usize = offset_of!($node $(<$($node_tyvar),*>)?, $link);

                #[inline(always)]
                unsafe fn get_link(element: NonNull<$node $(<$($node_tyvar),*>)?>) -> NonNull<$crate::Link> {
                    // NOTE: There is nothing stopping a caller from providing a
                    // `$link` field which is not of type `Link`. However, if
                    // they do this, then this method will fail to compile since
                    // we annotate `link: *mut Link`. That ensures that `$link:
                    // Link`.
                    //
                    // Note that a previous version of this code didn't have an
                    // `Adapter::get_link` function, and relied on `LINK_OFFSET`
                    // for both element -> link and link -> element pointer
                    // operations. That code was vulnerable to this bug.
                    let link: *mut $crate::Link = unsafe { core::ptr::addr_of_mut!((*element.as_ptr()).$link) };
                    unsafe { NonNull::new_unchecked(link) }
                }
            }
        };
    };
}

impl<T, A: Adapter<T>> UnsafeList<T, A> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            inner: UnsafeListInner::new(),
            _phantom_type: PhantomData,
            _phantom_adapter: PhantomData,
        }
    }

    /// # Safety
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    #[must_use]
    pub unsafe fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }

    /// Returns true if element is in **ANY** list that uses this list's adapter.
    ///
    /// # Safety
    /// `element` must be a valid, non-null pointer.
    #[must_use]
    pub unsafe fn is_element_linked(&self, element: NonNull<T>) -> bool {
        unsafe { A::get_link(element).as_ref().is_linked() }
    }

    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn push_front_unchecked(&mut self, element: NonNull<T>) {
        let link_ptr = unsafe { A::get_link(element) };
        unsafe { self.inner.push_front_unchecked(link_ptr) };
    }

    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn push_back_unchecked(&mut self, element: NonNull<T>) {
        let link_ptr = unsafe { A::get_link(element) };
        unsafe { self.inner.push_back_unchecked(link_ptr) };
    }

    /// Insert element_a into the list immediately before `element_b`
    ///
    /// # Safety
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    /// It is up to the caller to ensure that element_a is not in a list.
    /// It is up to the caller to ensure that element_b is in a list.
    /// It is up to the caller to ensure the element_a and element_b are non-null.
    unsafe fn insert_before(&mut self, element_a: NonNull<Link>, element_b: NonNull<Link>) {
        unsafe { self.inner.insert_before(element_a, element_b) };
    }

    /// Unlinks element from the linked list.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is in the list.
    pub unsafe fn unlink_element_unchecked(&mut self, element: NonNull<T>) {
        let link_ptr = unsafe { A::get_link(element) };
        unsafe { self.inner.unlink_element_unchecked(link_ptr) };
    }

    /// # Safety
    ///
    /// The caller must ensure that the element points to a valid `T`.
    pub unsafe fn unlink_element(&mut self, element: NonNull<T>) -> Option<NonNull<T>> {
        let link_ptr = unsafe { A::get_link(element) };
        unsafe { self.inner.unlink_element(link_ptr).map(|()| element) }
    }

    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    pub unsafe fn for_each<E, F: FnMut(&T) -> Result<(), E>>(
        &self,
        mut callback: F,
    ) -> Result<(), E> {
        let mut res = Ok(());
        unsafe {
            self.inner.for_each(&mut |link_ptr| {
                let element = A::get_element(link_ptr);
                callback(element.as_ref()).map_err(|e| res = Err(e))
            })
        };
        res
    }

    /// Filter iterates over every element in the list calling `callback` on
    /// each one.  If `callback` returns false, the element will be removed
    /// from the list without modifying the element itself.  It is safe to
    /// add the element to the another linked list within `callback` if it
    /// returns false.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    pub unsafe fn filter<F: FnMut(&mut T) -> bool>(&mut self, mut callback: F) {
        unsafe {
            self.inner.filter(&mut |link_ptr| {
                let mut element = A::get_element(link_ptr);
                callback(element.as_mut())
            })
        }
    }

    /// Return a reference to the first element in the list, clearing the
    /// prev/next fields in the element.
    ///
    /// # Safety
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    pub unsafe fn pop_head(&mut self) -> Option<NonNull<T>> {
        self.inner
            .pop_head()
            .map(|link_ptr| unsafe { A::get_element(link_ptr) })
    }
}

impl<T, A: Adapter<T>> UnsafeList<T, A> {
    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn sorted_insert_by_unchecked<F: FnMut(&T, &T) -> Ordering>(
        &mut self,
        element: NonNull<T>,
        mut compare: F,
    ) {
        let link_ptr = unsafe { A::get_link(element) };

        let mut cmp = move |lhs, rhs| {
            let lhs = unsafe { A::get_element(lhs) };
            let rhs = unsafe { A::get_element(rhs) };
            unsafe { compare(lhs.as_ref(), rhs.as_ref()) }
        };
        unsafe { self.inner.sorted_insert_by_unchecked(link_ptr, &mut cmp) };
    }
}

impl UnsafeListInner {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            head: None,
            tail: None,
        }
    }

    const fn is_empty(&self) -> bool {
        self.head.is_none()
    }

    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn push_front_unchecked(&mut self, link_ptr: NonNull<Link>) {
        // Link up the added element.
        let link = unsafe { link_ptr.as_ref() };
        link.set_next(self.head);
        link.set_prev(None);

        match self.head {
            // If `head` was `None`, the list is empty and `tail` should point
            // to the added element.
            None => self.tail = Some(link_ptr),

            // If `head` is not `None`, point the previous `head` to the added
            // element.
            Some(head) => unsafe { head.as_ref() }.set_prev(Some(link_ptr)),
        }

        // Finally point `head` to the added element.
        self.head = Some(link_ptr);
    }

    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn push_back_unchecked(&mut self, link_ptr: NonNull<Link>) {
        // Link up the added element.
        let link = unsafe { link_ptr.as_ref() };
        link.set_next(None);
        link.set_prev(self.tail);

        match self.tail {
            // If `tail` was `None`, the list is empty and `head` should point
            // to the added element.
            None => self.head = Some(link_ptr),

            // If `tail` is not `None`, point the previous `tail` to the added
            // element.
            Some(tail) => unsafe { tail.as_ref() }.set_next(Some(link_ptr)),
        }

        self.tail = Some(link_ptr);
    }

    /// Insert element_a into the list immediately before `element_b`
    ///
    /// # Safety
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    /// It is up to the caller to ensure that element_a is not in a list.
    /// It is up to the caller to ensure that element_b is in a list.
    /// It is up to the caller to ensure the element_a and element_b are non-null.
    unsafe fn insert_before(&mut self, element_a: NonNull<Link>, element_b: NonNull<Link>) {
        let a = unsafe { element_a.as_ref() };
        let b = unsafe { element_b.as_ref() };

        let prev = b.get_prev();

        a.set_next(Some(element_b));
        a.set_prev(prev);

        b.set_prev(Some(element_a));

        match prev {
            // Element is at the head of the list
            None => self.head = Some(element_a),

            // Element has elements before it in the list.
            Some(prev_ptr) => unsafe { prev_ptr.as_ref() }.set_next(Some(element_a)),
        }
    }

    /// Unlinks element from the linked list.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is in the list.
    pub unsafe fn unlink_element_unchecked(&mut self, link_ptr: NonNull<Link>) {
        // Wrap in a block to ensure `link`, which is a `&mut`, doesn't live
        // long enough to possibly overlap with other `&mut` references to the
        // same memory.
        let (prev, next) = {
            let link = unsafe { link_ptr.as_ref() };
            let prev = link.get_prev();
            let next = link.get_next();
            link.set_unlinked();
            (prev, next)
        };

        match prev {
            // Element is at the head of the list
            None => self.head = next,

            // Element has elements before it in the list.
            Some(prev_ptr) => unsafe { prev_ptr.as_ref() }.set_next(next),
        }

        match next {
            // Element is at the tail of the list
            None => self.tail = prev,

            // Element has elements after it in the list.
            Some(next_ptr) => unsafe { next_ptr.as_ref() }.set_prev(prev),
        }
    }

    /// # Safety
    ///
    /// The caller must ensure that the element points to a valid `T`.
    pub unsafe fn unlink_element(&mut self, link_ptr: NonNull<Link>) -> Option<()> {
        if unsafe { link_ptr.as_ref() }.is_linked() {
            unsafe { self.unlink_element_unchecked(link_ptr) };
            Some(())
        } else {
            None
        }
    }

    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    pub unsafe fn for_each(&self, callback: &mut dyn FnMut(NonNull<Link>) -> Result<(), ()>) {
        let mut cur = self.head;

        loop {
            let Some(cur_ptr) = cur else {
                break;
            };

            unsafe {
                if callback(cur_ptr).is_err() {
                    return;
                }
                cur = cur_ptr.as_ref().get_next();
            }
        }
    }

    /// Filter iterates over every element in the list calling `callback` on
    /// each one.  If `callback` returns false, the element will be removed
    /// from the list without modifying the element itself.  It is safe to
    /// add the element to the another linked list within `callback` if it
    /// returns false.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    pub unsafe fn filter(&mut self, callback: &mut dyn FnMut(NonNull<Link>) -> bool) {
        let mut cur = self.head;

        loop {
            let Some(cur_ptr) = cur else {
                break;
            };

            let next = unsafe {
                // Cache the next element so that we don't rely on `cur_ptr`
                // staying coherent across calls to `callback`.
                let next = cur_ptr.as_ref().get_next();

                if !callback(cur_ptr) {
                    self.unlink_element_unchecked(cur_ptr);
                }
                next
            };

            cur = next;
        }
    }

    /// Return a reference to the first element in the list, clearing the
    /// prev/next fields in the element.
    pub fn pop_head(&mut self) -> Option<NonNull<Link>> {
        let head = self.head?;
        unsafe { self.unlink_element_unchecked(head) };
        Some(head)
    }
}

impl UnsafeListInner {
    /// Unchecked means:
    /// * We don't `assert!((*element_ptr.as_ptr()).is_unlinked());`
    /// * We don't check that `element` is non-null.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure exclusive access to the list and its
    /// members.
    ///
    /// It is up to the caller to ensure the element is not in a list.
    pub unsafe fn sorted_insert_by_unchecked(
        &mut self,
        link_ptr: NonNull<Link>,
        cmp: &mut dyn FnMut(NonNull<Link>, NonNull<Link>) -> Ordering,
    ) {
        let mut cur = self.head;

        loop {
            let Some(cur_link_ptr) = cur else {
                break;
            };

            unsafe {
                if let Ordering::Less | Ordering::Equal = cmp(link_ptr, cur_link_ptr) {
                    self.insert_before(link_ptr, cur_link_ptr);
                    return;
                }
            }

            cur = unsafe { cur_link_ptr.as_ref().get_next() };
        }

        // Either the list is empty or all elements are less than `link_ptr`. In
        // both cases pushing the element to the back is appropriate.
        unsafe {
            self.push_back_unchecked(link_ptr);
        }
    }
}

impl<T, A: Adapter<T>> Default for UnsafeList<T, A> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use core::ptr::NonNull;

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

    impl PartialEq for TestMember {
        fn eq(&self, other: &Self) -> bool {
            self.value == other.value
        }
    }

    impl Eq for TestMember {}

    impl PartialOrd for TestMember {
        fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
            Some(self.cmp(other))
        }
    }

    impl Ord for TestMember {
        fn cmp(&self, other: &Self) -> core::cmp::Ordering {
            self.value.cmp(&other.value)
        }
    }

    define_adapter!(TestAdapter => TestMember::link);

    unsafe fn validate_list(
        list: &UnsafeList<TestMember, TestAdapter>,
        expected_values: &[u32],
    ) -> unittest::Result<()> {
        let mut index = 0;
        unsafe {
            list.for_each(|element| {
                unittest::assert_eq!(element.value, expected_values[index]);
                index += 1;
                Ok(())
            })
        }?;

        unittest::assert_eq!(index, expected_values.len());
        Ok(())
    }

    #[test]
    fn new_link_is_not_linked() -> unittest::Result<()> {
        let link = Link::new();
        unittest::assert_false!(link.is_linked());
        unittest::assert_true!(link.is_unlinked());
        Ok(())
    }

    #[test]
    fn new_list_is_empty() -> unittest::Result<()> {
        let list = UnsafeList::<TestMember, TestAdapter>::new();
        unittest::assert_true!(unsafe { list.is_empty() });
        Ok(())
    }

    #[test]
    fn single_element_list_is_non_empty_and_linked() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };

        let element1 = NonNull::from(&mut element1);

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();

        unittest::assert_true!(unsafe { list.is_empty() });
        unittest::assert_false!(unsafe { list.is_element_linked(element1) });

        unsafe { list.push_front_unchecked(element1) };

        unittest::assert_false!(unsafe { list.is_empty() });
        unittest::assert_true!(unsafe { list.is_element_linked(element1) });

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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unittest::assert_false!(unsafe { list.is_empty() });

        unsafe { validate_list(&list, &[1, 2]) }
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_back_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_back_unchecked(NonNull::from(&mut element1)) };

        unittest::assert_false!(unsafe { list.is_empty() });

        unsafe { validate_list(&list, &[2, 1]) }
    }

    #[test]
    fn unlink_removes_head_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };

        let element1 = NonNull::from(&mut element1);
        unsafe { list.push_front_unchecked(element1) };

        unsafe { list.unlink_element_unchecked(element1) };

        unsafe { validate_list(&list, &[2, 3]) }
    }

    #[test]
    fn unlink_removes_tail_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        let element3 = NonNull::from(&mut element3);
        unsafe { list.push_front_unchecked(element3) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.unlink_element_unchecked(element3) };

        unsafe { validate_list(&list, &[1, 2]) }
    }

    #[test]
    fn unlink_removes_middle_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        let element2 = NonNull::from(&mut element2);
        unsafe { list.push_front_unchecked(element2) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.unlink_element_unchecked(element2) };

        unsafe { validate_list(&list, &[1, 3]) }
    }

    #[test]
    fn unlink_fails_non_inserted_element() -> unittest::Result<()> {
        let mut element1 = TestMember {
            value: 1,
            link: Link::new(),
        };

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();

        unittest::assert_eq!(
            unsafe { list.unlink_element(NonNull::from(&mut element1)) },
            None
        );
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };

        unsafe {
            let e = list.pop_head();
            unittest::assert_true!(e.is_some());
            let e = e.unwrap().as_ref();
            unittest::assert_eq!(e.value, 3);
            unittest::assert_true!(e.link.is_unlinked());
        }

        unsafe {
            let e = list.pop_head();
            unittest::assert_true!(e.is_some());
            let e = e.unwrap().as_ref();
            unittest::assert_eq!(e.value, 2);
            unittest::assert_true!(e.link.is_unlinked());
        }

        unsafe {
            let e = list.pop_head();
            unittest::assert_true!(e.is_some());
            let e = e.unwrap().as_ref();
            unittest::assert_eq!(e.value, 1);
            unittest::assert_true!(e.link.is_unlinked());
        }

        unsafe { validate_list(&list, &[]) }
    }

    #[test]
    fn filter_removes_nothing_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.filter(|_| true) };

        unsafe { validate_list(&list, &[1, 2, 3]) }
    }

    #[test]
    fn filter_removes_everything_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.filter(|_| false) };

        unsafe { validate_list(&list, &[]) }
    }

    #[test]
    fn filter_removes_head_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.filter(|element| element.value != 1) };

        unsafe { validate_list(&list, &[2, 3]) }
    }

    #[test]
    fn filter_removes_middle_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.filter(|element| element.value != 2) };

        unsafe { validate_list(&list, &[1, 3]) }
    }

    #[test]
    fn filter_removes_tail_correctly() -> unittest::Result<()> {
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.push_front_unchecked(NonNull::from(&mut element3)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element2)) };
        unsafe { list.push_front_unchecked(NonNull::from(&mut element1)) };

        unsafe { list.filter(|element| element.value != 3) };

        unsafe { validate_list(&list, &[1, 2]) }
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element3), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element2), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element1), TestMember::cmp) };
        unsafe { validate_list(&list, &[1, 2, 3]) }
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element1), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element2), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element3), TestMember::cmp) };
        unsafe { validate_list(&list, &[1, 2, 3]) }
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

        let mut list = UnsafeList::<TestMember, TestAdapter>::new();
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element2), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element1), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element3), TestMember::cmp) };
        unsafe { list.sorted_insert_by_unchecked(NonNull::from(&mut element2_2), TestMember::cmp) };
        unsafe { validate_list(&list, &[1, 2, 2, 3]) }
    }
}
