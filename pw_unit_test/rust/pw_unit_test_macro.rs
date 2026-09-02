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
extern crate proc_macro;
use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemFn};

/// A custom `#[test]` attribute macro for Pigweed unit testing across host and device platforms.
///
/// * For Host builds (`cfg(not(pw_unit_test))`), it forwards the test function to the
///   standard built-in `#[test]` runner for normal host execution.
/// * For Device builds (`cfg(pw_unit_test)`), it retains the `fn()` signature and registers
///   the test in the `pw_unit_test_desc` linker section for on-device execution.
#[proc_macro_attribute]
pub fn test(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as ItemFn);
    if !matches!(input.sig.output, syn::ReturnType::Default) {
        return syn::Error::new_spanned(
            &input.sig.output,
            "pw_unit_test tests must have a `()` return type because assertion macros return early on failure",
        )
        .to_compile_error()
        .into();
    }

    let fn_name = &input.sig.ident;
    let fn_name_str = fn_name.to_string();
    let desc_name = syn::Ident::new(&format!("_PW_UNIT_TEST_DESC_{}", fn_name), fn_name.span());
    let block = &input.block;
    let vis = &input.vis;
    let attrs = &input.attrs;

    let expanded = quote! {
        #[cfg(not(pw_unit_test))]
        #[::core::prelude::v1::test]
        #input

        #[cfg(pw_unit_test)]
        const _: () = {
            #(#attrs)*
            #vis extern "C" fn #fn_name() {
                #block
            }

            #[allow(non_upper_case_globals)]
            #[cfg_attr(target_vendor = "apple", link_section = "__DATA,__pw_unit_test")]
            #[cfg_attr(not(target_vendor = "apple"), link_section = "pw_unit_test_desc")]
            #[used]
            static #desc_name: ::pw_unit_test::__private::TestDescriptor =
                ::pw_unit_test::__private::TestDescriptor {
                    name: concat!(#fn_name_str, "\0").as_ptr().cast::<::core::ffi::c_char>(),
                    suite: concat!(module_path!(), "\0").as_ptr().cast::<::core::ffi::c_char>(),
                    test_fn: Some(#fn_name),
                };
        };
    };

    TokenStream::from(expanded)
}
