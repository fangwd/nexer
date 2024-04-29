Nexer is a TCP traffic forwarder capable of automatically starting "preamble" processes (such as ssh tunneling).

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
