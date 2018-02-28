ifndef JAVA_HOME
    $(error JAVA_HOME not set)
endif

INCLUDE= -I"$(JAVA_HOME)/include" -I"$(JAVA_HOME)/include/linux"
CFLAGS=-Wall -Werror -std=c++11 -fPIC -shared $(INCLUDE)

TARGET=libastack.so

.PHONY: all clean test

all:
	g++ $(CFLAGS) -o $(TARGET) astack.cpp
	chmod 644 $(TARGET)

clean:
	rm -f $(TARGET)
	rm -f *.class

test: all
	$(JAVA_HOME)/bin/javac AStackTest.java
	./test.sh
