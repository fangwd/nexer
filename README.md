Nexer is a TCP traffic forward capable of automatically starting "preamble" processes (such as ssh).

This is still work in progress.

To build, run
```
cmake -B build . && cmake --build build
```

To start:
```
./build/nexer
```

When nexer starts it looks for $HOME/.nexer/nexer.conf.

See example/nexer.conf for an example config file.
