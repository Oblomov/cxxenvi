CXXFLAGS ?= -std=c++11
CTAGS ?= ctags-exuberant

all: tags tests

tests:
	$(MAKE) -C $@

clean:
	$(MAKE) -C tests clean

tags:
	$(CTAGS) -R . tests

.PHONY: tests clean
