// Copyright 2023 The Pigweed Authors
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
use proc_macro2::Ident;
use pw_format::macros::{
    generate_printf, Arg, CoreFmtFormatStringParser, FormatAndArgsFlavor, FormatStringParser,
    PrintfFormatMacroGenerator, PrintfFormatStringFragment, PrintfFormatStringParser, Result,
};
use quote::{quote, ToTokens};
use syn::parse::{Parse, ParseStream};
use syn::{parse_macro_input, Expr, Token};

type TokenStream2 = proc_macro2::TokenStream;

// Arguments to `pw_log[f]_backend`.  A log level followed by a [`pw_format`]
// format string.
#[derive(Debug)]
struct PwLogArgs<T: FormatStringParser> {
    log_level: Expr,
    format_and_args: FormatAndArgsFlavor<T>,
}

impl<T: FormatStringParser> Parse for PwLogArgs<T> {
    fn parse(input: ParseStream) -> syn::parse::Result<Self> {
        let log_level: Expr = input.parse()?;
        input.parse::<Token![,]>()?;
        let format_and_args: FormatAndArgsFlavor<_> = input.parse()?;

        Ok(PwLogArgs {
            log_level,
            format_and_args,
        })
    }
}

// Generator that implements [`pw_format::PrintfFormatMacroGenerator`] to take
// a log line and turn it into `printf` calls;
struct LogfGenerator<'a> {
    log_level: &'a Expr,
    args: Vec<TokenStream2>,
}

impl<'a> LogfGenerator<'a> {
    fn new(log_level: &'a Expr) -> Self {
        Self {
            log_level,
            args: Vec::new(),
        }
    }
}

// Use a [`pw_format::PrintfFormatMacroGenerator`] to prepare arguments to call
// `printf`.
impl PrintfFormatMacroGenerator for LogfGenerator<'_> {
    fn finalize(
        self,
        format_string_fragments: &[PrintfFormatStringFragment],
    ) -> Result<TokenStream2> {
        let log_level = self.log_level;
        let args = &self.args;
        let format_string_pieces: Vec<_> = format_string_fragments
            .iter()
            .map(|fragment| fragment.as_token_stream("__pw_log_backend_crate"))
            .collect::<Result<Vec<_>>>()?;
        Ok(quote! {
          {
            use core::ffi::{c_int, c_uchar};
            use __pw_log_backend_crate::{Arguments, VarArgs};
            // Prepend log level tag and append newline and null terminator for
            // C string validity.
            let format_string = __pw_log_backend_crate::concat_static_strs!(
              "[%s] ", #(#format_string_pieces),*, "\n\0"
            );
            unsafe {
              // Build up the argument type/value.
              let args = ();
              #(#args)*

              // Call printf through the argument type/value.
              args.call_printf(format_string.as_ptr(),
                __pw_log_backend_crate::log_level_tag(#log_level).as_ptr());
            }
          }
        })
    }

    fn string_fragment(&mut self, _string: &str) -> Result<()> {
        // String fragments are encoded directly into the format string.
        Ok(())
    }

    fn integer_conversion(&mut self, ty: Ident, expression: Arg) -> Result<Option<String>> {
        self.args.push(quote! {
          let args = <#ty as Arguments<#ty>>::push_arg(args, &((#expression) as #ty));
        });
        Ok(None)
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        // In order to not convert Rust Strings to CStrings at runtime, we use
        // the "%.*s" specifier to explicitly bound the length of the
        // non-null-terminated Rust String.
        self.args.push(quote! {
          let args = <&str as Arguments<&str>>::push_arg(args, &((#expression) as &str));
        });
        Ok(Some("%.*s".into()))
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.args.push(quote! {
          let args = <char as Arguments<char>>::push_arg(args, &((#expression) as char));
        });
        Ok(None)
    }

    fn untyped_conversion(&mut self, expression: Arg) -> Result<()> {
        match &expression {
            Arg::ExprCast(cast) => {
                let ty = &cast.ty;
                self.args.push(quote! {
                  let args = <#ty as Arguments<#ty>>::push_arg(args, &(#expression));
                });
            }
            Arg::Expr(_) => {
                return Err(pw_format::macros::Error::new(&format!(
                    "Expected argument to untyped format (%v) to be a cast expression (e.g. x as i32), but found {}.",
                    expression.to_token_stream()
                )));
            }
        }
        Ok(())
    }
}

#[proc_macro]
pub fn _pw_log_backend(tokens: TokenStream) -> TokenStream {
    let input = parse_macro_input!(tokens as PwLogArgs<CoreFmtFormatStringParser>);
    let generator = LogfGenerator::new(&input.log_level);

    match generate_printf(generator, input.format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

#[proc_macro]
pub fn _pw_logf_backend(tokens: TokenStream) -> TokenStream {
    let input = parse_macro_input!(tokens as PwLogArgs<PrintfFormatStringParser>);
    let generator = LogfGenerator::new(&input.log_level);

    match generate_printf(generator, input.format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}
