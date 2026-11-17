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
    generate, generate_core_fmt, generate_printf, Arg, CoreFmtFormatMacroGenerator,
    CoreFmtFormatStringParser, FormatAndArgsFlavor, FormatMacroGenerator, FormatParams,
    FormatStringParser, PrintfFormatMacroGenerator, PrintfFormatStringFragment,
    PrintfFormatStringParser, Result,
};
use quote::{quote, ToTokens};
use syn::parse_macro_input;

type TokenStream2 = proc_macro2::TokenStream;

// Generator for testing `generate()`.
struct TestGenerator {
    code_fragments: Vec<TokenStream2>,
}

impl TestGenerator {
    pub fn new() -> Self {
        Self {
            code_fragments: Vec::new(),
        }
    }
}

impl FormatMacroGenerator for TestGenerator {
    fn finalize(self) -> Result<TokenStream2> {
        let code_fragments = self.code_fragments;

        Ok(quote! {
          {
            use pw_format_test_macros_test::TestGeneratorOps;
            let mut ops = Vec::new();
            #(#code_fragments);*;
            ops.push(TestGeneratorOps::Finalize);
            ops
          }
        })
    }

    fn string_fragment(&mut self, string: &str) -> Result<()> {
        self.code_fragments.push(quote! {
            ops.push(TestGeneratorOps::StringFragment(#string.to_string()));
        });
        Ok(())
    }

    // This example ignores display type and width.
    fn integer_conversion(
        &mut self,
        params: &FormatParams,
        signed: bool,
        type_width: u8, // in bits
        expression: Arg,
    ) -> Result<()> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(TestGeneratorOps::IntegerConversion{
                params: #params,
                signed: #signed,
                type_width: #type_width,
                arg: #arg.to_string(),
            });
        });
        Ok(())
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<()> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(TestGeneratorOps::StringConversion(#arg.to_string()));
        });
        Ok(())
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<()> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(TestGeneratorOps::CharConversion(#arg.to_string()));
        });
        Ok(())
    }

    fn untyped_conversion(&mut self, expression: Arg, _params: &FormatParams) -> Result<()> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(TestGeneratorOps::UntypedConversion(#arg.to_string()));
        });
        Ok(())
    }
}

fn generator_test_macro_impl<T: FormatStringParser>(tokens: TokenStream) -> TokenStream {
    let format_and_args = parse_macro_input!(tokens as FormatAndArgsFlavor<T>);
    let generator = TestGenerator::new();
    match generate(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

#[proc_macro]
pub fn printf_format_generator_test_macro(tokens: TokenStream) -> TokenStream {
    generator_test_macro_impl::<PrintfFormatStringParser>(tokens)
}

#[proc_macro]
pub fn core_fmt_format_generator_test_macro(tokens: TokenStream) -> TokenStream {
    generator_test_macro_impl::<CoreFmtFormatStringParser>(tokens)
}

/// Converts an [`Arg`] to its string token representation for test operations.
fn arg_to_string(arg: &Arg) -> String {
    arg.to_token_stream().to_string()
}

/// Shared operation recorder for [`PrintfTestGenerator`] and [`CoreFmtTestGenerator`].
#[derive(Default)]
struct TestOpsRecorder {
    /// Accumulated token streams pushing recorded [`PrintfTestGeneratorOps`] entries.
    code_fragments: Vec<TokenStream2>,
    /// Optional format specifier override returned by `integer_conversion`.
    integer_specifier_override: Option<String>,
    /// Optional format specifier override returned by `string_conversion`.
    string_specifier_override: Option<String>,
    /// Optional format specifier override returned by `char_conversion`.
    char_specifier_override: Option<String>,
}

impl TestOpsRecorder {
    fn new() -> Self {
        Self::default()
    }

    fn with_integer_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.integer_specifier_override = Some(specifier.into());
        self
    }

    fn with_string_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.string_specifier_override = Some(specifier.into());
        self
    }

    fn with_char_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.char_specifier_override = Some(specifier.into());
        self
    }

    fn string_fragment(&mut self, string: &str) -> Result<()> {
        self.code_fragments.push(quote! {
            ops.push(PrintfTestGeneratorOps::StringFragment(#string.to_string()));
        });
        Ok(())
    }

    fn integer_conversion(&mut self, ty: Ident, expression: Arg) -> Result<Option<String>> {
        let ty_str = ty.to_string();
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(PrintfTestGeneratorOps::IntegerConversion{
               ty: #ty_str.to_string(),
               arg: #arg.to_string(),
            });
        });
        Ok(self.integer_specifier_override.clone())
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(PrintfTestGeneratorOps::StringConversion(#arg.to_string()));
        });
        Ok(self.string_specifier_override.clone())
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(PrintfTestGeneratorOps::CharConversion(#arg.to_string()));
        });
        Ok(self.char_specifier_override.clone())
    }

    fn untyped_conversion(&mut self, expression: Arg) -> Result<()> {
        let arg = arg_to_string(&expression);
        self.code_fragments.push(quote! {
            ops.push(PrintfTestGeneratorOps::UntypedConversion(#arg.to_string()));
        });
        Ok(())
    }
}

/// Generator for testing `generate_printf()`. Allows control over the return
/// value of the conversion functions.
#[derive(Default)]
struct PrintfTestGenerator {
    /// Shared operation recorder.
    inner: TestOpsRecorder,
}

impl PrintfTestGenerator {
    pub fn new() -> Self {
        Self {
            inner: TestOpsRecorder::new(),
        }
    }

    pub fn with_integer_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_integer_specifier(specifier);
        self
    }

    pub fn with_string_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_string_specifier(specifier);
        self
    }

    pub fn with_char_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_char_specifier(specifier);
        self
    }
}

impl PrintfFormatMacroGenerator for PrintfTestGenerator {
    fn finalize(
        self,
        format_string_fragments: &[PrintfFormatStringFragment],
    ) -> Result<TokenStream2> {
        // Create locally scoped alias so we can refer to them in `quote!()`.
        let code_fragments = self.inner.code_fragments;
        let format_string_pieces: Vec<_> = format_string_fragments
            .iter()
            .map(|fragment| fragment.as_token_stream("pw_format_core"))
            .collect::<Result<Vec<_>>>()?;

        Ok(quote! {
          {
            use pw_format_test_macros_test::PrintfTestGeneratorOps;
            let mut ops = Vec::new();
            let format_string = pw_bytes::concat_static_strs!(#(#format_string_pieces),*);
            #(#code_fragments);*;
            ops.push(PrintfTestGeneratorOps::Finalize);
            (format_string, ops)
          }
        })
    }

    fn string_fragment(&mut self, string: &str) -> Result<()> {
        self.inner.string_fragment(string)
    }

    fn integer_conversion(&mut self, ty: Ident, expression: Arg) -> Result<Option<String>> {
        self.inner.integer_conversion(ty, expression)
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.inner.string_conversion(expression)
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.inner.char_conversion(expression)
    }

    fn untyped_conversion(&mut self, expression: Arg) -> Result<()> {
        self.inner.untyped_conversion(expression)
    }
}

fn printf_generator_test_macro_impl<T: FormatStringParser>(tokens: TokenStream) -> TokenStream {
    let format_and_args = parse_macro_input!(tokens as FormatAndArgsFlavor<T>);
    let generator = PrintfTestGenerator::new();
    match generate_printf(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

#[proc_macro]
pub fn printf_format_printf_generator_test_macro(tokens: TokenStream) -> TokenStream {
    printf_generator_test_macro_impl::<PrintfFormatStringParser>(tokens)
}

#[proc_macro]
pub fn core_fmt_format_printf_generator_test_macro(tokens: TokenStream) -> TokenStream {
    printf_generator_test_macro_impl::<CoreFmtFormatStringParser>(tokens)
}

// Causes the generator to substitute %d with %K.
#[proc_macro]
pub fn integer_sub_printf_format_printf_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    let generator = PrintfTestGenerator::new().with_integer_specifier("%K");
    match generate_printf(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

// Causes the generator to substitute %s with %K.
#[proc_macro]
pub fn string_sub_printf_format_printf_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    let generator = PrintfTestGenerator::new().with_string_specifier("%K");
    match generate_printf(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

// Causes the generator to substitute %c with %K.
#[proc_macro]
pub fn char_sub_printf_format_printf_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    let generator = PrintfTestGenerator::new().with_char_specifier("%K");
    match generate_printf(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

/// Generator for testing `generate_core_fmt()`. Allows control over the return
/// value of the conversion functions.
#[derive(Default)]
struct CoreFmtTestGenerator {
    /// Shared operation recorder.
    inner: TestOpsRecorder,
}

impl CoreFmtTestGenerator {
    pub fn new() -> Self {
        Self {
            inner: TestOpsRecorder::new(),
        }
    }

    pub fn with_integer_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_integer_specifier(specifier);
        self
    }

    pub fn with_string_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_string_specifier(specifier);
        self
    }

    pub fn with_char_specifier(mut self, specifier: impl Into<String>) -> Self {
        self.inner = self.inner.with_char_specifier(specifier);
        self
    }
}

// Until they diverge, we reuse `PrintfTestGeneratorOps` here.
impl CoreFmtFormatMacroGenerator for CoreFmtTestGenerator {
    fn finalize(self, format_string: String) -> Result<TokenStream2> {
        // Create locally scoped alias so we can refer to them in `quote!()`.
        let code_fragments = self.inner.code_fragments;

        Ok(quote! {
          {
            use pw_format_test_macros_test::PrintfTestGeneratorOps;
            let mut ops = Vec::new();
            #(#code_fragments);*;
            ops.push(PrintfTestGeneratorOps::Finalize);
            (#format_string, ops)
          }
        })
    }

    fn string_fragment(&mut self, string: &str) -> Result<()> {
        self.inner.string_fragment(string)
    }

    fn integer_conversion(&mut self, ty: Ident, expression: Arg) -> Result<Option<String>> {
        self.inner.integer_conversion(ty, expression)
    }

    fn string_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.inner.string_conversion(expression)
    }

    fn char_conversion(&mut self, expression: Arg) -> Result<Option<String>> {
        self.inner.char_conversion(expression)
    }

    fn untyped_conversion(&mut self, expression: Arg) -> Result<()> {
        self.inner.untyped_conversion(expression)
    }
}

fn core_fmt_generator_test_macro_impl<T: FormatStringParser>(tokens: TokenStream) -> TokenStream {
    let format_and_args = parse_macro_input!(tokens as FormatAndArgsFlavor<T>);
    let generator = CoreFmtTestGenerator::new();
    match generate_core_fmt(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

#[proc_macro]
pub fn printf_format_core_fmt_generator_test_macro(tokens: TokenStream) -> TokenStream {
    core_fmt_generator_test_macro_impl::<PrintfFormatStringParser>(tokens)
}

#[proc_macro]
pub fn core_fmt_format_core_fmt_generator_test_macro(tokens: TokenStream) -> TokenStream {
    core_fmt_generator_test_macro_impl::<CoreFmtFormatStringParser>(tokens)
}

// Causes the generator to substitute {} with {:?}.
#[proc_macro]
pub fn integer_sub_printf_format_core_fmt_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    #[allow(clippy::literal_string_with_formatting_args)]
    let generator = CoreFmtTestGenerator::new().with_integer_specifier("{:?}");
    match generate_core_fmt(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

// Causes the generator to substitute {} with {:?}.
#[proc_macro]
pub fn string_sub_printf_format_core_fmt_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    #[allow(clippy::literal_string_with_formatting_args)]
    let generator = CoreFmtTestGenerator::new().with_string_specifier("{:?}");
    match generate_core_fmt(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}

// Causes the generator to substitute {} with {:?}.
#[proc_macro]
pub fn char_sub_printf_format_core_fmt_generator_test_macro(tokens: TokenStream) -> TokenStream {
    let format_and_args =
        parse_macro_input!(tokens as FormatAndArgsFlavor<PrintfFormatStringParser>);
    #[allow(clippy::literal_string_with_formatting_args)]
    let generator = CoreFmtTestGenerator::new().with_char_specifier("{:?}");
    match generate_core_fmt(generator, format_and_args.into()) {
        Ok(token_stream) => token_stream.into(),
        Err(e) => e.to_compile_error().into(),
    }
}
