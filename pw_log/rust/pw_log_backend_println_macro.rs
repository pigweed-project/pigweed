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
    generate_core_fmt, Arg, CoreFmtFormatMacroGenerator, CoreFmtFormatStringParser,
    FormatAndArgsFlavor, FormatStringParser, PrintfFormatStringParser, Result,
};
use quote::quote;
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

// Generator that implements [`pw_format::CoreFmtFormatMacroGenerator`] to take
// a log line and turn it into [`std::println`] calls.
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

// Use a [`pw_format::CoreFmtFormatMacroGenerator`] to prepare arguments to call
// [`std::println`].
impl CoreFmtFormatMacroGenerator for LogfGenerator<'_> {
    fn finalize(self, format_string: String) -> Result<TokenStream2> {
        let log_level = self.log_level;
        let args = &self.args;
        let format_string = format!("[{{}}] {format_string}");
        Ok(quote! {
          {
            // A limitation of the tokenizer macro is that untyped formats
            // are not supported, so instead of ("{}", x), the following
            // ("{}", x as type) must be used  instead. All the log stmts
            // now all use explicit casts even when using a different backend.
            // So match the behavior of the tokenized logger and also ignore
            // unnecessary casts here.
            #![allow(clippy::unnecessary_cast)]
            extern crate std;
            use std::println;
            println!(#format_string, __pw_log_backend_crate::log_level_tag(#log_level), #(#args),*);
          }
        })
    }

    fn string_fragment(&mut self, _string: &str) -> Result<()> {
        // String fragments are encoded directly into the format string.
        Ok(())
    }

    fn integer_conversion(&mut self, ty: Ident, expression: Arg) -> Result<Option<String>> {
        self.args.push(quote! {((#expression) as #ty)});
        Ok(None)
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.args.push(quote! {((#expression) as &str)});
        Ok(None)
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.args.push(quote! {((#expression) as char)});
        Ok(None)
    }

    fn untyped_conversion(&mut self, expression: Arg) -> Result<()> {
        self.args.push(quote! {(#expression)});
        Ok(())
    }
}

#[proc_macro]
pub fn _pw_log_backend(tokens: TokenStream) -> TokenStream {
    let input = parse_macro_input!(tokens as PwLogArgs<CoreFmtFormatStringParser>);

    let generator = LogfGenerator::new(&input.log_level);

    match generate_core_fmt(generator, input.format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

#[proc_macro]
pub fn _pw_logf_backend(tokens: TokenStream) -> TokenStream {
    let input = parse_macro_input!(tokens as PwLogArgs<PrintfFormatStringParser>);

    let generator = LogfGenerator::new(&input.log_level);

    match generate_core_fmt(generator, input.format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}
