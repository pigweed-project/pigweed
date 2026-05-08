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
use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::quote;
use syn::spanned::Spanned;
use syn::{Ident, ItemFn, ReturnType, Visibility, parse};

#[derive(Copy, Clone)]
enum ReturnKind {
    U32,
    Result,
    Unit,
}

fn validate_signature(f: &ItemFn, macro_name: &str) -> Result<ReturnKind, TokenStream> {
    let valid_signature = f.sig.constness.is_none()
        && f.sig.asyncness.is_none()
        && f.vis == Visibility::Inherited
        && f.sig.abi.is_none()
        && (f.sig.inputs.is_empty() || f.sig.inputs.len() == 1)
        && f.sig.generics.params.is_empty()
        && f.sig.generics.where_clause.is_none()
        && f.sig.variadic.is_none();

    if !valid_signature {
        return Err(parse::Error::new(
            f.span(),
            format!("`#[{}]` function has invalid signature", macro_name),
        )
        .to_compile_error()
        .into());
    }

    // Check return type: must be u32, Result or ()
    if let ReturnType::Type(_, ty) = &f.sig.output {
        if let syn::Type::Path(type_path) = ty.as_ref() {
            if type_path.path.is_ident("u32") {
                return Ok(ReturnKind::U32);
            } else if type_path
                .path
                .segments
                .last()
                .is_some_and(|s| s.ident == "Result")
            {
                return Ok(ReturnKind::Result);
            }
        }
    } else if let ReturnType::Default = &f.sig.output {
        return Ok(ReturnKind::Unit);
    }

    Err(parse::Error::new(
        f.span(),
        format!(
            "`#[{}]` function must return u32, Result, or ()",
            macro_name
        ),
    )
    .to_compile_error()
    .into())
}

fn generate_wrapper_function(
    user_func: ItemFn,
    wrapper_ident: Ident,
    has_arg: bool,
    return_kind: ReturnKind,
    asm: Option<proc_macro2::TokenStream>,
) -> proc_macro2::TokenStream {
    let args = if has_arg {
        quote!(arg: usize)
    } else {
        quote!()
    };

    let user_func_ident = &user_func.sig.ident;
    let user_func_call = if has_arg {
        quote! { #user_func_ident(arg) }
    } else {
        quote! { #user_func_ident() }
    };

    let wrapper_body = match return_kind {
        ReturnKind::U32 => {
            quote!(
                let ret = unsafe { #user_func_call };
                let _ = userspace::syscall::process_exit(ret);
            )
        }
        ReturnKind::Result => {
            quote!(
                use pw_status::StatusCode;
                let ret = unsafe { #user_func_call };
                let _ = userspace::syscall::process_exit(ret.status_code());
            )
        }
        ReturnKind::Unit => {
            quote!(
                let _ = unsafe { #user_func_call };
                let _ = userspace::syscall::process_exit(0);
            )
        }
    };

    quote!(
        #asm

        #user_func

        #[unsafe(no_mangle)]
        pub extern "C" fn #wrapper_ident(#args) -> ! {
            #wrapper_body
            loop {}
        }
    )
}

fn generate_entry_macro(args: TokenStream, input: TokenStream, asm: &'static str) -> TokenStream {
    let mut user_func = match parse::<ItemFn>(input) {
        Ok(item_fn) => item_fn,
        Err(e) => return e.to_compile_error().into(),
    };

    if !args.is_empty() {
        return parse::Error::new(Span::call_site(), "`#[entry]` accepts no arguments")
            .to_compile_error()
            .into();
    }

    let return_kind = match validate_signature(&user_func, "entry") {
        Ok(k) => k,
        Err(e) => return e,
    };

    let f_has_arg = user_func.sig.inputs.len() == 1;
    if f_has_arg {
        let arg = &user_func.sig.inputs[0];
        let valid_arg = if let syn::FnArg::Typed(pat_type) = arg {
            if let syn::Type::Path(type_path) = &*pat_type.ty {
                type_path.path.is_ident("usize")
            } else {
                false
            }
        } else {
            false
        };

        if !valid_arg {
            return parse::Error::new(arg.span(), "`#[entry]` argument must be of type `usize`")
                .to_compile_error()
                .into();
        }
    }

    let renamed_ident = Ident::new(
        &format!("_start_{}", user_func.sig.ident),
        Span::call_site(),
    );
    user_func.sig.ident = renamed_ident;

    let wrapper_name = Ident::new("main", Span::call_site());
    let asm_tokens = quote!(
        use core::arch::global_asm;
        global_asm!(#asm, options(raw));
    );
    generate_wrapper_function(
        user_func,
        wrapper_name,
        f_has_arg,
        return_kind,
        Some(asm_tokens),
    )
    .into()
}

#[proc_macro_attribute]
pub fn arm_cortex_m_entry(args: TokenStream, input: TokenStream) -> TokenStream {
    generate_entry_macro(args, input, include_str!("arm_cortex_m/entry.s"))
}

#[proc_macro_attribute]
pub fn riscv_entry(args: TokenStream, input: TokenStream) -> TokenStream {
    generate_entry_macro(args, input, include_str!("riscv/entry.s"))
}

#[proc_macro_attribute]
pub fn process_entry(args: TokenStream, input: TokenStream) -> TokenStream {
    let mut user_func = match parse::<ItemFn>(input) {
        Ok(item_fn) => item_fn,
        Err(e) => return e.to_compile_error().into(),
    };

    if args.is_empty() {
        return parse::Error::new(
            Span::call_site(),
            "`#[process_entry]` requires a process name argument",
        )
        .to_compile_error()
        .into();
    }
    let name_str = args.to_string().trim().replace('"', "");
    let entry_ident = Ident::new(&format!("_entry_{}", name_str), Span::call_site());
    let renamed_ident = Ident::new(&format!("_inner_{}", name_str), Span::call_site());
    user_func.sig.ident = renamed_ident;

    let return_kind = match validate_signature(&user_func, "process_entry") {
        Ok(k) => k,
        Err(e) => return e,
    };

    if !user_func.sig.inputs.is_empty() {
        return parse::Error::new(
            user_func.span(),
            "`#[process_entry]` function must not take any arguments",
        )
        .to_compile_error()
        .into();
    }

    generate_wrapper_function(user_func, entry_ident, false, return_kind, None).into()
}
