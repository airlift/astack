#!/bin/bash

set -eu

$JAVA_HOME/bin/java \
   -XX:+PrintGCApplicationStoppedTime \
   -agentpath:$PWD/libastack.so=port=2000 \
   -cp $PWD AStackTest 3 &

echo "Waiting..."
sleep 1
TEST=/dev/tcp/localhost/2000

echo "Testing..."

grep -q '"main" prio=5' < $TEST
grep -q 'java.lang.Thread.Stage: TIMED_WAITING (sleeping)' < $TEST
grep -q 'at AStackTest.main(AStackTest.java:20)' < $TEST

wait
