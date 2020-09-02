FAQ
===

.. _why_not_pickle:

Why not just use pickle?
------------------------

The builtin pickle_ module
(or other extensions like cloudpickle_) can definitely support more
types, but come with security issues if you're unpickling unknown data. From
`the official docs`_:

.. warning::

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
writing and ~1-3x faster at reading.


Why not msgpack, json, etc?
---------------------------

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
<https://github.com/ijl/orjson>`__ implementations.

That said, if you're writing a network service that needs to talk to non-python
things, ``json`` or ``msgpack`` will definitely serve you better. Even if
you're writing something only in Python, you might still want to consider using
something more standardized like ``json`` or ``msgpack``.

When would I use this?
----------------------

I wanted this for writing RPC-style applications in Python. I was unsatisfied
with ``json`` or ``msgpack``, since they didn't support all the rich types I'm
used to in Python. And the existing pickle implementation added measurable
per-message overhead when writing low-latency applications (not to mention
security issues). If you don't have a similar use case, you may be better
served elsewhere.

.. _pickle: 
.. _the official docs: https://docs.python.org/3/library/pickle.html
.. _cloudpickle: https://github.com/cloudpipe/cloudpickle
