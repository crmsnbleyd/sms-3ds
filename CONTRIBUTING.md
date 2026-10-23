# Contributing

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` package group, plus CMake,
Ninja and Python 3. The build uses [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) (with its `decomp`
submodule) checked out in `refs/sms-pc-port` at the commit recorded in `CMakeLists.txt`:

```sh
git clone --recurse-submodules https://github.com/chasem-dev/sms-pc-port refs/sms-pc-port
git -C refs/sms-pc-port checkout --recurse-submodules e3cdcca1d3a78f657d2d195c0db084bbe28c5a45
scripts/build-3ds.sh
```

The result is `build-3ds/sms_3ds.3dsx`.
