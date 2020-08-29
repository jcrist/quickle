quickle 🥒
===========

|travis| |pypi|

.. |travis| image:: https://travis-ci.com/jcrist/quickle.svg?branch=master
   :target: https://travis-ci.com/jcrist/quickle
.. |pypi| image:: https://img.shields.io/pypi/v/quickle.svg
   :target: https://pypi.org/project/quickle/

``quickle`` is a fast and small serialization format for a subset of Python
types. It's based off of `Pickle
<https://docs.python.org/3/library/pickle.html>`__, but includes several
optimizations and extensions to provide improved performance and security. For
supported types, serializing a message with ``quickle`` can be *~2-10x faster*
than using ``pickle``.

Quickle currently supports serializing the following types:

- ``None``
- ``bool``
- ``int``
- ``float``
- ``complex``
- ``str``
- ``bytes``
- ``bytearray``
- ``tuple``
- ``list``
- ``dict``
- ``set``
- ``frozenset``
- ``PickleBuffer``
- ``quickle.Struct``
- ``enum.Enum``

FAQ
---

Why not just use pickle?
~~~~~~~~~~~~~~~~~~~~~~~~

The builtin `pickle <https://docs.python.org/3/library/pickle.html>`__ module
(or other extensions like `cloudpickle
<https://github.com/cloudpipe/cloudpickle>`__) can definitely support more
types, but come with security issues if you're unpickling unknown data. From
the official docs:

  Warning

  The ``pickle`` module **is not secure**. Only unpickle data you trust.

  It is possible to construct malicious pickle data which will **execute
  arbitrary code during unpickling**. Never unpickle data that could have come
  from an untrusted source, or that could have been tampered with.

The pickle protocol contains instructions for loading and executing arbitrary
python code - a maliciously crafted pickle could wipe your machine or steal
secrets. ``quickle`` does away with those instructions, removing that
security issue.

The builtin ``pickle`` module also needs to support multiple protocols, and
includes some optimizations for writing to/reading from files that result in
slowdowns for users wanting fast in-memory performance (as required by
networked services). For common payloads ``quickle`` can be ~2-10x faster at
writing and ~1-3x faster at reading. Here's a quick non-scientific benchmark
(on Python 3.8).

.. code-block:: python

    In [1]: import pickle, quickle

    In [2]: encoder = quickle.Encoder()

    In [3]: data = {"fruit": ["apple", "banana", "cherry", "durian"],
       ...:         "vegetables": ["asparagus", "broccoli", "cabbage"],
       ...:         "numbers": [1, 2, 3, 4, 5]}

    In [4]: %timeit pickle.dumps(data)  # pickle
    955 ns ± 2.97 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [5]: %timeit encoder.dumps(data) # quickle
    481 ns ± 1.76 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [6]: import string, random

    In [7]: data = [''.join(random.choices(string.ascii_letters, k=15)) for _ in range(100)]

    In [8]: %timeit pickle.dumps(data)  # pickle
    5.53 µs ± 35.7 ns per loop (mean ± std. dev. of 7 runs, 100000 loops each)

    In [9]: %timeit encoder.dumps(data)  # quickle
    1.88 µs ± 5.88 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

Why not msgpack, json, etc?
~~~~~~~~~~~~~~~~~~~~~~~~~~~

There are optimized versions of ``msgpack`` and ``json`` for Python that can be
great for similar use cases. However, both ``msgpack`` and ``json`` have
simpler object models than Python, which makes it tricky to roundtrip all the
rich builtin types Python supports.

- Both ``msgpack`` and ``json`` only support a single "array" type, which makes
  it hard to roundtrip messages where you want to distinguish lists from
  tuples. Or sets.
- While ``msgpack`` supports both binary and unicode types, ``json`` requires
  all bytes be encoded into something utf8 compatible.
- Quickle supports "memoization" - if a message contains the same object
  instance multiple times, it will only be serialized once in the payload. For
  messages where this may happen, this can result in a significant reduction in
  payload size. (note that ``quickle`` also contains an option to disable
  memoization if you don't need it, which can result in further speedups).
- Quickle also supports recursive and self-referential objects, which will cause
  recursion errors in other serializers. While uncommon, there are use cases
  for such data structures, and quickle supports them natively.
- With the introduction of the `Pickle 5 protocol
  <https://www.python.org/dev/peps/pep-0574/>`__, Pickle (and Quickle) supports
  sending messages containing large binary payloads in a zero-copy fashion.
  This is hard (or impossible) to do with either ``msgpack`` or ``json``.

``quickle`` is also competitive with common Python `msgpack
<https://github.com/msgpack/msgpack-python>`__ and `json
<https://github.com/ijl/orjson>`__ implementations. Another non-scientific
benchmark:

.. code-block:: python

    In [1]: import quickle, orjson, msgpack

    In [2]: encoder = quickle.Encoder()

    In [3]: packer = msgpack.Packer()

    In [4]: data = {"fruit": ["apple", "banana", "cherry", "durian"],
       ...:         "vegetables": ["asparagus", "broccoli", "cabbage"],
       ...:         "numbers": [1, 2, 3, 4, 5]}

    In [5]: %timeit encoder.dumps(data)  # quickle
    482 ns ± 1.03 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [6]: %timeit packer.pack(data)  # msgpack 
    852 ns ± 3.22 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [7]: %timeit orjson.dumps(data)  # json
    834 ns ± 2.62 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [8]: decoder = quickle.Decoder()

    In [9]: quickle_data = encoder.dumps(data)

    In [10]: msgpack_data = packer.pack(data)

    In [11]: json_data = orjson.dumps(data)

    In [12]: %timeit decoder.loads(quickle_data)  # quickle
    1.16 µs ± 7.33 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [13]: %timeit msgpack.loads(msgpack_data)  # msgpack
    1.07 µs ± 13.4 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [14]: %timeit orjson.loads(json_data)  # json
    1.16 µs ± 3.54 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

That said, if you're writing a network service that needs to talk to non-python
things, ``json`` or ``msgpack`` will definitely serve you better. Even if
you're writing something only in Python, you might still want to consider using
something more standardized like ``json`` or ``msgpack``.

When would I use this?
~~~~~~~~~~~~~~~~~~~~~~

I wanted this for writing RPC-style applications in Python. I was unsatisfied
with ``json`` or ``msgpack``, since they didn't support all the rich types I'm
used to in Python. And the existing pickle implementation added measurable
per-message overhead when writing low-latency applications (not to mention
security issues). If you don't have a similar use case, you may be better
served elsewhere.
