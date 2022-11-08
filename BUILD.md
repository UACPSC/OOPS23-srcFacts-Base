# Build

Create a build directory and move into it:

```console
mkdir build
cd build
```

Once in your build directory run cmake:

```console
cmake ..
```

An example input file is provided. To run with make:

```console
make run
```

To run on the command line:

```console
./srcfacts < demo.xml
```

You can also time it:

```console
time ./srcfacts < demo.xml
```

Tracing is off by default. To turn tracing on:

```console
cmake .. -DTRACE=ON
```

To turn tracing back off:

```console
cmake .. -DTRACE=OFF
```
