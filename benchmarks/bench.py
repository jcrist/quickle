import random
import string
import timeit
from typing import List, Optional, NamedTuple

import gc
import msgpack
import orjson
import pickle
import quickle
import proto_bench


# Define struct schemas for use with Quickle
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


# Define named tuple schemas for use with pickle
class AddressTuple(NamedTuple):
    street: str
    state: str
    zip: int


class PersonTuple(NamedTuple):
    first: str
    last: str
    age: int
    addresses: Optional[List[Address]] = None
    telephone: Optional[str] = None
    email: Optional[str] = None


states = [
    "AL",
    "AK",
    "AZ",
    "AR",
    "CA",
    "CO",
    "CT",
    "DE",
    "FL",
    "GA",
    "HI",
    "ID",
    "IL",
    "IN",
    "IA",
    "KS",
    "KY",
    "LA",
    "ME",
    "MD",
    "MA",
    "MI",
    "MN",
    "MS",
    "MO",
    "MT",
    "NE",
    "NV",
    "NH",
    "NJ",
    "NM",
    "NY",
    "NC",
    "ND",
    "OH",
    "OK",
    "OR",
    "PA",
    "RI",
    "SC",
    "SD",
    "TN",
    "TX",
    "UT",
    "VT",
    "VA",
    "WA",
    "WV",
    "WI",
    "WY",
]


def randstr(random, min=None, max=None):
    if max is not None:
        min = random.randint(min, max)
    return "".join(random.choices(string.ascii_letters, k=min))


def make_person(rand=random):
    n_addresses = rand.choice([0, 1, 1, 2, 2, 3])
    has_phone = rand.choice([True, True, False])
    has_email = rand.choice([True, True, False])

    addresses = [
        {
            "street": randstr(rand, 10, 40),
            "state": rand.choice(states),
            "zip": rand.randint(10000, 99999),
        }
        for _ in range(n_addresses)
    ]

    return {
        "first": randstr(rand, 3, 15),
        "last": randstr(rand, 3, 15),
        "age": rand.randint(0, 99),
        "addresses": addresses if addresses else None,
        "telephone": randstr(rand, 9) if has_phone else None,
        "email": randstr(rand, 15, 30) if has_email else None,
    }


def make_people(n, seed=42):
    rand = random.Random(seed)
    return [make_person(rand) for _ in range(n)]


def do_timeit(func, data):
    gc.collect()
    timer = timeit.Timer("func(data)", globals={"func": func, "data": data})
    n, t = timer.autorange()
    return t / n


def bench(dumps, loads, data):
    dumps_time = do_timeit(dumps, data)
    loads_time = do_timeit(loads, dumps(data))
    msg_size = len(dumps(data))
    return dumps_time, loads_time, msg_size


def bench_msgpack(data):
    packer = msgpack.Packer()
    return bench(packer.pack, msgpack.loads, data)


def bench_orjson(data):
    return bench(orjson.dumps, orjson.loads, data)


def bench_pyrobuf(data):
    def convert(addresses=None, email=None, telephone=None, **kwargs):
        p = proto_bench.Person()
        p.ParseFromDict(kwargs)
        if addresses:
            for a in addresses:
                p.addresses.append(proto_bench.Address(**a))
        if telephone:
            p.telephone = telephone
        if email:
            p.email = email
        return p

    if isinstance(data, list):
        data = proto_bench.People(people=[convert(**d) for d in data])
        loads = proto_bench.People.FromString
    else:
        data = convert(**data)
        loads = proto_bench.Person.FromString

    def dumps(p):
        return p.SerializeToString()

    return bench(dumps, loads, data)


def bench_pickle(data):
    return bench(pickle.dumps, pickle.loads, data)


def bench_pickle_namedtuple(data):
    def convert(addresses=None, **kwargs):
        addrs = [AddressTuple(**a) for a in addresses] if addresses else None
        return PersonTuple(addresses=addrs, **kwargs)

    data = [convert(**d) for d in data] if isinstance(data, list) else convert(**data)
    return bench(pickle.dumps, pickle.loads, data)


def bench_quickle(data):
    enc = quickle.Encoder()
    dec = quickle.Decoder()
    return bench(enc.dumps, dec.loads, data)


def bench_quickle_structs(data):
    enc = quickle.Encoder(registry=[Person, Address], memoize=False)
    dec = quickle.Decoder(registry=[Person, Address])

    def convert(addresses=None, **kwargs):
        addrs = [Address(**a) for a in addresses] if addresses else None
        return Person(addresses=addrs, **kwargs)

    data = [convert(**d) for d in data] if isinstance(data, list) else convert(**data)
    return bench(enc.dumps, dec.loads, data)


BENCHMARKS = [
    ("orjson", bench_orjson),
    ("msgpack", bench_msgpack),
    ("pyrobuf", bench_pyrobuf),
    ("pickle", bench_pickle),
    ("pickle tuples", bench_pickle_namedtuple),
    ("quickle", bench_quickle),
    ("quickle structs", bench_quickle_structs),
]


def format_time(n):
    if n >= 1:
        return "%.2f s" % n
    if n >= 1e-3:
        return "%.2f ms" % (n * 1e3)
    return "%.2f us" % (n * 1e6)


def format_bytes(n):
    if n >= 2 ** 30:
        return "%.1f GiB" % (n / (2 ** 30))
    elif n >= 2 ** 20:
        return "%.1f MiB" % (n / (2 ** 20))
    elif n >= 2 ** 10:
        return "%.1f KiB" % (n / (2 ** 10))
    return "%s B" % n


def preprocess_results(results):
    data = dict(zip(["benchmark", "dumps", "loads", "size"], map(list, zip(*results))))
    data["total"] = [d + l for d, l in zip(data["dumps"], data["loads"])]

    max_time = max(data["total"])
    if max_time < 1e-6:
        time_unit = "ns"
        scale = 1e9
    elif max_time < 1e-3:
        time_unit = "us"
        scale = 1e6
    else:
        time_unit = "ms"
        scale = 1e3

    for k in ["dumps", "loads", "total"]:
        data[f"{k}_labels"] = [format_time(t) for t in data[k]]
        data[k] = [scale * t for t in data[k]]

    max_size = max(data["size"])
    if max_size < 1e3:
        size_unit = "B"
        scale = 1
    elif max_size < 1e6:
        size_unit = "KiB"
        scale = 1e3
    elif max_size < 1e9:
        size_unit = "MiB"
        scale = 1e6

    data["size_labels"] = [format_bytes(s) for s in data["size"]]
    data["size"] = [s / scale for s in data["size"]]

    return data, time_unit, size_unit


def make_plot(results, title):
    import json
    import bokeh.plotting as bp
    from bokeh.transform import dodge
    from bokeh.layouts import column, row
    from bokeh.models import CustomJS, RadioGroup, FactorRange

    data, time_unit, size_unit = preprocess_results(results)

    sort_options = ["total", "dumps", "loads", "size"]
    sort_orders = [
        list(zip(*sorted(zip(data[order], data["benchmark"]), reverse=True)))[1]
        for order in sort_options
    ]

    source = bp.ColumnDataSource(data=data)
    tooltips = [("time", "@$name")]

    x_range = FactorRange(*sort_orders[0])

    p = bp.figure(
        x_range=x_range,
        plot_height=250,
        plot_width=660,
        title=title,
        toolbar_location=None,
        tools="",
        tooltips=tooltips,
    )

    p.vbar(
        x=dodge("benchmark", -0.25, range=p.x_range),
        top="dumps",
        width=0.2,
        source=source,
        color="#c9d9d3",
        legend_label="dumps",
        name="dumps_labels",
    )
    p.vbar(
        x=dodge("benchmark", 0.0, range=p.x_range),
        top="loads",
        width=0.2,
        source=source,
        color="#718dbf",
        legend_label="loads",
        name="loads_labels",
    )
    p.vbar(
        x=dodge("benchmark", 0.25, range=p.x_range),
        top="total",
        width=0.2,
        source=source,
        color="#e84d60",
        legend_label="total",
        name="total_labels",
    )

    p.x_range.range_padding = 0.1
    p.xaxis.visible = False
    p.xgrid.grid_line_color = None
    p.ygrid.grid_line_color = None
    p.yaxis.axis_label = f"Time ({time_unit})"
    p.yaxis.minor_tick_line_color = None
    p.legend.location = "top_right"
    p.legend.orientation = "horizontal"
    tooltips = [("size", "@size_labels")]

    size_plot = bp.figure(
        x_range=x_range,
        plot_height=150,
        plot_width=660,
        title=None,
        toolbar_location=None,
        tools="hover",
        tooltips=tooltips,
    )
    size_plot.vbar(x="benchmark", top="size", width=0.9, source=source)

    size_plot.y_range.start = 0
    size_plot.yaxis.axis_label = f"Size ({size_unit})"
    size_plot.yaxis.minor_tick_line_color = None
    size_plot.x_range.range_padding = 0.1
    size_plot.xgrid.grid_line_color = None
    size_plot.ygrid.grid_line_color = None

    # Setup widget
    select = RadioGroup(labels=sort_options, active=0)
    callback = CustomJS(
        args=dict(x_range=x_range),
        code="""
        var lookup = {lookup_table};
        x_range.factors = lookup[this.active];
        x_range.change.emit();
        """.format(
            lookup_table=json.dumps(sort_orders)
        ),
    )
    select.js_on_click(callback)
    out = row(column(p, size_plot), select)
    return out


def run(data, plot_title, plot_name, save_plot=False, save_json=False):
    results = []
    for name, func in BENCHMARKS:
        print(f"- {name}...")
        dumps_time, loads_time, msg_size = func(data)
        print(f"  dumps: {dumps_time * 1e6:.2f} us")
        print(f"  loads: {loads_time * 1e6:.2f} us")
        print(f"  size: {msg_size} bytes")
        results.append((name, dumps_time, loads_time, msg_size))
    if save_plot or save_json:
        import json
        from bokeh.resources import CDN
        from bokeh.embed import file_html, json_item

        plot = make_plot(results, plot_title)
        if save_plot:
            with open(f"{plot_name}.html", "w") as f:
                html = file_html(plot, CDN, "Benchmarks")
                f.write(html)
        if save_json:
            with open(f"{plot_name}.json", "w") as f:
                data = json.dumps(json_item(plot))
                f.write(data)


def run_1(save_plot=False, save_json=False):
    print("Benchmark - 1 object")
    data = make_people(1)[0]
    data["addresses"] = None
    run(data, "Benchmark - 1 object", "bench-1", save_plot, save_json)


def run_1k(save_plot=False, save_json=False):
    print("Benchmark - 1k objects")
    data = make_people(1000)
    run(data, "Benchmark - 1000 objects", "bench-1k", save_plot, save_json)


def run_10k(save_plot=False, save_json=False):
    print("Benchmark - 10k objects")
    data = make_people(10000)
    run(data, "Benchmark - 10,000 objects", "bench-10k", save_plot, save_json)


def run_all(save_plot=False, save_json=False):
    for runner in [run_1, run_1k, run_10k]:
        runner(save_plot, save_json)


benchmarks = {"all": run_all, "1": run_1, "1k": run_1k, "10k": run_10k}


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Benchmark different python serializers"
    )
    parser.add_argument(
        "--benchmark",
        default="all",
        choices=list(benchmarks),
        help="which benchmark to run, defaults to 'all'",
    )
    parser.add_argument(
        "--plot",
        action="store_true",
        help="whether to plot the results",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="whether to output json representations of each plot",
    )
    args = parser.parse_args()
    benchmarks[args.benchmark](args.plot, args.json)


if __name__ == "__main__":
    main()
