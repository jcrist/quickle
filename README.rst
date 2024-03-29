quickle 🥒
===========

|travis| |pypi| |conda|

**Quickle is no longer maintained**

``quickle`` was an interesting experiment, but I no longer believe this to be a
good idea. For users looking for a fast and flexible serialization library for
Python, I recommend using `msgspec <https://github.com/jcrist/msgspec>`__
instead.  Everything ``quickle`` could do, ``msgspec`` can do better and
faster, all while using standardized protocols (JSON and MessagePack
currently), rather than something Python-specific like Pickle. See `the docs
<https://jcristharif.com/msgspec/>`__ for more information.

The original README is below:

----

``quickle`` is a fast and small serialization format for a subset of Python
types. It's based off of `Pickle
<https://docs.python.org/3/library/pickle.html>`__, but includes several
optimizations and extensions to provide improved performance and security. For
supported types, serializing a message with ``quickle`` can be *~2-10x faster*
than using ``pickle``.

.. image:: https://github.com/jcrist/quickle/raw/master/docs/source/_static/bench-1.png
    :target: https://jcristharif.com/quickle/benchmarks.html

See `the documentation <https://jcristharif.com/quickle/>`_ for more
information.

LICENSE
-------

New BSD. See the
`License File <https://github.com/jcrist/quickle/blob/master/LICENSE>`_.

.. |travis| image:: https://travis-ci.com/jcrist/quickle.svg?branch=master
   :target: https://travis-ci.com/jcrist/quickle
.. |pypi| image:: https://img.shields.io/pypi/v/quickle.svg
   :target: https://pypi.org/project/quickle/
.. |conda| image:: https://img.shields.io/conda/vn/conda-forge/quickle.svg
   :target: https://anaconda.org/conda-forge/quickle
