smolpickle 🥒
=============

*Like `pickle`, but smol.*

``smolpickle`` is an implementation of `pickle
<https://docs.python.org/3/library/pickle.html>`__ that only supports builtin
types. Specifically, only the following types (and *not* subclasses) are
supported:

- ``None``
- ``bool``
- ``int``
- ``float``
- ``str``
- ``bytes``
- ``bytearray``
- ``tuple``
- ``list``
- ``dict``
- ``set``
- ``frozenset``
- ``PickleBuffer``

It also only supports pickle `protocol 5
<https://www.python.org/dev/peps/pep-0574/>`__ (and up, once new versions are
released).

The ``smolpickle.dumps`` and ``smolpickle.loads`` methods should be drop-in
compatible with ``pickle.dumps`` and ``pickle.loads`` (for supported types).
Further, pickles written by ``smolpickle`` are readable using ``pickle`` -
``smolpickle`` *is* ``pickle``, just smaller.

``smolpickle`` is intended mainly for writing networked services, as such the
implementation is optimized for writing to in-memory streams. The
``smolpickle.Pickler`` and ``smolpickle.Unpickler`` classes are *not* drop-in
compatible with those provided on the ``pickle`` module. If you're doing a lot
of repeated calls to ``dumps``/``loads``, it's recommended to create a
``Pickler``/``Unpickler`` and use the corresponding objects on these classes -
you'll get a nice performance boost from doing so.

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
secrets. ``smolpickle`` does away with those instructions, removing that
security issue.

The builtin ``pickle`` module also needs to support multiple protocols, and
includes some optimizations for writing to/reading from files that result in
slowdowns for users wanting fast in-memory performance (as required by
networked services). For common payloads ``smolpickle`` can be 2-3X faster at
writing and marginally faster at reading. Here's a quick non-scientific
benchmark (on Python 3.8).

.. code-block:: python

    In [1]: import pickle, smolpickle

    In [2]: pickler = smolpickle.Pickler()

    In [3]: data = {"fruit": ["apple", "banana", "cherry", "durian"],
       ...:         "vegetables": ["asparagus", "broccoli", "cabbage"],
       ...:         "numbers": [1, 2, 3, 4, 5]}

    In [4]: %timeit pickle.dumps(data)  # pickle
    955 ns ± 2.97 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [5]: %timeit pickler.dumps(data) # smolpickle
    481 ns ± 1.76 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [6]: import string, random

    In [7]: data = [''.join(random.choices(string.ascii_letters, k=15)) for _ in range(100)]

    In [8]: %timeit pickle.dumps(data)  # pickle
    5.53 µs ± 35.7 ns per loop (mean ± std. dev. of 7 runs, 100000 loops each)

    In [9]: %timeit pickler.dumps(data)  # smolpickle
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
- Pickle supports "memoization" - if a message contains the same object
  instance multiple times, it will only be serialized once in the payload. For
  messages where this may happen, this can result in a significant reduction in
  payload size. (note that ``smolpickle`` also contains an option to disable
  memoization if you don't need it, which can result in further speedups).
- Pickle also supports recursive and self-referential objects, which will cause
  recursion errors in other serializers. While uncommon, there are use cases
  for such data structures, and pickle supports them natively.
- With the introduction of the `Pickle 5 protocol
  <https://www.python.org/dev/peps/pep-0574/>`__, Pickle supports sending
  messages containing large binary payloads in a zero-copy fashion. This is
  hard (or impossible) to do with either ``msgpack`` or ``json``.

``smolpickle`` is also competitive with common Python `msgpack
<https://github.com/msgpack/msgpack-python>`__ and `json
<https://github.com/ijl/orjson>`__ implementations. Another non-scientific
benchmark:

.. code-block:: python

    In [1]: import smolpickle, orjson, msgpack

    In [2]: pickler = smolpickle.Pickler()

    In [3]: packer = msgpack.Packer()

    In [4]: data = {"fruit": ["apple", "banana", "cherry", "durian"],
       ...:         "vegetables": ["asparagus", "broccoli", "cabbage"],
       ...:         "numbers": [1, 2, 3, 4, 5]}

    In [5]: %timeit pickler.dumps(data)  # smolpickle
    482 ns ± 1.03 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [6]: %timeit packer.pack(data)  # msgpack 
    852 ns ± 3.22 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [7]: %timeit orjson.dumps(data)  # json
    834 ns ± 2.62 ns per loop (mean ± std. dev. of 7 runs, 1000000 loops each)

    In [8]: unpickler = smolpickle.Unpickler()

    In [9]: pickle_data = pickler.dumps(data)

    In [10]: msgpack_data = packer.pack(data)

    In [11]: json_data = orjson.dumps(data)

    In [12]: %timeit unpickler.loads(pickle_data)  # smolpickle
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
