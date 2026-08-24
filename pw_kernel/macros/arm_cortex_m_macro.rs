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

//! # Proc macros for ARM Cortex M support in the kernel
//!
//! # Exceptions
//!
//! Two attribute macros ([`kernel_only_exception`] and [`user_space_exception`])
//! are provided for generating exception wrappers.  They have the same syntax
//! and are intended to be conditionally imported based on the kernel's
//! `user_space` feature:
//!
//! ```
//! #[cfg(not(feature = "user_space"))]
//! pub(crate) use arm_cortex_m_macro::kernel_only_exception as exception;
//! #[cfg(feature = "user_space")]
//! pub(crate) use arm_cortex_m_macro::user_space_exception as exception;
//! ```
//!
//! To use the macros, an exception name must be provided:
//! ```
//! #[exception(exception = "HardFault")]
//! #[unsafe(no_mangle)]
//! extern "C" fn hard_fault(frame: *mut FullExceptionFrame) ->  *mut FullExceptionFrame {
//! }
//! ```
//!
//! An optional, `disable_interrupts` attribute can be passed to disable
//! interrupts while invoking the handler:
//! ```
//! #[exception(exception = "PendSV", disable_interrupts)]
//! #[unsafe(no_mangle)]
//! extern "C" fn pendsv(frame: *mut FullExceptionFrame) -> *mut FullExceptionFrame {
//! ```

use arch_macro_helpers::{validate_handler_abi, validate_interrupt_handler_args};
use proc_macro::TokenStream;
use quote::{format_ident, quote};
use syn::parse::{Parse, ParseStream, Result};
use syn::punctuated::Punctuated;
use syn::spanned::Spanned;
use syn::{Error, FnArg, Ident, ItemFn, Token, Type, parse_macro_input};

#[derive(Eq, PartialEq, Hash, Debug)]
enum KernelMode {
    UserSpace,
    KernelOnly,
}

impl KernelMode {
    fn save_psp_needed(&self) -> bool {
        *self == Self::UserSpace
    }
}

#[derive(Eq, PartialEq, Hash, Debug)]
enum Attribute {
    Exception(String),
    DisableInterrupts,
}

impl Attribute {
    fn parse_value(input: ParseStream) -> Result<String> {
        let _ = input.parse::<Token![=]>()?;
        let value = input.parse::<syn::LitStr>()?;
        Ok(value.value())
    }
}

impl Parse for Attribute {
    fn parse(input: ParseStream) -> Result<Self> {
        let name = input.parse::<Ident>()?;
        let name_str = name.to_string();
        match name_str.as_str() {
            "exception" => {
                let value = Self::parse_value(input)?;
                Ok(Attribute::Exception(value))
            }
            "disable_interrupts" => Ok(Attribute::DisableInterrupts),
            _ => Err(input.error(format!("unknown attribute {name_str}"))),
        }
    }
}

struct Attributes {
    exception: String,
    disable_interrupts: bool,
}

impl Parse for Attributes {
    fn parse(input: ParseStream) -> Result<Self> {
        let attributes = Punctuated::<Attribute, Token![,]>::parse_terminated(input)?;
        let mut exception = None;
        let mut disable_interrupts = false;
        for attr in attributes {
            match attr {
                Attribute::Exception(name) => exception = Some(name),
                Attribute::DisableInterrupts => disable_interrupts = true,
            }
        }

        let exception = exception.ok_or_else(|| input.error("missing exception name"))?;

        Ok(Attributes {
            exception,
            disable_interrupts,
        })
    }
}

fn handler_function_argument_error(handler: &ItemFn) -> Error {
    Error::new(
        handler.sig.inputs.span(),
        "Handler must have a single `*mut KernelExceptionFrame` argument",
    )
}

fn validate_handler_args(handler: &ItemFn) -> Result<()> {
    if handler.sig.inputs.len() != 1 {
        return Err(handler_function_argument_error(handler));
    }

    let FnArg::Typed(pat_ty) = &handler.sig.inputs.first().unwrap() else {
        return Err(handler_function_argument_error(handler));
    };

    let Type::Ptr(ty) = &*pat_ty.ty else {
        return Err(handler_function_argument_error(handler));
    };

    if !matches!(ty.mutability, syn::PointerMutability::Mut(_)) {
        return Err(handler_function_argument_error(handler));
    };

    Ok(())
}

fn validate_handler_function(handler: &ItemFn) -> Result<()> {
    validate_handler_abi(handler)?;
    validate_handler_args(handler)?;

    Ok(())
}

fn save_exception_frame(asm: &mut String, kernel_mode: &KernelMode) {
    asm.push_str("// save the additional registers\n");
    if kernel_mode.save_psp_needed() {
        asm.push_str(
            "
            mrs     r1, control
            mrs     r0, psp
            push    {{ r0 - r1, lr }}

            push    {{ r4 - r11 }}
            mov     r0, sp
            sub     sp, 4   // Align stack to 8 bytes
            ",
        );
    } else {
        asm.push_str(
            "
            push    {{ r4 - r11, lr }}
            mov     r0, sp
            sub     sp, 4   // Align stack to 8 bytes
            ",
        );
    }
}

fn restore_exception_frame(asm: &mut String, kernel_mode: &KernelMode) {
    if kernel_mode.save_psp_needed() {
        asm.push_str(
            "
            mov     sp, r0
            pop     {{ r4 - r11 }}

            pop     {{ r0 - r1 }}
            msr     psp, r0
            msr     control, r1

            pop     {{ pc }}
    ",
        );
    } else {
        asm.push_str(
            "
            mov     sp, r0
            pop     {{ r4 - r11, pc }}
    ",
        );
    }
}

fn disable_interrupts(asm: &mut String) {
    asm.push_str("cpsid   i\n");
}

fn enable_interrupts(asm: &mut String) {
    asm.push_str("cpsie   i\n");
}

fn exception(attr: TokenStream, item: TokenStream, kernel_mode: KernelMode) -> TokenStream {
    let handler = parse_macro_input!(item as ItemFn);
    let attributes = parse_macro_input!(attr as Attributes);

    if let Err(e) = validate_handler_function(&handler) {
        return e.to_compile_error().into();
    }

    let exception_ident = format_ident!("{}", attributes.exception);
    let handler_ident = &handler.sig.ident;
    let handler_name = handler_ident.clone().to_string();

    let mut asm = String::new();
    save_exception_frame(&mut asm, &kernel_mode);

    if attributes.disable_interrupts {
        disable_interrupts(&mut asm);
    }

    asm.push_str(&format!("bl     {handler_name}\n"));

    if attributes.disable_interrupts {
        enable_interrupts(&mut asm);
    }

    restore_exception_frame(&mut asm, &kernel_mode);

    quote! {
        #[unsafe(no_mangle)]
        #[unsafe(naked)]
        pub unsafe extern "C" fn #exception_ident() -> ! {
            unsafe {
                core::arch::naked_asm!(#asm)
            }
        }
        // Compile time assert that the handler function signature matches.
        const _: crate::exceptions::ExceptionHandler = #handler_ident;
        #handler
    }
    .into()
}

/// Generate an exception wrapper with kernel only support.
#[proc_macro_attribute]
pub fn kernel_only_exception(attr: TokenStream, item: TokenStream) -> TokenStream {
    exception(attr, item, KernelMode::KernelOnly)
}

/// Generate an exception wrapper with user-space support.
#[proc_macro_attribute]
pub fn user_space_exception(attr: TokenStream, item: TokenStream) -> TokenStream {
    exception(attr, item, KernelMode::UserSpace)
}

/// Generate an interrupt wrapper that passes `from_userspace: bool` to the handler.
#[proc_macro_attribute]
pub fn interrupt(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let mut handler = syn::parse_macro_input!(item as syn::ItemFn);

    if let Err(err) = validate_handler_abi(&handler) {
        return err.to_compile_error().into();
    }
    if let Err(err) = validate_interrupt_handler_args(&handler) {
        return err.to_compile_error().into();
    }

    let handler_ident = handler.sig.ident.clone();
    let handler_impl_ident = quote::format_ident!("{}_impl", handler_ident);

    // Update handler's ident to the impl name.
    handler.sig.ident = handler_impl_ident.clone();

    quote::quote! {
        // This wrapper must be a naked function to ensure we can read the Link Register (`lr`)
        // at the very beginning of the exception before any compiler-generated prologue
        // modifies registers or the stack. It also allows us to perform a tail call
        // directly to the handler without creating a stack frame.
        #[unsafe(no_mangle)]
        #[unsafe(naked)]
        pub unsafe extern "C" fn #handler_ident() {
            unsafe {
                core::arch::naked_asm!(
                    "mov r0, lr",
                    "and r0, #4",
                    "lsr r0, #2",
                    "b {handler}",
                    handler = sym #handler_impl_ident,
                )
            }
        }
        #handler
    }
    .into()
}
