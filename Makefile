BIN     ?= ndseq
LDLIBS  ?= -ljack
SRC      = $(wildcard *.c)

$(BIN)   : $(SRC)

clean    :
	@rm -rf $(BIN)

.PHONY   : clean
