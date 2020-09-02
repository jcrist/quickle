Benchmarks
==========

.. note::

    Benchmarks are *hard*. Repeatedly calling the same function in a tight loop
    will lead to the instruction cache staying hot and branches being highly
    predictable. That's not representative of real world access patterns. It's
    also hard to write a nonbiased benchmark. I wrote quickle, naturally
    whatever benchmark I publish it's going to perform well in.

    Even so, people like to see benchmarks. I've tried to be as nonbiased as I
    can be, and the results hopefully indicate a few tradeoffs you make when
    you choose different serialization formats. I encourage you to write your
    own benchmarks before making these decisions.

Here we show a simple benchmark serializing some structured data. The data
we're serializing has the following schema (defined here using `quickle.Struct`
types):

.. code-block:: python

    import quickle
    from typing import List, Optional

    class Address(quickle.Struct):
        street: str
        state: str
        zip: int


    class Person(quickle.Struct):
        first: str
        last: str
        age: int
        addresses: Optional[List[Address]] = None
        telephone: Optional[str] = None
        email: Optional[str] = None

The libraries we're benchmarking are the following:

- ``msgpack`` - msgpack_ with dict message types
- ``orjson`` - orjson_ with dict message types
- ``pyrobuf`` - pyrobuf_ with protobuf message types
- ``pickle`` - pickle_ with dict message types
- ``pickle tuples`` - pickle_ with `collections.namedtuple` message types
- ``quickle`` - quickle_ with dict message types
- ``quickle structs`` - quickle_ with `quickle.Struct` message types

Each benchmark creates one or more instances of a ``Person`` message, and
serializes it/deserializes it in a loop. The full benchmark code can be found
`here <https://github.com/jcrist/quickle/tree/master/benchmarks>`__.

Benchmark - 1 Object
--------------------

Some workflows involve sending around very small messages. Here the overhead
per function call dominates (parsing of options, allocating temporary buffers,
etc...). Libraries like ``quickle`` and ``msgpack``, where internal structures
are allocated once and can be reused will generally perform better here than
libraries like ``pickle``, where each call needs to allocate some temporary
objects.

.. raw:: html

    <div class="bk-root" id="bench-1"></div>

.. note::

    You can use the radio buttons on the right to sort by total roundtrip time,
    dumps (serialization)  time, loads (deserialization) time, or serialized
    message size.

From the chart above, you can see that ``quickle structs`` is the fastest
method for both serialization and deserialization. It also results in the
second smallest message size (behind ``pyrobuf``). This makes sense, struct
types don't need to serialize the fields in each message (things like
``first``, ``last``, ...), only the values, so there's less data to send
around. Since python is dynamic, each object serialized requires a few pointer
chases, so serializing fewer objects results in faster and smaller messages.

I'm actually surprised at how much overhead ``pyrobuf`` has (the actual
protobuf encoding should be pretty efficient), I suspect there's some
optimizations that could still be done there.

That said, all of these methods serialize/deserialize pretty quickly relative
to other python operations, so unless you're counting every microsecond your
choice here probably doesn't matter that much.


Benchmark - 1000 Objects
------------------------

Here we serialize a list of 1000 ``Person`` objects. There's a lot more data
here, so the per-call overhead will no longer dominate, and we're now measuring
the efficiency of the encoding/decoding.

.. raw:: html

    <div class="bk-root" id="bench-1k"></div>

As with before ``quickle structs`` and ``quickle`` both perform well here.
What's interesting is that ``msgpack`` and ``orjson`` have now moved to the
back for deserialization time.

The reason for this is *memoization*. Since each message here is structured
(all dicts have the same keys), ``msgpack`` and ``orjson`` are serializing the
same strings multiple times. In contrast, ``quickle`` and ``pickle`` both
support memoization - identical objects in a message will only be serialized
once, and then referenced later on. This results in smaller messages and faster
deserialization times. For messages without repeat objects, memoization is an
added cost you don't need.  But as soon as you get more than a handful of
repeat objects, the performance win becomes important. 

Note that ``quickle structs``, ``pickle tuples``, and ``pyrobuf`` don't require
memoization to be efficient here, as the repeated field names aren't serialized
as part of the message.


Benchmark - 10,000 Objects
--------------------------

Here we run the same benchmark as before, but 10,000 ``Person`` objects.

.. raw:: html

    <div class="bk-root" id="bench-10k"></div>

Like the 1000 object benchmark, the cost of serializing/deserializing repeated
strings dominate for the ``orjson`` and ``msgpack`` benchmarks.


.. raw:: html

    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-2.1.1.min.js" integrity="sha384-kLr4fYcqcSpbuI95brIH3vnnYCquzzSxHPU6XGQCIkQRGJwhg0StNbj1eegrHs12" crossorigin="anonymous"></script>
    <script type="text/javascript" src="https://cdn.bokeh.org/bokeh/release/bokeh-widgets-2.1.1.min.js" integrity="sha384-xIGPmVtaOm+z0BqfSOMn4lOR6ciex448GIKG4eE61LsAvmGj48XcMQZtKcE/UXZe" crossorigin="anonymous"></script>
    <script>
    fetch('_static/bench-1.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1') })
    fetch('_static/bench-1k.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-1k') })
    fetch('_static/bench-10k.json')
        .then(function(response) { return response.json() })
        .then(function(item) { return Bokeh.embed.embed_item(item, 'bench-10k') })
    </script>


.. _msgpack: https://github.com/msgpack/msgpack-python
.. _orjson: https://github.com/ijl/orjson
.. _pyrobuf: https://github.com/appnexus/pyrobuf
.. _pickle: https://docs.python.org/3/library/pickle.html
.. _quickle: https://jcristharif.com/quickle/
