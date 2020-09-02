Quickle
=======

.. toctree::
   :maxdepth: 2
   :caption: Contents:


``quickle`` is a fast and small serialization format for a subset of Python
types. It's based off of `Pickle
<https://docs.python.org/3/library/pickle.html>`__, but includes several
optimizations and extensions to provide improved performance and security. For
supported types, serializing a message with ``quickle`` can be *~2-10x faster*
than using ``pickle``.

Quickle currently supports serializing the following types:

.. hlist::
   :columns: 4

   * ``None``
   * ``bool``
   * ``int``
   * ``float``
   * ``complex``
   * ``str``
   * ``bytes``
   * ``bytearray``
   * ``tuple``
   * ``list``
   * ``dict``
   * ``set``
   * ``frozenset``
   * ``PickleBuffer``
   * ``quickle.Struct``
   * ``enum.Enum``

.. toctree::
    :hidden:
    :maxdepth: 2

    faq.rst
    api.rst
