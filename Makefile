LDLIBS  ?= -ljack
SRC      = $(wildcard *.c)

ndtrig   : $(SRC)

clean    :
	@rm -rf ndtrig

.PHONY: clean
