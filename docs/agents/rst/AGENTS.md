# reStructuredText (reST) style guide and conventions

## Formatting

### Line length

In general, 80 columns is the limit.

The following components can exceed 80 columns when it improves readability or
is necessary for some other reason:

* Tables

* Code blocks

### Indentation character

Always use spaces, never tabs.

## Sections

### Section title adornment conventions

reST identifies sections through their titles, which are marked up with
underline and (optional) overline adornment. All Pigweed documentation must
follow these section title adornment conventions:

```rst
==================
H1: Document title
==================

-----------
H2: Section
-----------

H3: Subsection
==============

H4: Subsubsection
-----------------

H5: Subsubsubsection
^^^^^^^^^^^^^^^^^^^^

H6: Subsubsubsubsection
.......................
```

H7 or higher section titles are not allowed.

#### Adornment length

The underline and overline (when present) must match the length of the section
title text that they adorn.

Yes:

```rst
=====
Title
=====

-------
Section
-------

Subsection
==========
```

No (too long):

```rst
========
Title
========

--------
Section
--------

Subsection
===============
```

No (too short):

```rst
====
Title
====

---
Section
---

Subsection
======
```

### Section title whitespace

Do not put a blank line between a section title and the content that follows
it.

Yes:

```rst
=====
Title
=====
Lorem ipsum
```

No:

```rst
=====
Title
=====

Lorem ipsum
```

Put one blank line between the end of content and a following section title.

Yes:

```rst
Lorem ipsum

-------
Section
-------
```

No (too many blank lines):

```rst
Lorem ipsum


-------
Section
-------
```

No (no blank line):

```rst
Lorem ipsum
-------
Section
-------
```

## Directives

### Whitespace

Indent the attributes and content of directives 3 spaces so that they align
with the directive name. Don't put a blank line between the directive name and
its attributes. Put 1 blank line between the directive name (or last attribute)
and the content.

Yes:

```rst
.. foo::
   :attr:

   Content
```

No (attribute and content are indented 2 spaces):

```rst
.. foo::
  :attr:

  Content
```

No (missing blank line between attribute and content):

```rst
.. foo::
   :attr:
   Content
```

## Tables

Do not use [grid tables] or [simple tables]. Use [list-table] (preferred) or
[csv-table].

Yes:

```rst
.. list-table::
   :header-rows: 1

   * - cat
     - dog
   * - green
     - blue
```

No (grid table):

```rst
+-------+------+
| cat   | dog  |
+=======+======+
| green | blue |
+-------+------+
```

No (simple table):

```rst
===== ====
cat   dog
===== ====
green blue
===== ====
```

## Code blocks

Use the `code-block` directive. Do not use the `code` or `sourcecode` aliases.

If Google has a style guide for the programming language in the code block,
then the code should match that style guide.

### Short names

`code-block` requires a [lexer short name](https://pygments.org/languages/):

```rst
.. code-block:: <name>

   …
```

Many languages support 2 or more short names. Always use the following:

* Bazel: `bazel` for `*.bazel` files and `starlark` for `*.bzl` files

* CMake: `cmake`

* C++: `c++`

* General log output, text diagrams, and unformatted text: `output`

* GN: `py`

* Python: `py` for `*.py` scripts and `pycon` for console sessions

* Rust: `rs`

* Shell scripts: `bash` for Bash and `fish` for Fish

* CLI interactions: `console` for Unix and `doscon` for Windows

When in doubt, use the short name that seems most canonical and
inform the user of your uncertainty.

## Links

### Code Search links

Use the custom `:cs:` role when linking to
`cs.opensource.google/pigweed/pigweed`.

#### Examples

Link to a tip-of-tree file or directory:

```rst
:cs:`pw_allocator/allocator.cc`
```

Link to a file or directory at a specific commit:

```rst
:cs:`pw_allocator/allocator.cc
<a18dd872b2c6fd544f96b38b31aafca6b4a0fa7b:pw_allocator/allocator.cc>`
```

Link to a commit:

```rst
:cs:`my commit <a18dd872b2c6fd544f96b38b31aafca6b4a0fa7b>`
```

Link to a specific line within a file at a certain commit:

```rst
:cs:`my line
<a18dd872b2c6fd544f96b38b31aafca6b4a0fa7b:pw_allocator/allocator.cc;l=22>`
```

### Sphinx-to-Doxygen links (Doxylink)

Pigweed's C/C++ API reference is hosted on a Doxygen subsite. To link from a
Sphinx page to a Doxygen page, use the `:cc:` role.

> [!NOTE]
> The `:cc:` role is powered by a fork of Doxylink that Pigweed maintains.

#### Always fully namespace underlying links

When using customized link text, the underlying link must always be fully
namespaced.

Yes:

```rst
:cc:`TinyBlock <pw::allocator::TinyBlock>`
```

`TinyBlock` is the link text and `pw::allocator::TinyBlock` is the underlying
link.

No (the underlying link is relative):

```rst
:cc:`TinyBlock <allocator::TinyBlock>`
```

#### Unqualified symbols

Never allowed.

Yes:

```rst
:cc:`pw::foo::Bar`
```

No:

```rst
:cc:`Bar`
```

If multiple `Bar` symbols exist in different namespaces, Doxylink guesses at
what particular `Bar` it should link to. The docs can become incorrect in
subtle, difficult-to-detect ways.

#### Symbols in the pw namespace

For symbols in the `pw` namespace (e.g. `pw::Status`) always use the fully
qualified name.

Yes:

```rst
:cc:`pw::IntrusiveList`
```

No:

```rst
:cc:`IntrusiveList <pw::IntrusiveList>`
```

#### Symbols with 2 or more levels of namespace

When referring to a symbol within the same namespace, display the unqualified
name.

Yes (when linking from a `pw_channel` doc to a `pw_channel` symbol):

```rst
:cc:`PendRead <pw::channel::AnyChannel::PendRead>`
```

No (when linking from a `pw_channel` doc to a `pw_channel` symbol):

```rst
:cc:`pw::channel::AnyChannel::PendRead`
```

When referring to a symbol from another namespace, use the minimal amount of
namespacing that's required to disambiguate the namespace.

Yes (when linking from a `pw_async2` doc to a `pw_channel` symbol):

```rst
:cc:`channel::AnyChannel::PendRead <pw::channel::AnyChannel::PendRead>`
```

No (when linking from a `pw_async2` doc to a `pw_channel` symbol):

```rst
:cc:`pw::channel::AnyChannel::PendRead`
```

## Iteration loop

To check your work, build the documentation and resolve any errors:

1.  Run `bazelisk build //docs`.

2.  Address any warnings or errors reported by the build.

3.  Repeat until the build is clean.

[grid tables]: https://docutils.sourceforge.io/docs/ref/rst/restructuredtext.html#grid-tables
[simple tables]: https://docutils.sourceforge.io/docs/ref/rst/restructuredtext.html#simple-tables
[list-table]: https://docutils.sourceforge.io/docs/ref/rst/directives.html#list-table
[csv-table]: https://docutils.sourceforge.io/docs/ref/rst/directives.html#csv-table
