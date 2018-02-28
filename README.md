# Overview

**astack** is a [JVM TI][] agent that allows capturing Java stack traces
of all threads in a JVM, similar to [jstack], but without requiring the
JVM to reach a safepoint. This allows debugging and monitoring without
impacting the running application, which is important because reaching
a safepoint may take many seconds when the system is under heavy load.

The agent listens on a TCP port and returns a new thread dump whenever
a client connects, allowing easy remote monitoring.

[JVM TI]: https://docs.oracle.com/javase/9/docs/specs/jvmti.html
[jstack]: https://docs.oracle.com/javase/9/tools/jstack.htm

# Building

    make JAVA_HOME=/path/to/jdk

# Usage

Run Java with the agent added as a JVM argument, specifying the port
number for the agent to listen on:

    -agentpath:/path/to/libastack.so=port=2000

Alternatively, if modifying the Java command line is not possible, the
above may be added to the `JAVA_TOOL_OPTIONS` environment variable.
