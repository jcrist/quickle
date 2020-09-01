import enum
import itertools
import pickle
import pickletools
import string
import sys
import uuid
from distutils.version import StrictVersion

import pytest

import quickle


BATCHSIZE = 1000


def test_picklebuffer_is_shared():
    assert pickle.PickleBuffer is quickle.PickleBuffer


def test_module_version():
    StrictVersion(quickle.__version__)


def check(obj, sol=None):
    if sol is None:
        sol = obj

    quick_res = quickle.dumps(obj)
    obj2 = quickle.loads(quick_res)
    assert obj2 == sol
    assert type(obj2) is type(sol)

    obj3 = pickle.loads(quick_res)
    assert obj3 == sol
    assert type(obj3) is type(sol)

    pickle_res = pickle.dumps(obj, protocol=5)
    obj4 = quickle.loads(pickle_res)
    assert obj4 == sol
    assert type(obj4) is type(sol)


def test_pickle_none():
    check(None)


@pytest.mark.parametrize("value", [True, False])
def test_pickle_bool(value):
    check(value)


@pytest.mark.parametrize("nbytes", [1, 2, 4, 8, 254, 255, 256, 257])
@pytest.mark.parametrize("negative", [False, True])
def test_pickle_int(nbytes, negative):
    value = 2 ** (nbytes * 8 - 6)
    if negative:
        value *= -1

    check(value)


@pytest.mark.parametrize(
    "value",
    [
        0.0,
        4.94e-324,
        1e-310,
        7e-308,
        6.626e-34,
        0.1,
        0.5,
        3.14,
        263.44582062374053,
        6.022e23,
        1e30,
    ],
)
@pytest.mark.parametrize("negative", [False, True])
def test_pickle_float(value, negative):
    if negative:
        value *= -1

    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_bytes(nbytes):
    value = b"y" * nbytes
    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_bytearray(nbytes):
    value = bytearray(b"y" * nbytes)
    check(value)


@pytest.mark.parametrize("nbytes", [0, 10, 512])
def test_pickle_unicode(nbytes):
    value = "y" * nbytes
    check(value)


@pytest.mark.parametrize(
    "value",
    ["<\\u>", "<\\\u1234>", "<\n>", "<\\>", "\U00012345", "<\\\U00012345>", "<\udc80>"],
)
def test_pickle_unicode_edgecases(value):
    check(value)


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_set(n):
    check(set(range(n)))


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_frozenset(n):
    check(frozenset(range(n)))


@pytest.mark.parametrize("n", [0, 1, 2, 3, 100, BATCHSIZE + 10])
def test_pickle_tuple(n):
    check(tuple(range(n)))


def test_pickle_recursive_tuple():
    obj = ([None],)
    obj[0][0] = obj

    quick_res = quickle.dumps(obj)
    for loads in [quickle.loads, pickle.loads]:
        obj2 = loads(quick_res)
        assert isinstance(obj2, tuple)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0][0] = None
        assert obj2 == ([None],)


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_list(n):
    check(list(range(n)))


def test_pickle_recursive_list():
    # self referential
    obj = []
    obj.append(obj)

    quick_res = quickle.dumps(obj)
    for loads in [quickle.loads, pickle.loads]:
        obj2 = loads(quick_res)
        assert isinstance(obj2, list)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = [[None]]
    obj[0][0] = obj

    quick_res = quickle.dumps(obj)
    for loads in [quickle.loads, pickle.loads]:
        obj2 = loads(quick_res)
        assert isinstance(obj2, list)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0][0] = None
        assert obj2 == [[None]]


@pytest.mark.parametrize("n", [0, 1, 5, 100, BATCHSIZE + 10])
def test_pickle_dict(n):
    value = dict(
        zip(itertools.product(string.ascii_letters, string.ascii_letters), range(n))
    )
    check(value)


def test_pickle_recursive_dict():
    # self referential
    obj = {}
    obj[0] = obj

    quick_res = quickle.dumps(obj)
    for loads in [quickle.loads, pickle.loads]:
        obj2 = loads(quick_res)
        assert isinstance(obj2, dict)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = {0: []}
    obj[0].append(obj)

    quick_res = quickle.dumps(obj)
    for loads in [quickle.loads, pickle.loads]:
        obj2 = loads(quick_res)
        assert isinstance(obj2, dict)
        assert obj2[0][0] is obj2
        # Fix the cycle so `==` works, then test
        obj2[0].pop()
        assert obj2 == {0: []}


def test_pickle_highly_nested_list():
    obj = []
    for _ in range(66):
        obj = [obj]
    check(obj)


def test_pickle_large_memo():
    obj = [[1, 2, 3] for _ in range(2000)]
    check(obj)


def test_pickle_a_little_bit_of_everything():
    obj = [
        1,
        1.5,
        True,
        False,
        None,
        "hello",
        b"hello",
        bytearray(b"hello"),
        (1, 2, 3),
        [1, 2, 3],
        {"hello": "world"},
        {1, 2, 3},
        frozenset([1, 2, 3]),
    ]
    check(obj)


def opcode_in_pickle(code, pickle):
    for op, _, _ in pickletools.genops(pickle):
        if op.code == code.decode("latin-1"):
            return True
    return False


@pytest.mark.parametrize("memoize", [True, False])
def test_pickle_memoize_class_setting(memoize):
    obj = [[1], [2]]

    enc = quickle.Encoder(memoize=memoize)
    assert enc.memoize == memoize

    # immutable
    with pytest.raises(AttributeError):
        enc.memoize = not memoize
    assert enc.memoize == memoize

    # default taken from class
    res = enc.dumps(obj)
    assert opcode_in_pickle(pickle.MEMOIZE, res) == memoize
    assert enc.memoize == memoize

    # specify None, no change
    res = enc.dumps(obj, memoize=None)
    assert opcode_in_pickle(pickle.MEMOIZE, res) == memoize
    assert enc.memoize == memoize

    # specify same, no change
    res = enc.dumps(obj, memoize=memoize)
    assert opcode_in_pickle(pickle.MEMOIZE, res) == memoize
    assert enc.memoize == memoize

    # overridden by opposite value
    res = enc.dumps(obj, memoize=(not memoize))
    assert opcode_in_pickle(pickle.MEMOIZE, res) != memoize
    assert enc.memoize == memoize


@pytest.mark.parametrize("memoize", [True, False])
def test_pickle_memoize_function_settings(memoize):
    obj = [[1], [2]]

    res = quickle.dumps(obj, memoize=memoize)
    assert opcode_in_pickle(pickle.MEMOIZE, res) == memoize
    obj2 = quickle.loads(res)
    assert obj == obj2

    obj = [[]] * 2
    res = quickle.dumps(obj, memoize=memoize)
    assert opcode_in_pickle(pickle.MEMOIZE, res) == memoize
    obj2 = quickle.loads(res)
    assert obj == obj2
    assert (obj2[0] is not obj2[1]) == (not memoize)


def test_pickle_memoize_false_recursion_error():
    obj = []
    obj.append(obj)
    with pytest.raises(RecursionError):
        quickle.dumps(obj, memoize=False)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_pickle_picklebuffer_no_callback(cls):
    sol = cls(b"hello")
    obj = quickle.PickleBuffer(sol)
    check(obj, sol)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_pickler_collect_buffers_true(cls):
    data = cls(b"hello")
    pbuf = quickle.PickleBuffer(data)

    enc = quickle.Encoder(collect_buffers=True)
    assert enc.collect_buffers

    with pytest.raises(AttributeError):
        enc.collect_buffers = False

    # No buffers present returns None
    res, buffers = enc.dumps(data)
    assert buffers is None
    assert quickle.loads(res) == data

    # Buffers are collected and returned
    res, buffers = enc.dumps(pbuf)
    assert buffers == [pbuf]
    assert quickle.loads(res, buffers=buffers) is pbuf

    # Override None uses default
    res, buffers = enc.dumps(pbuf, collect_buffers=None)
    assert buffers == [pbuf]
    assert quickle.loads(res, buffers=buffers) is pbuf

    # Override True is same as default
    res, buffers = enc.dumps(pbuf, collect_buffers=True)
    assert buffers == [pbuf]
    assert quickle.loads(res, buffers=buffers) is pbuf

    # Override False disables buffer collecting
    res = enc.dumps(pbuf, collect_buffers=False)
    assert quickle.loads(res) == data

    # Override doesn't persist
    res, buffers = enc.dumps(pbuf)
    assert buffers == [pbuf]
    assert quickle.loads(res, buffers=buffers) is pbuf


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_pickler_collect_buffers_false(cls):
    data = cls(b"hello")
    pbuf = quickle.PickleBuffer(data)

    enc = quickle.Encoder(collect_buffers=False)
    assert not enc.collect_buffers

    with pytest.raises(AttributeError):
        enc.collect_buffers = True

    # By default buffers are serialized in-band
    res = enc.dumps(pbuf)
    assert quickle.loads(res) == data

    # Override None uses default
    res = enc.dumps(pbuf, collect_buffers=None)
    assert quickle.loads(res) == data

    # Override False is the same as default
    res = enc.dumps(pbuf, collect_buffers=False)
    assert quickle.loads(res) == data

    # Override True works
    res, buffers = enc.dumps(pbuf, collect_buffers=True)
    assert buffers == [pbuf]
    assert quickle.loads(res, buffers=buffers) is pbuf

    # If no buffers present, output is None
    res, buffers = enc.dumps(data, collect_buffers=True)
    assert buffers is None
    assert quickle.loads(res, buffers=buffers) == data

    # Override doesn't persist
    res = enc.dumps(pbuf)
    assert quickle.loads(res) == data


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_quickle_pickle_collect_buffers_true_compatibility(cls):
    data = cls(b"hello")
    pbuf = quickle.PickleBuffer(data)

    # quickle -> pickle
    quick_res, quick_buffers = quickle.dumps(pbuf, collect_buffers=True)
    obj = pickle.loads(quick_res, buffers=quick_buffers)
    assert obj is pbuf

    # pickle -> quickle
    pickle_buffers = []
    pickle_res = pickle.dumps(pbuf, buffer_callback=pickle_buffers.append, protocol=5)
    obj = quickle.loads(pickle_res, buffers=pickle_buffers)
    assert obj is pbuf


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_quickle_pickle_collect_buffers_false_compatibility(cls):
    data = cls(b"hello")
    pbuf = quickle.PickleBuffer(data)

    # quickle -> pickle
    quick_res = quickle.dumps(pbuf)
    obj = pickle.loads(quick_res)
    assert obj == data

    # pickle -> quickle
    pickle_res = pickle.dumps(pbuf, protocol=5)
    obj = quickle.loads(pickle_res)
    assert obj == data


def test_loads_buffers_errors():
    obj = quickle.PickleBuffer(b"hello")
    res, _ = quickle.dumps(obj, collect_buffers=True)

    with pytest.raises(TypeError):
        quickle.loads(res, buffers=object())

    with pytest.raises(quickle.DecodingError):
        quickle.loads(res, buffers=[])


@pytest.mark.parametrize("value", [object(), object, sum, itertools.count])
def test_dumps_and_loads_unpickleable_types(value):
    with pytest.raises(TypeError):
        quickle.dumps(value)

    o = pickle.dumps(value, protocol=5)

    with pytest.raises(quickle.DecodingError):
        quickle.loads(o)


def test_loads_truncated_input():
    data = quickle.dumps([1, 2, 3])
    with pytest.raises(quickle.DecodingError):
        quickle.loads(data[:-2])


def test_loads_bad_pickle():
    with pytest.raises(quickle.DecodingError):
        quickle.loads(b"this isn't valid at all")


def test_getsizeof():
    a = sys.getsizeof(quickle.Encoder(write_buffer_size=64))
    b = sys.getsizeof(quickle.Encoder(write_buffer_size=128))
    assert b > a
    # Smoketest
    sys.getsizeof(quickle.Decoder())


@pytest.mark.parametrize(
    "enc",
    [
        # bad stacks
        b".",  # STOP
        b"0",  # POP
        b"1",  # POP_MARK
        b"a",  # APPEND
        b"Na",
        b"e",  # APPENDS
        b"(e",
        b"s",  # SETITEM
        b"Ns",
        b"NNs",
        b"t",  # TUPLE
        b"u",  # SETITEMS
        b"(u",
        b"}(Nu",
        b"\x85",  # TUPLE1
        b"\x86",  # TUPLE2
        b"N\x86",
        b"\x87",  # TUPLE3
        b"N\x87",
        b"NN\x87",
        b"\x90",  # ADDITEMS
        b"(\x90",
        b"\x91",  # FROZENSET
        b"\x94",  # MEMOIZE
        # bad marks
        b"N(.",  # STOP
        b"]N(a",  # APPEND
        b"}NN(s",  # SETITEM
        b"}N(Ns",
        b"}(NNs",
        b"}((u",  # SETITEMS
        b"N(\x85",  # TUPLE1
        b"NN(\x86",  # TUPLE2
        b"N(N\x86",
        b"NNN(\x87",  # TUPLE3
        b"NN(N\x87",
        b"N(NN\x87",
        b"]((\x90",  # ADDITEMS
        b"N(\x94",  # MEMOIZE
    ],
)
def test_bad_stack_or_mark(enc):
    with pytest.raises(quickle.DecodingError):
        quickle.loads(enc)


@pytest.mark.parametrize(
    "enc",
    [
        b"B",  # BINBYTES
        b"B\x03\x00\x00",
        b"B\x03\x00\x00\x00",
        b"B\x03\x00\x00\x00ab",
        b"C",  # SHORT_BINBYTES
        b"C\x03",
        b"C\x03ab",
        b"G",  # BINFLOAT
        b"G\x00\x00\x00\x00\x00\x00\x00",
        b"J",  # BININT
        b"J\x00\x00\x00",
        b"K",  # BININT1
        b"M",  # BININT2
        b"M\x00",
        b"T",  # BINSTRING
        b"T\x03\x00\x00",
        b"T\x03\x00\x00\x00",
        b"T\x03\x00\x00\x00ab",
        b"U",  # SHORT_BINSTRING
        b"U\x03",
        b"U\x03ab",
        b"X",  # BINUNICODE
        b"X\x03\x00\x00",
        b"X\x03\x00\x00\x00",
        b"X\x03\x00\x00\x00ab",
        b"Nh",  # BINGET
        b"Nj",  # LONG_BINGET
        b"Nj\x00\x00\x00",
        b"Nr\x00\x00\x00",
        b"\x80",  # PROTO
        b"\x8a",  # LONG1
        b"\x8b",  # LONG4
        b"\x8b\x00\x00\x00",
        b"\x8c",  # SHORT_BINUNICODE
        b"\x8c\x03",
        b"\x8c\x03ab",
        b"\x8d",  # BINUNICODE8
        b"\x8d\x03\x00\x00\x00\x00\x00\x00",
        b"\x8d\x03\x00\x00\x00\x00\x00\x00\x00",
        b"\x8d\x03\x00\x00\x00\x00\x00\x00\x00ab",
        b"\x8e",  # BINBYTES8
        b"\x8e\x03\x00\x00\x00\x00\x00\x00",
        b"\x8e\x03\x00\x00\x00\x00\x00\x00\x00",
        b"\x8e\x03\x00\x00\x00\x00\x00\x00\x00ab",
        b"\x96",  # BYTEARRAY8
        b"\x96\x03\x00\x00\x00\x00\x00\x00",
        b"\x96\x03\x00\x00\x00\x00\x00\x00\x00",
        b"\x96\x03\x00\x00\x00\x00\x00\x00\x00ab",
        b"\x95",  # FRAME
        b"\x95\x02\x00\x00\x00\x00\x00\x00",
        b"\x95\x02\x00\x00\x00\x00\x00\x00\x00",
        b"\x95\x02\x00\x00\x00\x00\x00\x00\x00N",
    ],
)
def test_truncated_data(enc):
    with pytest.raises(quickle.DecodingError):
        quickle.loads(enc)


class MyStruct(quickle.Struct):
    x: object
    y: object


class MyStruct2(quickle.Struct):
    x: object
    y: object = 1
    z: object = []
    z2: object = 3


class MyStruct3(quickle.Struct):
    x: object
    y: object
    z: object


def test_pickler_unpickler_registry_kwarg_errors():
    with pytest.raises(TypeError, match="registry must be a list or a dict"):
        quickle.Encoder(registry="bad")

    with pytest.raises(TypeError, match="an integer is required"):
        quickle.Encoder(registry={MyStruct: 1.0})

    with pytest.raises(ValueError, match="registry values must be between"):
        quickle.Encoder(registry={MyStruct: -1})

    with pytest.raises(TypeError, match="registry must be a list or a dict"):
        quickle.Decoder(registry="bad")


@pytest.mark.parametrize("registry_type", ["list", "dict"])
@pytest.mark.parametrize("use_functions", [True, False])
def test_pickle_struct(registry_type, use_functions):
    if registry_type == "list":
        p_registry = u_registry = [MyStruct]
    else:
        p_registry = {MyStruct: 0}
        u_registry = {0: MyStruct}

    x = MyStruct(1, 2)

    if use_functions:
        s = quickle.dumps(x, registry=p_registry)
        x2 = quickle.loads(s, registry=u_registry)
    else:
        enc = quickle.Encoder(registry=p_registry)
        dec = quickle.Decoder(registry=u_registry)
        s = enc.dumps(x)
        x2 = dec.loads(s)

    assert x == x2


@pytest.mark.parametrize("code", [0, 2 ** 8 - 1, 2 ** 16 - 1, 2 ** 32 - 1])
def test_pickle_struct_codes(code):
    x = MyStruct(1, 2)

    p_registry = {MyStruct: code}
    u_registry = {code: MyStruct}

    s = quickle.dumps(x, registry=p_registry)
    x2 = quickle.loads(s, registry=u_registry)

    assert x2 == x


def test_pickle_struct_code_out_of_range():
    x = MyStruct(1, 2)
    with pytest.raises(ValueError, match="registry values must be between"):
        quickle.dumps(x, registry={MyStruct: 2 ** 32})


def test_pickle_struct_recursive():
    x = MyStruct(1, None)
    x.y = x
    s = quickle.dumps(x, registry=[MyStruct])
    x2 = quickle.loads(s, registry=[MyStruct])
    assert x2.x == 1
    assert x2.y is x2
    assert type(x) is MyStruct


@pytest.mark.parametrize("registry", ["missing", None, [], {}, {1: MyStruct}])
def test_pickle_errors_struct_missing_from_registry(registry):
    x = MyStruct(1, 2)
    s = quickle.dumps(x, registry=[MyStruct])
    kwargs = {} if registry == "missing" else {"registry": registry}
    with pytest.raises(ValueError, match="Typecode"):
        quickle.loads(s, **kwargs)


@pytest.mark.parametrize(
    "registry", ["missing", None, [], [MyStruct2], {}, {MyStruct2: 0}]
)
def test_unpickle_errors_struct_typecode_missing_from_registry(registry):
    kwargs = {} if registry == "missing" else {"registry": registry}
    x = MyStruct(1, 2)
    with pytest.raises(TypeError, match="Type MyStruct isn't in type registry"):
        quickle.dumps(x, **kwargs)


def test_unpickle_errors_obj_in_registry_is_not_struct_type():
    class Foo(object):
        pass

    x = MyStruct(1, 2)
    s = quickle.dumps(x, registry=[MyStruct])
    with pytest.raises(TypeError, match="Value for typecode"):
        quickle.loads(s, registry=[Foo])


def test_unpickle_errors_buildstruct_on_non_struct_object():
    s = b"\x80\x05K\x00\x94(K\x01K\x02\xb0."
    with pytest.raises(quickle.DecodingError, match="BUILDSTRUCT"):
        quickle.loads(s, registry=[MyStruct])


def test_struct_registry_mismatch_fewer_args_no_defaults_errors():
    x = MyStruct(1, 2)
    s = quickle.dumps(x, registry=[MyStruct])
    with pytest.raises(TypeError, match="Missing required argument 'z'"):
        quickle.loads(s, registry=[MyStruct3])


def test_struct_registry_mismatch_fewer_args_default_parameters_respected():
    """Unpickling a struct with a newer version that has additional default
    parameters at the end works (the defaults are used)."""
    x = MyStruct(1, 2)
    s = quickle.dumps(x, registry=[MyStruct])
    x2 = quickle.loads(s, registry=[MyStruct2])
    assert isinstance(x2, MyStruct2)
    assert x2.x == x.x
    assert x2.y == x.y
    assert x2.z == []
    assert x2.z2 == 3


def test_struct_registry_mismatch_extra_args_are_ignored():
    """Unpickling a struct with an older version that has fewer parameters
    works (the extra args are ignored)."""
    x = MyStruct2(1, 2)
    s = quickle.dumps(x, registry=[MyStruct2])
    x2 = quickle.loads(s, registry=[MyStruct])
    assert x2.x == 1
    assert x2.y == 2


class Fruit(enum.IntEnum):
    APPLE = 1
    BANANA = 2
    ORANGE = 3


class PyObjects(enum.Enum):
    LIST = []
    STRING = ""
    OBJECT = object()


@pytest.mark.parametrize("x", list(Fruit))
def test_pickle_intenum(x):
    s = quickle.dumps(x, registry=[Fruit])
    x2 = quickle.loads(s, registry=[Fruit])
    assert x2 == x


@pytest.mark.parametrize("x", list(PyObjects))
def test_pickle_enum(x):
    s = quickle.dumps(x, registry=[PyObjects])
    assert x.name.encode() in s
    x2 = quickle.loads(s, registry=[PyObjects])
    assert x2 == x


@pytest.mark.parametrize("code", [0, 2 ** 8 - 1, 2 ** 16 - 1, 2 ** 32 - 1])
def test_pickle_enum_codes(code):
    p_registry = {Fruit: code}
    u_registry = {code: Fruit}

    s = quickle.dumps(Fruit.APPLE, registry=p_registry)
    x2 = quickle.loads(s, registry=u_registry)

    assert x2 == Fruit.APPLE


def test_pickle_enum_code_out_of_range():
    class Fruit(enum.IntEnum):
        APPLE = 1

    with pytest.raises(ValueError, match="registry values must be between"):
        quickle.dumps(Fruit.APPLE, registry={Fruit: 2 ** 32})


@pytest.mark.parametrize("registry", [None, [], {1: PyObjects}])
def test_pickle_errors_enum_missing_from_registry(registry):
    s = quickle.dumps(Fruit.APPLE, registry=[Fruit])
    with pytest.raises(ValueError, match="Typecode"):
        quickle.loads(s, registry=registry)


@pytest.mark.parametrize("registry", [None, [PyObjects], {PyObjects: 1}])
def test_unpickle_errors_enum_typecode_missing_from_registry(registry):
    with pytest.raises(TypeError, match="Type Fruit isn't in type registry"):
        quickle.dumps(Fruit.APPLE, registry=registry)


def test_unpickle_errors_obj_in_registry_is_not_enum_type():
    s = quickle.dumps(Fruit.APPLE, registry=[Fruit])
    with pytest.raises(TypeError, match="Value for typecode"):
        quickle.loads(s, registry=[MyStruct])


def test_unpickle_errors_intenum_missing_value():
    class Fruit2(enum.IntEnum):
        APPLE = 1

    s = quickle.dumps(Fruit.ORANGE, registry=[Fruit])
    with pytest.raises(ValueError, match="Fruit2"):
        quickle.loads(s, registry=[Fruit2])


def test_unpickle_errors_enum_missing_attribute():
    class PyObjects2(enum.Enum):
        LIST = []

    s = quickle.dumps(PyObjects.OBJECT, registry=[PyObjects])
    with pytest.raises(AttributeError, match="OBJECT"):
        quickle.loads(s, registry=[PyObjects2])


@pytest.mark.parametrize("x", [0j, 1j, 1 + 0j, 1 + 1j, 1e-9 - 2.5e9j])
def test_pickle_complex(x):
    s = quickle.dumps(x)
    x2 = quickle.loads(s)
    assert x == x2


def test_objects_with_only_one_refcount_arent_memoized():
    class Test(quickle.Struct):
        x: list
        y: str

    def rstr():
        return str(uuid.uuid4().hex)

    data = [
        (rstr(),),
        (rstr(), rstr(), rstr(), rstr(), rstr()),
        ([[[rstr()]]],),
        [rstr()],
        {rstr()},
        frozenset([rstr()]),
        {rstr(): rstr()},
        rstr(),
        rstr().encode(),
        bytearray(rstr().encode()),
        Test([rstr()], rstr()),
    ]

    s = quickle.dumps(data, registry=[Test])
    # only initial arg is memoized, since its refcnt is 2
    assert s.count(pickle.MEMOIZE) == 1

    # Grab a reference to a tuple containing only non-container types
    a = data[1]
    s = quickle.dumps(data, registry=[Test])
    # 2 memoize codes, 1 for data and 1 for the tuple
    assert s.count(pickle.MEMOIZE) == 2
    del a

    # Grab a reference to a tuple containing container types
    a = data[2]
    s = quickle.dumps(data, registry=[Test])
    # 5 memoize codes, 1 for data and 1 for the tuple, 1 for each list
    assert s.count(pickle.MEMOIZE) == 5
    del a
