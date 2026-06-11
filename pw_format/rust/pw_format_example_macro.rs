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
use pw_format::macros::{
    generate, Arg, FormatAndArgsFlavor, FormatMacroGenerator, FormatParams,
    PrintfFormatStringParser, Result,
};
use quote::quote;
use syn::parse::{Parse, ParseStream};
use syn::{parse_macro_input, Expr, Token};

type TokenStream2 = proc_macro2::TokenStream;

// Declare a struct to hold our proc macro arguments.
#[derive(Debug)]
struct MacroArgs {
    prefix: Expr,
    // Select printf style format strings.
    format_and_args: FormatAndArgsFlavor<PrintfFormatStringParser>,
}

// Implement `Parse` for our argument struct.
impl Parse for MacroArgs {
    fn parse(input: ParseStream) -> syn::parse::Result<Self> {
        // Our prefix argument comes first argument and ends with a `,`.
        let prefix: Expr = input.parse()?;
        input.parse::<Token![,]>()?;

        // Prase the remaining arguments as a format string and arguments.
        let format_and_args: FormatAndArgsFlavor<_> = input.parse()?;

        Ok(MacroArgs {
            prefix,
            format_and_args,
        })
    }
}

// Our generator struct needs to track the prefix as well as the code
// fragments we've generated.
struct Generator {
    prefix: Expr,
    code_fragments: Vec<TokenStream2>,
}

impl Generator {
    pub fn new(prefix: Expr) -> Self {
        Self {
            prefix,
            code_fragments: Vec::new(),
        }
    }
}

// This toy example implements the generator by calling `format!()` at runtime.
impl FormatMacroGenerator for Generator {
    // Wrap all our fragments in boilerplate and return the code.
    fn finalize(self) -> Result<TokenStream2> {
        // Create locally scoped alias so we can refer to them in `quote!()`.
        let prefix = self.prefix;
        let code_fragments = self.code_fragments;

        Ok(quote! {
          {
            // Initialize the result string with our prefix.
            let mut result = String::new();
            result.push_str(#prefix);

            // Enumerate all our code fragments.
            #(#code_fragments);*;

            // Return the resulting string
            result
          }
        })
    }

    // Handles a string embedded in the format string.  This is different from
    // `string_conversion` which is used to handle a string referenced by the
    // format string (i.e. "%s'").
    fn string_fragment(&mut self, string: &str) -> Result<()> {
        self.code_fragments.push(quote! {
            result.push_str(#string);
        });
        Ok(())
    }

    // This example ignores display type and width.
    fn integer_conversion(
        &mut self,
        _params: &FormatParams,
        _signed: bool,
        _type_width: u8, // in bits
        expression: Arg,
    ) -> Result<()> {
        self.code_fragments.push(quote! {
            result.push_str(&format!("{}", #expression));
        });
        Ok(())
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<()> {
        self.code_fragments.push(quote! {
            result.push_str(&format!("{}", #expression));
        });
        Ok(())
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<()> {
        self.code_fragments.push(quote! {
            result.push_str(&format!("{}", #expression));
        });
        Ok(())
    }

    fn untyped_conversion(&mut self, expression: Arg, _params: &FormatParams) -> Result<()> {
        self.code_fragments.push(quote! {
            result.push_str(&format!("{}", #expression));
        });
        Ok(())
    }
}

// Lastly we declare our proc macro.
#[proc_macro]
pub fn example_macro(tokens: TokenStream) -> TokenStream {
    // Parse our proc macro arguments.
    let input = parse_macro_input!(tokens as MacroArgs);

    // Create our generator.
    let generator = Generator::new(input.prefix);

    // Call into `generate()` to handle the code generation.
    match generate(generator, input.format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}
