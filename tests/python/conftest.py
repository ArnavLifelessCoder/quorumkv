import pathlib


def pytest_configure(config):
    basetemp = config.getoption("basetemp")
    if basetemp:
        pathlib.Path(basetemp).resolve().parent.mkdir(parents=True, exist_ok=True)
