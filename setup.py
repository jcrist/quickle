import os

from setuptools import setup, find_packages
from setuptools.extension import Extension

ext_modules = [Extension("smolpickle", ["smolpickle.c"])]

setup(
    name="smolpickle",
    version="0.0.1",
    maintainer="Jim Crist-Harif",
    maintainer_email="jcristharif@gmail.com",
    url="https://github.com/jcrist/smolpickle",
    project_urls={
        "Source": "https://github.com/jcrist/smolpickle/",
        "Issue Tracker": "https://github.com/jcrist/smolpickle/issues",
    },
    description="Like pickle, but smol",
    classifiers=[
        "License :: OSI Approved :: BSD License",
        "Programming Language :: Python :: 3.8",
    ],
    license="BSD",
    packages=find_packages(),
    ext_modules=ext_modules,
    long_description=(
        open("README.rst").read() if os.path.exists("README.rst") else ""
    ),
    python_requires=">=3.8",
    zip_safe=False,
)
