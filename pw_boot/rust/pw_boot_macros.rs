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

use proc_macro::TokenStream;
use quote::quote;
use syn::ItemFn;

#[proc_macro_attribute]
pub fn entry(args: TokenStream, input: TokenStream) -> TokenStream {
    match entry_impl(args.into(), input.into()) {
        Ok(tokens) => tokens.into(),
        Err(err) => err.to_compile_error().into(),
    }
}

fn entry_impl(
    args: proc_macro2::TokenStream,
    input: proc_macro2::TokenStream,
) -> syn::Result<proc_macro2::TokenStream> {
    if !args.is_empty() {
        return Err(syn::Error::new(
            proc_macro2::Span::call_site(),
            "#[entry] macro does not take any arguments",
        ));
    }

    let entry_fn = syn::parse2::<ItemFn>(input)?;
    validate_entry_signature(&entry_fn.sig)?;

    let entry_fn_ident = &entry_fn.sig.ident;
    let expanded = quote! {
        #entry_fn

        #[no_mangle]
        pub extern "C" fn pw_boot_rust_entry(_arg: *mut ::core::ffi::c_void) {
            #entry_fn_ident();
        }
    };

    Ok(expanded)
}

fn validate_entry_signature(sig: &syn::Signature) -> syn::Result<()> {
    let is_valid = sig.constness.is_none()
        && sig.asyncness.is_none()
        && sig.unsafety.is_none()
        && sig.abi.is_none()
        && sig.generics.params.is_empty()
        && sig.generics.where_clause.is_none()
        && sig.inputs.is_empty()
        && sig.variadic.is_none()
        && matches!(sig.output, syn::ReturnType::Type(_, ref ty) if matches!(ty.as_ref(), syn::Type::Never(_)));

    if !is_valid {
        return Err(syn::Error::new_spanned(
            sig,
            "#[entry] function must have the signature `fn f() -> !`",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use quote::quote;

    use super::*;

    #[test]
    fn test_valid_entry() {
        let args = quote! {};
        let input = quote! {
            fn main() -> ! {
                loop {}
            }
        };
        entry_impl(args, input).unwrap();
    }

    #[test]
    fn test_args_not_empty() {
        let args = quote! { foo };
        let input = quote! {
            fn main() -> ! {
                loop {}
            }
        };
        let err = entry_impl(args, input).unwrap_err();
        assert_eq!(
            err.to_string(),
            "#[entry] macro does not take any arguments"
        );
    }

    #[test]
    fn test_invalid_signature() {
        let test_cases = vec![
            quote! { const fn main() -> ! { loop {} } },
            quote! { async fn main() -> ! { loop {} } },
            quote! { unsafe fn main() -> ! { loop {} } },
            quote! { extern "C" fn main() -> ! { loop {} } },
            quote! { fn main<T>() -> ! { loop {} } },
            quote! { fn main(x: i32) -> ! { loop {} } },
            quote! { fn main() {} },
            quote! { fn main() -> () {} },
            quote! { fn main() -> i32 { 0 } },
        ];

        for input in test_cases {
            let args = quote! {};
            let err = entry_impl(args, input).unwrap_err();
            assert_eq!(
                err.to_string(),
                "#[entry] function must have the signature `fn f() -> !`"
            );
        }
    }
}
