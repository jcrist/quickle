import enum
import itertools
import pickle
import pickletools
import string
import sys
from distutils.version import StrictVersion

import pytest

import smolpickle


BATCHSIZE = 1000


def test_picklebuffer_is_shared():
    assert pickle.PickleBuffer is smolpickle.PickleBuffer


def test_exceptions_are_unique():
    assert pickle.PickleError is not smolpickle.PickleError
    assert pickle.PicklingError is not smolpickle.PicklingError
    assert pickle.UnpicklingError is not smolpickle.UnpicklingError


def test_module_version():
    StrictVersion(smolpickle.__version__)


def check(obj, sol=None):
    if sol is None:
        sol = obj

    smol_res = smolpickle.dumps(obj)
    obj2 = smolpickle.loads(smol_res)
    assert obj2 == sol
    assert type(obj2) is type(sol)

    obj3 = pickle.loads(smol_res)
    assert obj3 == sol
    assert type(obj3) is type(sol)

    pickle_res = pickle.dumps(obj, protocol=5)
    obj4 = smolpickle.loads(pickle_res)
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

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
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

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, list)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = [[None]]
    obj[0][0] = obj

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
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

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
        assert isinstance(obj2, dict)
        assert obj2[0] is obj2
        assert len(obj2) == 1

    # one level removed
    obj = {0: []}
    obj[0].append(obj)

    smol_res = smolpickle.dumps(obj)
    for loads in [smolpickle.loads, pickle.loads]:
        obj2 = loads(smol_res)
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
    for op, dummy, dummy in pickletools.genops(pickle):
        if op.code == code.decode("latin-1"):
            return True
    return False


def test_pickle_memoize_false():
    obj = [[1], [2]]
    res = smolpickle.dumps(obj, memoize=False)
    assert not opcode_in_pickle(pickle.BINPUT, res)
    obj2 = smolpickle.loads(res)
    assert obj == obj2

    obj = [[]] * 2
    res = smolpickle.dumps(obj, memoize=False)
    assert not opcode_in_pickle(pickle.BINPUT, res)
    obj2 = smolpickle.loads(res)
    assert obj == obj2
    assert obj2[0] is not obj2[1]


def test_pickle_memoize_false_recursion_error():
    obj = []
    obj.append(obj)
    with pytest.raises(RecursionError):
        smolpickle.dumps(obj, memoize=False)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_pickle_picklebuffer_no_callback(cls):
    sol = cls(b"hello")
    obj = smolpickle.PickleBuffer(sol)
    check(obj, sol)


@pytest.mark.parametrize("cls", [bytes, bytearray])
def test_dumps_buffer_callback(cls):
    sol = cls(b"hello")
    obj = smolpickle.PickleBuffer(sol)

    buffers = []
    smol_res = smolpickle.dumps(obj, buffer_callback=buffers.append)

    # roundtrip
    obj2 = smolpickle.loads(smol_res, buffers=buffers)
    assert obj2 is obj

    # smolpickle -> pickle
    obj3 = pickle.loads(smol_res, buffers=buffers)
    assert obj3 is obj

    # pickle -> smolpickle
    buffers = []
    pickle_res = pickle.dumps(obj, protocol=5, buffer_callback=buffers.append)
    obj4 = smolpickle.loads(pickle_res, buffers=buffers)
    assert obj4 is obj


def test_dumps_buffer_callback_errors():
    obj = pickle.PickleBuffer(b"hello")

    with pytest.raises(TypeError):
        smolpickle.dumps(obj, buffer_callback=object())

    with pytest.raises(ZeroDivisionError):
        smolpickle.dumps(obj, buffer_callback=lambda x: 1 / 0)


def test_loads_buffers_errors():
    obj = smolpickle.PickleBuffer(b"hello")
    data = smolpickle.dumps(obj, buffer_callback=lambda x: None)

    with pytest.raises(TypeError):
        smolpickle.loads(data, buffers=object())

    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(data, buffers=[])


@pytest.mark.parametrize("value", [object(), object, sum, itertools.count])
def test_dumps_and_loads_unpickleable_types(value):
    with pytest.raises(TypeError):
        smolpickle.dumps(value)

    o = pickle.dumps(value, protocol=5)

    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(o)


def test_loads_truncated_input():
    data = smolpickle.dumps([1, 2, 3])
    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(data[:-2])


def test_loads_bad_pickle():
    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(b"this isn't valid at all")


def test_getsizeof():
    a = sys.getsizeof(smolpickle.Pickler(buffer_size=64))
    b = sys.getsizeof(smolpickle.Pickler(buffer_size=128))
    assert b > a
    # Smoketest
    sys.getsizeof(smolpickle.Unpickler())


@pytest.mark.parametrize(
    "p",
    [
        # bad stacks
        b".",  # STOP
        b"0",  # POP
        b"1",  # POP_MARK
        b"a",  # APPEND
        b"Na",
        b"e",  # APPENDS
        b"(e",
        b"q\x00",  # BINPUT
        b"r\x00\x00\x00\x00",  # LONG_BINPUT
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
        b"N(q\x00",  # BINPUT
        b"N(r\x00\x00\x00\x00",  # LONG_BINPUT
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
def test_bad_stack_or_mark(p):
    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(p)


@pytest.mark.parametrize(
    "p",
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
        b"Nq",  # BINPUT
        b"Nr",  # LONG_BINPUT
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
def test_truncated_data(p):
    with pytest.raises(smolpickle.UnpicklingError):
        smolpickle.loads(p)


class MyStruct(smolpickle.Struct):
    x: object
    y: object


class MyStruct2(smolpickle.Struct):
    x: object
    y: object
    z: object = []


def test_pickler_unpickler_registry_kwarg_errors():
    with pytest.raises(TypeError, match="registry must be a list or a dict"):
        smolpickle.Pickler(registry="bad")

    with pytest.raises(TypeError, match="registry must be a list or a dict"):
        smolpickle.Unpickler(registry="bad")


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
        s = smolpickle.dumps(x, registry=p_registry)
        x2 = smolpickle.loads(s, registry=u_registry)
    else:
        p = smolpickle.Pickler(registry=p_registry)
        u = smolpickle.Unpickler(registry=u_registry)
        s = p.dumps(x)
        x2 = u.loads(s)

    assert x == x2


@pytest.mark.parametrize("code", [0, 2 ** 8 - 1, 2 ** 16 - 1, 2 ** 32 - 1])
def test_pickle_struct_codes(code):
    x = MyStruct(1, 2)

    p_registry = {MyStruct: code}
    u_registry = {code: MyStruct}

    s = smolpickle.dumps(x, registry=p_registry)
    x2 = smolpickle.loads(s, registry=u_registry)

    assert x2 == x


def test_pickle_struct_code_out_of_range():
    x = MyStruct(1, 2)
    with pytest.raises(ValueError, match="out of range"):
        smolpickle.dumps(x, registry={MyStruct: 2 ** 32})


def test_pickle_struct_recursive():
    x = MyStruct(1, None)
    x.y = x
    s = smolpickle.dumps(x, registry=[MyStruct])
    x2 = smolpickle.loads(s, registry=[MyStruct])
    assert x2.x == 1
    assert x2.y is x2
    assert type(x) is MyStruct


def test_pickle_struct_registry_mismatch_default_parameters_respected():
    """Unpickling a struct with a newer version that has additional default
    parameters at the end works (the defaults are used). This can be used to
    support evolving schemas, provided fields are never reordered and all new
    fields have default values"""
    x = MyStruct(1, 2)
    s = smolpickle.dumps(x, registry=[MyStruct])
    x2 = smolpickle.loads(s, registry=[MyStruct2])
    assert isinstance(x2, MyStruct2)
    assert x2.x == x.x
    assert x2.y == x.y
    assert x2.z == []


@pytest.mark.parametrize("registry", ["missing", None, [], {}, {1: MyStruct}])
def test_pickle_errors_struct_missing_from_registry(registry):
    x = MyStruct(1, 2)
    s = smolpickle.dumps(x, registry=[MyStruct])
    kwargs = {} if registry == "missing" else {"registry": registry}
    with pytest.raises(ValueError, match="Typecode"):
        smolpickle.loads(s, **kwargs)


@pytest.mark.parametrize(
    "registry", ["missing", None, [], [MyStruct2], {}, {MyStruct2: 0}]
)
def test_unpickle_errors_struct_typecode_missing_from_registry(registry):
    kwargs = {} if registry == "missing" else {"registry": registry}
    x = MyStruct(1, 2)
    with pytest.raises(TypeError, match="Type MyStruct isn't in type registry"):
        smolpickle.dumps(x, **kwargs)


def test_unpickle_errors_obj_in_registry_is_not_struct_type():
    class Foo(object):
        pass

    x = MyStruct(1, 2)
    s = smolpickle.dumps(x, registry=[MyStruct])
    with pytest.raises(TypeError, match="Value for typecode"):
        smolpickle.loads(s, registry=[Foo])


def test_unpickle_errors_buildstruct_on_non_struct_object():
    s = b"\x80\x05K\x00\x94(K\x01K\x02\xb0."
    with pytest.raises(smolpickle.UnpicklingError, match="BUILDSTRUCT"):
        smolpickle.loads(s, registry=[MyStruct])


def test_unpickle_errors_struct_registry_mismatch():
    x = MyStruct2(1, 2)
    s = smolpickle.dumps(x, registry=[MyStruct2])
    with pytest.raises(TypeError, match="Extra positional arguments provided"):
        smolpickle.loads(s, registry=[MyStruct])


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
    s = smolpickle.dumps(x, registry=[Fruit])
    x2 = smolpickle.loads(s, registry=[Fruit])
    assert x2 == x


@pytest.mark.parametrize("x", list(PyObjects))
def test_pickle_enum(x):
    s = smolpickle.dumps(x, registry=[PyObjects])
    assert x.name.encode() in s
    x2 = smolpickle.loads(s, registry=[PyObjects])
    assert x2 == x


@pytest.mark.parametrize("code", [0, 2 ** 8 - 1, 2 ** 16 - 1, 2 ** 32 - 1])
def test_pickle_enum_codes(code):
    p_registry = {Fruit: code}
    u_registry = {code: Fruit}

    s = smolpickle.dumps(Fruit.APPLE, registry=p_registry)
    x2 = smolpickle.loads(s, registry=u_registry)

    assert x2 == Fruit.APPLE


def test_pickle_enum_code_out_of_range():
    class Fruit(enum.IntEnum):
        APPLE = 1

    with pytest.raises(ValueError, match="out of range"):
        smolpickle.dumps(Fruit.APPLE, registry={Fruit: 2 ** 32})


@pytest.mark.parametrize("registry", [None, [], {1: PyObjects}])
def test_pickle_errors_enum_missing_from_registry(registry):
    s = smolpickle.dumps(Fruit.APPLE, registry=[Fruit])
    with pytest.raises(ValueError, match="Typecode"):
        smolpickle.loads(s, registry=registry)


@pytest.mark.parametrize("registry", [None, [PyObjects], {PyObjects: 1}])
def test_unpickle_errors_enum_typecode_missing_from_registry(registry):
    with pytest.raises(TypeError, match="Type Fruit isn't in type registry"):
        smolpickle.dumps(Fruit.APPLE, registry=registry)


def test_unpickle_errors_obj_in_registry_is_not_enum_type():
    s = smolpickle.dumps(Fruit.APPLE, registry=[Fruit])
    with pytest.raises(TypeError, match="Value for typecode"):
        smolpickle.loads(s, registry=[MyStruct])


def test_unpickle_errors_intenum_missing_value():
    class Fruit2(enum.IntEnum):
        APPLE = 1

    s = smolpickle.dumps(Fruit.ORANGE, registry=[Fruit])
    with pytest.raises(ValueError, match="Fruit2"):
        smolpickle.loads(s, registry=[Fruit2])


def test_unpickle_errors_enum_missing_attribute():
    class PyObjects2(enum.Enum):
        LIST = []

    s = smolpickle.dumps(PyObjects.OBJECT, registry=[PyObjects])
    with pytest.raises(AttributeError, match="OBJECT"):
        smolpickle.loads(s, registry=[PyObjects2])


@pytest.mark.parametrize("x", [0j, 1j, 1 + 0j, 1 + 1j, 1e-9 - 2.5e9j])
def test_pickle_complex(x):
    s = smolpickle.dumps(x)
    x2 = smolpickle.loads(s)
    assert x == x2
