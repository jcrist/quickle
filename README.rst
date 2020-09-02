quickle 🥒
===========

|travis| |pypi|

``quickle`` is a fast and small serialization format for a subset of Python
types. It's based off of `Pickle
<https://docs.python.org/3/library/pickle.html>`__, but includes several
optimizations and extensions to provide improved performance and security. For
supported types, serializing a message with ``quickle`` can be *~2-10x faster*
than using ``pickle``.

.. raw:: html

    <a href="https://jcristharif.com/quickle/benchmarks.html">
        <img alt="Benchmark" src="https://github.com/jcrist/quickle/raw/master/docs/source/_static/bench-1.png" style="max-width:80%;display:block;margin-left:auto;margin-right:auto">
    </a>

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
