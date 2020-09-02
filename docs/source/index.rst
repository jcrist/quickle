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

   * `None`
   * `bool`
   * `int`
   * `float`
   * `complex`
   * `str`
   * `bytes`
   * `bytearray`
   * `tuple`
   * `list`
   * `dict`
   * `set`
   * `frozenset`
   * `enum.Enum`
   * `quickle.PickleBuffer`
   * `quickle.Struct`

Highlights
----------

- Quickle is **fast**. :doc:`benchmarks` show it's among the fastest
  serialization methods for Python.
- Quickle is **safe**. :ref:`Unlike pickle <why_not_pickle>`, deserializing a
  user provided message doesn't allow for arbitrary code execution.
- Quickle is **flexible**. Unlike ``msgpack`` or ``json``, Quickle natively
  supports a wide range of Python builtin types.
- Quickle supports :ref:`"schema evolution" <schema-evolution>`. Messages can
  be sent between clients with different schemas without error.

Installation
------------

Quickle can be installed via ``pip``. Note that Python >= 3.8 is required.

.. code-block:: shell

    pip install quickle


.. currentmodule:: quickle


Usage
-----

Like ``pickle``, ``quickle`` exposes two functions `dumps` and `loads`, for
serializing and deserializing objects respectively.

.. code-block:: python

    >>> import quickle
    >>> data = quickle.dumps({"hello": "world"})
    >>> quickle.loads(data)
    {'hello': 'world'}

Note that if you're making multiple calls to `dumps` or `loads`, it's more
efficient to create an `Encoder` and `Decoder` once, and then use
`Encoder.dumps` and `Decoder.loads`.

.. code-block:: python

    >>> enc = quickle.Encoder()
    >>> dec = quickle.Decoder()
    >>> data = enc.dumps({"hello": "world"})
    >>> dec.loads(data)
    {'hello': 'world'}


Structs and Enums
~~~~~~~~~~~~~~~~~

Quickle can serialize most builtin types, but unlike pickle, it can't serialize
arbitrary user classes. This is due to security concerns - deserializing
arbitrary python objects requires executing arbitrary python code.
Deserializing a malicious message could wipe your machine or steal secrets (see
:ref:`why_not_pickle` for more information).

Quickle does support serializing two non-builtin types:

- `Struct`
- `enum.Enum`

Structs are useful for defining structured messages. Fields are defined using
python type annotations (the type annotations themselves are ignored, only the
field names are used). Defaults values can also be specified for any optional
arguments.

Here we define a struct representing a person, with two required fields and two
optional fields.

.. code-block:: python

    >>> class Person(quickle.Struct):
    ...     """A struct describing a person"""
    ...     first : str
    ...     last : str
    ...     address : str = ""
    ...     phone : str = None

Struct types automatically generate a few methods based on the provided type
annotations:

- ``__init__``
- ``__repr__``
- ``__copy__``
- ``__eq__`` & ``__ne__``

.. code-block:: python

    >>> harry = Person("Harry", "Potter", address="4 Privet Drive")
    >>> harry
    Person(first='Harry', last='Potter', address='4 Privet Drive', phone=None)
    >>> harry.first
    "Harry"
    >>> ron = Person("Ron", "Weasley", address="The Burrows")
    >>> ron == harry
    False

It is forbidden to override ``__init__``/``__new__`` in a struct definition,
but other methods can be overridden or added as needed. The struct fields are
available via the ``__struct_fields__`` attribute (a tuple of the fields in
argument order ) if you need them. Here we add a method for converting a struct
to a dict.

.. code-block:: python

    >>> class Point(quickle.Struct):
    ...     """A point in 2D space"""
    ...     x : float
    ...     y : float
    ...     
    ...     def to_dict(self):
    ...         return {f: getattr(self, f) for f in self.__struct_fields__}
    ...
    >>> p = Point(1.0, 2.0)
    >>> p.to_dict()
    {"x": 1.0, "y": 2.0}

Struct types are written in C and are quite speedy and lightweight. They're
great for defining structured messages both for serialization and for use in an
application.

To add serialization support for `Struct` types, you need to register them with
an `Encoder` and `Decoder`.

.. code-block:: python

    >>> enc = quickle.Encoder(registry=[Person, Point])
    >>> dec = quickle.Decoder(registry=[Person, Point])
    >>> data = enc.dumps(harry)
    >>> dec.loads(data)
    Person(first='Harry', last='Potter', address='4 Privet Drive', phone=None)

Unregistered types will fail to serialize or deserialize. Note that for
deserialization to be successful the registry of the `Decoder` must match that
of the `Encoder`.

Like `Struct` types, `enum.Enum` types also need to be registered before
they can be serialized:

.. code-block:: python

    >>> import enum
    >>> class Fruit(enum.IntEnum):
    ...     APPLE = 1
    ...     BANANA = 2
    ...     ORANGE = 3
    ...
    >>> enc = quickle.Encoder(registry=[Fruit])
    >>> dec = quickle.Decoder(registry=[Fruit])
    >>> data = enc.dumps(Fruit.APPLE)
    >>> dec.loads(data)
    <Fruit.APPLE: 1>

.. _schema-evolution:

Schema Evolution
~~~~~~~~~~~~~~~~

Quickle includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines when
defining and registering new `Struct` and `enum.Enum` types:

1. Any new fields on a struct must be added to the end of the struct
   definition, and must contain default values. *Do not reorder fields in a
   struct.*
2. Any new `Struct` or `enum.Enum` types must be appended to the end of the
   registry. *Do not reorder types in the registry.*

For example, suppose we wanted to add a new ``email`` field to our ``Person``
struct. To do so, we add it at the end of the definition, with a default value.

.. code-block:: python

    >>> class Person2(quickle.Struct):
    ...     """A struct describing a person"""
    ...     first : str
    ...     last : str
    ...     address : str = ""
    ...     phone : str = None
    ...     email : str = None  # added at the end, with a default
    ...
    >>> vernon = Person2("Vernon", "Dursley", address="4 Privet Drive", email="vernon@grunnings.com")

Messages serialized using the new and old schemas can still be exchanged
without error.

.. code-block:: python

    >>> old_enc = quickle.Encoder(registry=[Person])
    >>> old_dec = quickle.Decoder(registry=[Person])
    >>> new_enc = quickle.Encoder(registry=[Person2])
    >>> new_dec = quickle.Decoder(registry=[Person2])

    >>> new_msg = new_enc.dumps(vernon)
    >>> old_dec.loads(new_msg)  # deserializing a new msg with an older decoder
    Person(first="Vernon", last="Dursley", address="4 Privet Drive", phone=None)

    >>> old_msg = old_enc.dumps(harry)
    >>> new_dec.loads(old_msg) # deserializing an old msg with a new decoder
    Person2(first='Harry', last='Potter', address='4 Privet Drive', phone=None, email=None)


.. toctree::
    :hidden:
    :maxdepth: 2

    faq.rst
    benchmarks.rst
    api.rst
