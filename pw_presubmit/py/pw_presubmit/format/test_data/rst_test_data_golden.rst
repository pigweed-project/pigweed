#############
My Test Title
#############

.. note::
   `b/434198083 <https://pwbug.dev/434198083>`__: The golden for this test
   has some unexpected results, and the reST formatter needs to be updated
   to produce a better fixes.

This is a paragraph with some        tabs and trailing spaces.

Here is another paragraph.

*   A list item.

    .. note::
        A nested note with bad indentation.

*   Another list item.

.. code-block:: cpp

   int main() { return 0; }

Here is a code block with options and bad indentation.

  .. code-block:: python
     :caption: A Python Example
     :linenos:

     def hello():
               print("Hello, world!")

This is the end of the file.
